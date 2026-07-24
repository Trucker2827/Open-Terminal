// WebTools.cpp — web_search (DuckDuckGo + headless Chromium) and web_fetch MCP tools.
// GUI-tier: depends on HeadlessBrowser (QtWebEngine), registered in McpInitGui.cpp.
//
// Handler threading: HeadlessBrowser::fetch() is blocking and designed to be called from
// a worker thread — it hops the page load to the GUI thread internally. The MCP provider
// dispatches async_handler on a worker thread, so no additional marshaling is needed here.

#include "mcp/tools/WebTools.h"
#include "storage/repositories/SettingsRepository.h"
#include "web/HeadlessBrowser.h"
#include "web/WebSearchParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

namespace openmarketterminal::mcp::tools {

std::vector<ToolDef> get_web_tools() {
    std::vector<ToolDef> tools;

    // ── web_search ──────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "web_search";
        t.description =
            "Search the web for current information and return the top results "
            "(title, url, snippet). Engine chain: Google Programmable Search when "
            "connectors.google_cse_key/_cx are set in Settings, else DuckDuckGo, "
            "falling back to Mojeek when an engine serves a bot challenge. Use for "
            "live/general facts not covered by the finance data tools: recent events, "
            "general news, definitions, company background. "
            "Args: query (string, required), max_results (int, default 5, max 20).";
        t.category = "web";
        t.input_schema.properties = QJsonObject{
            {"query",       QJsonObject{{"type", "string"},  {"description", "Search query"}}},
            {"max_results", QJsonObject{{"type", "integer"}, {"description", "Max results to return (default 5, max 20)"}}},
        };
        t.input_schema.required = QStringList{"query"};
        t.auth_required = AuthLevel::None;
        // The chain may try up to three engines at ~9s each.
        t.default_timeout_ms = 30000;
        t.async_handler = [](const QJsonObject& args, ToolContext ctx,
                              std::shared_ptr<QPromise<ToolResult>> promise) {
            const QString q = args.value("query").toString().trimmed();
            // Fetch a few extra so filtering ads doesn't under-deliver.
            const int requested = std::clamp(args.value("max_results").toInt(5), 1, 20);
            const int fetch_n   = std::min(requested + 5, 25);

            if (q.isEmpty()) {
                promise->addResult(ToolResult::fail("web_search: query is required"));
                promise->finish();
                return;
            }
            if (ctx.cancelled()) {
                promise->addResult(ToolResult::fail("cancelled"));
                promise->finish();
                return;
            }

            // Engine chain: Google Programmable Search (if the user configured
            // an API key — the only reliable Google path; scraping google.com
            // hits bot walls immediately) → DuckDuckGo html endpoint →
            // Mojeek (keyless, scraper-tolerant). DDG started serving an
            // "anomaly" anti-bot challenge to automated traffic in 2026 —
            // detect it and fall through instead of reporting no results.
            const QString js = "document.documentElement.outerHTML";
            QVector<web::WebResult> rows;
            QStringList engines_tried;

            const auto key_result = openmarketterminal::SettingsRepository::instance().get(
                QStringLiteral("connectors.google_cse_key"));
            const auto cx_result = openmarketterminal::SettingsRepository::instance().get(
                QStringLiteral("connectors.google_cse_cx"));
            const QString cse_key = key_result.is_ok() ? key_result.value().trimmed() : QString();
            const QString cse_cx = cx_result.is_ok() ? cx_result.value().trimmed() : QString();
            if (!cse_key.isEmpty() && !cse_cx.isEmpty()) {
                engines_tried << QStringLiteral("google");
                QUrl gurl("https://www.googleapis.com/customsearch/v1");
                QUrlQuery gq;
                gq.addQueryItem("key", cse_key);
                gq.addQueryItem("cx", cse_cx);
                gq.addQueryItem("q", q);
                gq.addQueryItem("num", QString::number(std::min(fetch_n, 10)));
                gurl.setQuery(gq);
                // Chromium renders raw JSON inside a <pre> — innerText restores it.
                const QString body = web::HeadlessBrowser::instance().fetch(
                    gurl, QStringLiteral("document.body.innerText"), 9000);
                const auto doc = QJsonDocument::fromJson(body.toUtf8());
                for (const auto& item : doc.object().value(QStringLiteral("items")).toArray()) {
                    const auto o = item.toObject();
                    rows.push_back({o.value(QStringLiteral("title")).toString(),
                                    o.value(QStringLiteral("link")).toString(),
                                    o.value(QStringLiteral("snippet")).toString()});
                }
            }

            if (rows.isEmpty()) {
                engines_tried << QStringLiteral("duckduckgo");
                QUrl url("https://html.duckduckgo.com/html/");
                QUrlQuery qq;
                qq.addQueryItem("q", q);
                url.setQuery(qq);
                const QString html = web::HeadlessBrowser::instance().fetch(url, js, 9000);
                if (promise->future().isFinished()) return; // provider watchdog already fired
                if (!html.isEmpty() && !web::looks_like_bot_challenge(html))
                    rows = web::parse_ddg_results(html, fetch_n);
            }

            if (rows.isEmpty()) {
                engines_tried << QStringLiteral("mojeek");
                QUrl murl("https://www.mojeek.com/search");
                QUrlQuery mq;
                mq.addQueryItem("q", q);
                murl.setQuery(mq);
                const QString html = web::HeadlessBrowser::instance().fetch(murl, js, 9000);
                if (promise->future().isFinished()) return;
                if (!html.isEmpty() && !web::looks_like_bot_challenge(html))
                    rows = web::parse_mojeek_results(html, fetch_n);
            }

            if (rows.isEmpty()) {
                promise->addResult(ToolResult::fail(
                    QStringLiteral("web_search: no engine returned results (tried %1) — "
                                   "pages blocked, empty, or timed out")
                        .arg(engines_tried.join(QStringLiteral(", ")))));
                promise->finish();
                return;
            }

            // Filter out DuckDuckGo ad/tracker links (host == *.duckduckgo.com).
            QJsonArray arr;
            for (const auto& r : rows) {
                if (arr.size() >= requested) break;
                const QString host = QUrl(r.url).host();
                if (host.endsWith("duckduckgo.com")) continue;
                arr.append(QJsonObject{
                    {"title",   r.title},
                    {"url",     r.url},
                    {"snippet", r.snippet},
                });
            }

            if (arr.isEmpty()) {
                promise->addResult(ToolResult::fail("web_search: no results found (all filtered or empty page)"));
                promise->finish();
                return;
            }

            promise->addResult(ToolResult::ok_data(QJsonObject{{"results", arr}}));
            promise->finish();
        };
        tools.push_back(std::move(t));
    }

    // ── web_fetch ───────────────────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "web_fetch";
        t.description =
            "Fetch a web page (headless Chromium, executes JS) and return its readable text. "
            "Use after web_search to read a full article, or when given a specific URL. "
            "Strips nav/header/footer/scripts for clean text. "
            "Args: url (string, required, must be http or https), max_chars (int, default 6000).";
        t.category = "web";
        t.input_schema.properties = QJsonObject{
            {"url",       QJsonObject{{"type", "string"},  {"description", "Absolute http(s) URL to fetch"}}},
            {"max_chars", QJsonObject{{"type", "integer"}, {"description", "Truncate returned text to this many chars (default 6000)"}}},
        };
        t.input_schema.required = QStringList{"url"};
        t.auth_required = AuthLevel::None;
        t.default_timeout_ms = 20000;
        t.async_handler = [](const QJsonObject& args, ToolContext ctx,
                              std::shared_ptr<QPromise<ToolResult>> promise) {
            const QUrl url(args.value("url").toString().trimmed());
            const int   cap = std::max(1, args.value("max_chars").toInt(6000));

            if (!url.isValid() ||
                (url.scheme() != "http" && url.scheme() != "https")) {
                promise->addResult(ToolResult::fail(
                    "web_fetch: a valid http(s) url is required"));
                promise->finish();
                return;
            }
            if (ctx.cancelled()) {
                promise->addResult(ToolResult::fail("cancelled"));
                promise->finish();
                return;
            }

            // Strip noise elements, then extract title + body text as JSON.
            const QString js =
                "(function(){"
                "var t=document.title;"
                "document.querySelectorAll('script,style,nav,header,footer,noscript,aside,iframe').forEach(function(e){e.remove();});"
                "var body=document.body?document.body.innerText:'';"
                "return JSON.stringify({title:t,text:body});"
                "})()";

            const QString raw = web::HeadlessBrowser::instance().fetch(url, js, 15000);

            if (promise->future().isFinished()) return;

            if (raw.isEmpty()) {
                promise->addResult(ToolResult::fail("web_fetch: page load failed or timed out"));
                promise->finish();
                return;
            }

            QJsonParseError perr;
            const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &perr);
            if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
                promise->addResult(ToolResult::fail("web_fetch: could not parse page content"));
                promise->finish();
                return;
            }
            const QJsonObject parsed = doc.object();
            QString text = parsed.value("text").toString();
            if (text.size() > cap)
                text = text.left(cap) + QChar(0x2026); // …

            promise->addResult(ToolResult::ok_data(QJsonObject{
                {"title", parsed.value("title").toString()},
                {"url",   url.toString()},
                {"text",  text},
            }));
            promise->finish();
        };
        tools.push_back(std::move(t));
    }

    return tools;
}

} // namespace openmarketterminal::mcp::tools
