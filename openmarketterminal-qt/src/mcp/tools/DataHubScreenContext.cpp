// DataHubScreenContext.cpp — GUI-coupled screen-context helpers + the
// `get_terminal_context` MCP tool. Split out of DataHubPeekHelpers.cpp so that
// the Core peek helpers compile Qt6::Core-only and register_core_tools() links
// without QtWidgets. This TU keeps the QtWidgets/WindowFrame dependencies and is
// registered exclusively via register_gui_tools() (see McpInitGui.cpp).

#include "mcp/tools/DataHubScreenContext.h"

#include "app/DockScreenRouter.h"
#include "app/WindowFrame.h"
#include "core/logging/Logger.h"
#include "datahub/DataHub.h"
#include "mcp/tools/DataHubPeekHelpers.h"
#include "mcp/tools/ThreadHelper.h"

#include <QApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QWidget>

namespace openmarketterminal::mcp::tools {

static constexpr const char* TAG = "DataHubScreenContext";

namespace detail {

namespace {

constexpr int kMaxContextQuotes = 12;
constexpr qint64 kMaxQuoteAgeMs = 5 * 60 * 1000;

openmarketterminal::DockScreenRouter* find_active_router_main_thread() {
    if (auto* active = QApplication::activeWindow()) {
        if (auto* mw = qobject_cast<openmarketterminal::WindowFrame*>(active))
            return mw->dock_router();
    }
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (auto* mw = qobject_cast<openmarketterminal::WindowFrame*>(w)) {
            if (mw->window_id() == 0)
                return mw->dock_router();
        }
    }
    return nullptr;
}

} // namespace

QString build_screen_context_brief() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList lines;
    int count = 0;

    for (const auto& s : openmarketterminal::datahub::DataHub::instance().stats()) {
        if (!s.topic.startsWith(QStringLiteral("market:quote:")))
            continue;
        if (s.subscriber_count <= 0 || s.last_publish_ms <= 0)
            continue;
        if ((now - s.last_publish_ms) > kMaxQuoteAgeMs)
            continue;

        const QString sym = s.topic.mid(QStringLiteral("market:quote:").size());
        const auto q = peek_quote(sym);
        if (!q)
            continue;

        lines << QStringLiteral("%1 $%2 (%3%)")
                     .arg(q->symbol)
                     .arg(q->price, 0, 'f', 2)
                     .arg(q->change_pct, 0, 'f', 2);
        if (++count >= kMaxContextQuotes)
            break;
    }

    if (lines.isEmpty())
        return {};

    return QStringLiteral("[Screen context — live quotes streaming on your terminal]\n") + lines.join('\n');
}

QJsonObject build_terminal_context_json() {
    QJsonObject out;
    QString current_id;
    QString current_title;

    if (qApp) {
        detail::run_on_target_thread_sync(qApp, [&]() {
            if (auto* router = find_active_router_main_thread()) {
                current_id = router->current_screen_id();
                if (!current_id.isEmpty())
                    current_title = openmarketterminal::DockScreenRouter::title_for_id(current_id);
            }
        });
    }

    out["current_screen"] =
        current_id.isEmpty()
            ? QJsonValue(QJsonValue::Null)
            : QJsonValue(QJsonObject{{"id", current_id}, {"title", current_title}});

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QJsonArray quotes;
    QJsonArray topics;

    for (const auto& s : openmarketterminal::datahub::DataHub::instance().stats()) {
        const bool is_quote = s.topic.startsWith(QStringLiteral("market:quote:"));
        const bool is_news = s.topic.startsWith(QStringLiteral("news:"));
        if (!is_quote && !is_news)
            continue;
        if (s.subscriber_count <= 0 && s.last_publish_ms <= 0)
            continue;

        QJsonObject t;
        t["topic"] = s.topic;
        t["subscribers"] = s.subscriber_count;
        t["age_ms"] = s.last_publish_ms > 0 ? static_cast<qint64>(now - s.last_publish_ms) : -1;
        topics.append(t);

        if (is_quote && quotes.size() < kMaxContextQuotes && s.last_publish_ms > 0 &&
            (now - s.last_publish_ms) <= kMaxQuoteAgeMs) {
            const QString sym = s.topic.mid(QStringLiteral("market:quote:").size());
            if (const auto q = peek_quote(sym)) {
                QJsonObject qj = quote_to_json(*q);
                qj["age_ms"] = t["age_ms"];
                quotes.append(qj);
            }
        }
    }

    out["live_quotes"] = quotes;
    out["active_topics"] = topics;
    out["hint"] =
        QStringLiteral("Prefer datahub_peek / get_quote (hub-first) for symbols listed in live_quotes.");
    return out;
}

} // namespace detail

std::vector<ToolDef> get_terminal_context_tools() {
    std::vector<ToolDef> tools;

    // ── get_terminal_context ────────────────────────────────────────────
    {
        ToolDef t;
        t.name = "get_terminal_context";
        t.description =
            "Snapshot of what the user is looking at: focused screen id/title plus "
            "live DataHub topics (quotes, news) currently streaming on visible widgets. "
            "Call this when the user asks to summarize what's on screen, or before "
            "fetching data that may already be cached in the hub.";
        t.category = "datahub";
        t.handler = [](const QJsonObject&) -> ToolResult {
            const QJsonObject ctx = detail::build_terminal_context_json();
            const auto screen = ctx.value(QStringLiteral("current_screen")).toObject();
            const QString id = screen.value(QStringLiteral("id")).toString();
            const QString msg =
                id.isEmpty()
                    ? QStringLiteral("Terminal context (no focused screen yet)")
                    : QStringLiteral("Focused screen: %1").arg(
                          screen.value(QStringLiteral("title")).toString(id));
            return ToolResult::ok(msg, ctx);
        };
        tools.push_back(std::move(t));
    }

    LOG_INFO(TAG, QString("Defined %1 terminal-context tools").arg(tools.size()));
    return tools;
}

} // namespace openmarketterminal::mcp::tools
