# Local-model tool reliability + web browsing — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make local/Ollama models stop hallucinating live data by letting them call the app's existing live-data tools in one step, and add `web_search`/`web_fetch` tools powered by the bundled QtWebEngine (headless Chromium) querying DuckDuckGo.

**Architecture:** Part 1 — when the active provider is local (`is_local_model`), attach a curated "essentials" toolset directly (Tool-RAG bypassed) + a short anti-hallucination prompt rule. Part 2 — a main-thread `HeadlessBrowser` singleton (offscreen `QWebEnginePage`) plus a pure `parse_ddg_results` parser, surfaced as two GUI-tier MCP tools whose handlers marshal worker→main with a hard timeout.

**Tech Stack:** Qt 6 (Core, Widgets, WebEngineWidgets), C++20, the app's MCP `ToolDef` framework, QtTest.

## Global Constraints

- C++20; Qt 6.8+ (`/opt/homebrew`). Build GUI/app target `OpenMarketTerminal`; build tests with `-DOPENMARKETTERMINAL_BUILD_TESTS=ON`.
- Spec: `docs/design/2026-06-17-local-model-tools-web-browsing-design.md`.
- New non-test C++ sources MUST be added to BOTH app source groups in `CMakeLists.txt` (the two `*_SOURCES` lists where `src/screens/ai_chat/AnalysisSlashCommands.cpp` appears).
- Web tools are **GUI-tier** (registered in `register_gui_tools()` / `McpInitGui.cpp`) — they need QtWebEngine + a GUI event loop; they are intentionally absent from the headless CLI.
- Web tools are **read-only**: `auth_required = AuthLevel::None`, no `requires_confirmation`.
- Tool registration mirrors `src/mcp/tools/NotebookTools.cpp` (`std::vector<ToolDef> get_*_tools()`, `t.input_schema.properties = QJsonObject{...}`, set `t.async_handler` for I/O work, `t.default_timeout_ms`).
- Search backend: DuckDuckGo only (`https://html.duckduckgo.com/html/?q=`). No API key. No Google/Bing in this change.
- Verify on the real path: a passing test must be confirmed to RUN and to FAIL without the change (neuter-verify per repo discipline).

---

### Task 1: DuckDuckGo result parser (pure, TDD)

**Files:**
- Create: `src/web/WebSearchParser.h`, `src/web/WebSearchParser.cpp`
- Create: `tests/fixtures/ddg_results.html` (saved real DDG html-endpoint response)
- Test: `tests/tst_web_search_parser.cpp`
- Modify: `tests/CMakeLists.txt`, `CMakeLists.txt` (both app source groups)

**Interfaces:**
- Produces: `namespace openmarketterminal::web { struct WebResult { QString title, url, snippet; }; QVector<WebResult> parse_ddg_results(const QString& html, int max_results); }`

- [ ] **Step 1: Save a fixture.** Fetch the DDG html endpoint once and save the body:

```bash
curl -sL -A "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36" \
  "https://html.duckduckgo.com/html/?q=apple+stock+news" \
  -o tests/fixtures/ddg_results.html
# sanity: should contain result anchors
grep -c 'result__a' tests/fixtures/ddg_results.html   # expect >= 5
```

If `result__a` count is 0 (markup changed/blocked), open the file and adjust the selectors in Step 3 to the actual classes; do not proceed on an empty fixture.

- [ ] **Step 2: Write the failing test.**

```cpp
#include <QtTest>
#include <QFile>
#include "web/WebSearchParser.h"
using namespace openmarketterminal::web;

class TstWebSearchParser : public QObject {
    Q_OBJECT
private slots:
    void parses_fixture() {
        QFile f(QFINDTESTDATA("fixtures/ddg_results.html"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString html = QString::fromUtf8(f.readAll());
        const QVector<WebResult> rows = parse_ddg_results(html, 5);
        QVERIFY2(rows.size() >= 3, "expected at least 3 parsed results");
        QVERIFY(rows.size() <= 5);
        for (const auto& r : rows) {
            QVERIFY(!r.title.trimmed().isEmpty());
            QVERIFY(r.url.startsWith("http"));          // decoded, absolute
            QVERIFY(!r.url.contains("/l/?uddg="));        // DDG redirect decoded
        }
    }
    void empty_html_yields_empty() {
        QVERIFY(parse_ddg_results("", 5).isEmpty());
        QVERIFY(parse_ddg_results("<html><body>no results</body></html>", 5).isEmpty());
    }
    void respects_max_results() {
        QFile f(QFINDTESTDATA("fixtures/ddg_results.html"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(parse_ddg_results(QString::fromUtf8(f.readAll()), 2).size(), 2);
    }
};
QTEST_MAIN(TstWebSearchParser)
#include "tst_web_search_parser.moc"
```

- [ ] **Step 3: Implement the parser.**

`WebSearchParser.h`:
```cpp
#pragma once
#include <QString>
#include <QVector>
namespace openmarketterminal::web {
struct WebResult { QString title; QString url; QString snippet; };
// Parse the DuckDuckGo html-endpoint (html.duckduckgo.com/html) result list.
// Returns up to max_results rows; decodes DDG /l/?uddg= redirect links to the
// real URL. Tolerant of missing snippets. Pure (no network, no Qt GUI).
QVector<WebResult> parse_ddg_results(const QString& html, int max_results);
}
```

`WebSearchParser.cpp`:
```cpp
#include "web/WebSearchParser.h"
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

namespace openmarketterminal::web {

static QString strip_tags(QString s) {
    s.remove(QRegularExpression("<[^>]*>"));
    s = s.toHtmlEscaped();                       // normalize, then unescape entities
    s.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">")
     .replace("&quot;", "\"").replace("&#x27;", "'").replace("&#39;", "'").replace("&nbsp;", " ");
    return s.simplified();
}

static QString decode_ddg_url(const QString& href) {
    // DDG wraps results as /l/?uddg=<urlencoded real url>&...
    QUrl u(href);
    if (u.path().contains("/l/")) {
        const QString uddg = QUrlQuery(u).queryItemValue("uddg", QUrl::FullyDecoded);
        if (!uddg.isEmpty()) return uddg;
    }
    if (href.startsWith("//")) return "https:" + href;
    return href;
}

QVector<WebResult> parse_ddg_results(const QString& html, int max_results) {
    QVector<WebResult> out;
    if (html.isEmpty() || max_results <= 0) return out;
    // Each result anchor: <a class="result__a" href="...">title</a>
    static const QRegularExpression kАnchor(
        "<a[^>]*class=\"[^\"]*result__a[^\"]*\"[^>]*href=\"([^\"]+)\"[^>]*>(.*?)</a>",
        QRegularExpression::DotMatchesEverythingOption);
    // Snippet: <a class="result__snippet" ...>snippet</a>  (may be absent)
    static const QRegularExpression kSnippet(
        "<a[^>]*class=\"[^\"]*result__snippet[^\"]*\"[^>]*>(.*?)</a>",
        QRegularExpression::DotMatchesEverythingOption);

    auto anchors = kАnchor.globalMatch(html);
    auto snippets = kSnippet.globalMatch(html);
    while (anchors.hasNext() && out.size() < max_results) {
        const auto m = anchors.next();
        WebResult r;
        r.url = decode_ddg_url(m.captured(1));
        r.title = strip_tags(m.captured(2));
        if (snippets.hasNext()) r.snippet = strip_tags(snippets.next().captured(1));
        if (!r.title.isEmpty() && r.url.startsWith("http")) out.push_back(r);
    }
    return out;
}
}
```

(If the fixture uses different class names, adjust the two regexes to match — keep the decode/strip helpers.)

- [ ] **Step 4: Wire the test target.** Append to `tests/CMakeLists.txt` (after the `tst_session_title` block):

```cmake
add_executable(tst_web_search_parser
    tst_web_search_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/web/WebSearchParser.cpp)
target_include_directories(tst_web_search_parser PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tst_web_search_parser PRIVATE Qt6::Core Qt6::Test)
# fixtures/ resolved via QFINDTESTDATA — copy next to the binary
add_custom_command(TARGET tst_web_search_parser POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures $<TARGET_FILE_DIR:tst_web_search_parser>/fixtures)
add_test(NAME tst_web_search_parser COMMAND tst_web_search_parser)
```

Add `src/web/WebSearchParser.cpp` to BOTH app source groups in `CMakeLists.txt` (next to `AnalysisSlashCommands.cpp`).

- [ ] **Step 5: Build + run; verify PASS, then neuter-verify.**

```bash
env -u CI cmake --build /tmp/ot-build-clean --target tst_web_search_parser 2>&1 | grep -iE "error:" || true
/tmp/ot-build-clean/tests/tst_web_search_parser   # expect: 3 passed
```
Neuter: temporarily `return {};` at the top of `parse_ddg_results` → rebuild → expect `parses_fixture` + `respects_max_results` FAIL. Revert.

- [ ] **Step 6: Commit.**

```bash
git add src/web/WebSearchParser.* tests/tst_web_search_parser.cpp tests/fixtures/ddg_results.html tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(web): pure DuckDuckGo result parser + unit test"
```

---

### Task 2: HeadlessBrowser (offscreen QtWebEngine, main thread)

**Files:**
- Create: `src/web/HeadlessBrowser.h`, `src/web/HeadlessBrowser.cpp`
- Modify: `CMakeLists.txt` (both app source groups; ensure `Qt6::WebEngineWidgets` is linked to the app target — it already is, since QtWebEngine is bundled)

**Interfaces:**
- Produces: `namespace openmarketterminal::web { class HeadlessBrowser : public QObject { public: static HeadlessBrowser& instance(); // BLOCKS the calling thread; safe to call from a worker thread. Loads url, waits for load + settle, runs extraction_js, returns its string result (or "" on timeout/failure). QString fetch(const QUrl& url, const QString& extraction_js, int timeout_ms = 15000); }; }`

**Notes for the implementer:** `QWebEnginePage` must live on the GUI thread. `fetch()` is called from an MCP worker thread, so it must (a) `QMetaObject::invokeMethod(this, ..., Qt::QueuedConnection)` to do the actual load on the main thread, and (b) block the worker on a `QWaitCondition`/`QEventLoop` until the main thread signals done or the timeout fires. Serialize with a `QMutex` so only one load runs at a time (one shared page).

- [ ] **Step 1: Implement the header.**

```cpp
#pragma once
#include <QObject>
#include <QUrl>
#include <QString>
#include <QMutex>
class QWebEnginePage;
namespace openmarketterminal::web {
class HeadlessBrowser : public QObject {
    Q_OBJECT
public:
    static HeadlessBrowser& instance();
    // Blocking; callable from any thread. Returns extraction_js result, or "" on
    // timeout/load failure. Serialized internally (one load at a time).
    QString fetch(const QUrl& url, const QString& extraction_js, int timeout_ms = 15000);
private:
    explicit HeadlessBrowser(QObject* parent = nullptr);
    Q_INVOKABLE void start_load(QUrl url, QString extraction_js, int timeout_ms);
    QWebEnginePage* page_ = nullptr;   // created lazily on the GUI thread
    QMutex serialize_;                 // one fetch at a time
};
}
```

- [ ] **Step 2: Implement the source.**

```cpp
#include "web/HeadlessBrowser.h"
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <QCoreApplication>

namespace openmarketterminal::web {

HeadlessBrowser& HeadlessBrowser::instance() {
    static HeadlessBrowser inst;
    return inst;
}
HeadlessBrowser::HeadlessBrowser(QObject* parent) : QObject(parent) {
    // Ensure this object lives on the GUI/main thread (where QApplication runs).
    moveToThread(QCoreApplication::instance()->thread());
}

// Runs on the GUI thread (queued from fetch()). Performs the load + extraction and
// stores the result, then quits the per-call loop via the result pointer mechanism.
struct LoadResult { QString text; bool done = false; };

QString HeadlessBrowser::fetch(const QUrl& url, const QString& extraction_js, int timeout_ms) {
    QMutexLocker lock(&serialize_);
    QString result;
    QEventLoop worker_loop;            // blocks THIS (worker) thread
    bool finished = false;

    // Hop to the GUI thread to drive QtWebEngine.
    QMetaObject::invokeMethod(this, [&]() {
        if (!page_) {
            page_ = new QWebEnginePage(QWebEngineProfile::defaultProfile(), this);
            page_->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
            page_->settings()->setAttribute(QWebEngineSettings::AutoLoadImages, false);
            QObject::connect(page_, &QWebEnginePage::javaScriptDialogRequested,
                             [](auto* req){ req->dialogReject(); });   // never block on dialogs
        }
        auto* settle = new QTimer(page_);
        settle->setSingleShot(true);
        auto* hard = new QTimer(page_);
        hard->setSingleShot(true);

        auto finish = [&, settle, hard](const QString& js_result) {
            if (finished) return;
            finished = true;
            settle->stop(); hard->stop(); settle->deleteLater(); hard->deleteLater();
            result = js_result;
            // wake the worker loop
            QMetaObject::invokeMethod(&worker_loop, &QEventLoop::quit, Qt::QueuedConnection);
        };

        QObject::connect(hard, &QTimer::timeout, page_, [finish]() { finish(QString()); });
        hard->start(timeout_ms);

        QObject::connect(page_, &QWebEnginePage::loadFinished, page_,
            [this, settle, extraction_js, finish](bool ok) {
                if (!ok) { finish(QString()); return; }
                settle->disconnect();
                QObject::connect(settle, &QTimer::timeout, page_, [this, extraction_js, finish]() {
                    page_->runJavaScript(extraction_js, [finish](const QVariant& v) {
                        finish(v.toString());
                    });
                });
                settle->start(400);    // let late JS settle
            }, Qt::UniqueConnection);

        page_->setUrl(url);
    }, Qt::QueuedConnection);

    // Block the worker until the GUI side calls finish() (or hard timeout fires).
    // Add a belt-and-suspenders worker-side timeout slightly longer than the GUI one.
    QTimer::singleShot(timeout_ms + 2000, &worker_loop, &QEventLoop::quit);
    worker_loop.exec();
    return result;
}
void HeadlessBrowser::start_load(QUrl, QString, int) {}  // reserved (lambda path used above)
}
```

(If `loadFinished` connections accumulate across calls, the serialize mutex makes that harmless — only one fetch runs at a time; reset `page_` connections inside the lambda if needed.)

- [ ] **Step 3: Wire CMake + build.** Add `src/web/HeadlessBrowser.cpp` to BOTH app source groups in `CMakeLists.txt`. Confirm the app target links `Qt6::WebEngineWidgets` (search `CMakeLists.txt` for `WebEngine`; it's already used for KLineChart/Live TV — add to the same `target_link_libraries` if missing).

```bash
env -u CI cmake --build /tmp/ot-build-clean --target OpenMarketTerminal 2>&1 | grep -iE "error:|undefined" | grep -v "newer version" || echo "build OK"
```

- [ ] **Step 4: Commit.**

```bash
git add src/web/HeadlessBrowser.* CMakeLists.txt
git commit -m "feat(web): offscreen QtWebEngine HeadlessBrowser (main-thread, timeout, serialized)"
```

---

### Task 3: web_search + web_fetch MCP tools (GUI-tier)

**Files:**
- Create: `src/mcp/tools/WebTools.h`, `src/mcp/tools/WebTools.cpp`
- Modify: `src/mcp/McpInitGui.cpp` (register), `CMakeLists.txt` (both app source groups)

**Interfaces:**
- Consumes: `web::parse_ddg_results` (Task 1), `web::HeadlessBrowser::instance().fetch(...)` (Task 2).
- Produces: `namespace openmarketterminal::mcp::tools { std::vector<ToolDef> get_web_tools(); }`

- [ ] **Step 1: Implement `WebTools.h`.**

```cpp
#pragma once
#include "mcp/McpTypes.h"
#include <vector>
namespace openmarketterminal::mcp::tools { std::vector<ToolDef> get_web_tools(); }
```

- [ ] **Step 2: Implement `WebTools.cpp`** (mirror `NotebookTools.cpp` registration; async handlers since they do I/O):

```cpp
#include "mcp/tools/WebTools.h"
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

    { // web_search
        ToolDef t;
        t.name = "web_search";
        t.description =
            "Search the web (DuckDuckGo) for CURRENT information and return the top results "
            "(title, url, snippet). Use for live/general facts not covered by the finance data "
            "tools (recent events, general news, definitions). Args: query (string), "
            "max_results (int, default 5).";
        t.category = "web";
        t.input_schema.properties = QJsonObject{
            {"query", QJsonObject{{"type","string"},{"description","Search query"}}},
            {"max_results", QJsonObject{{"type","integer"},{"description","Max results (default 5)"}}},
        };
        t.input_schema.required = QStringList{"query"};
        t.auth_required = AuthLevel::None;
        t.default_timeout_ms = 20000;
        t.async_handler = [](const QJsonObject& args, AsyncToolResult cb) {
            const QString q = args.value("query").toString();
            const int n = args.value("max_results").toInt(5);
            if (q.trimmed().isEmpty()) { cb(ToolResult::error("query is required")); return; }
            QUrl url("https://html.duckduckgo.com/html/");
            QUrlQuery qq; qq.addQueryItem("q", q); url.setQuery(qq);
            // extraction: return the page HTML (parser runs in C++).
            const QString js = "document.documentElement.outerHTML";
            const QString html = web::HeadlessBrowser::instance().fetch(url, js, 15000);
            if (html.isEmpty()) { cb(ToolResult::error("web_search: load failed or timed out")); return; }
            const auto rows = web::parse_ddg_results(html, n);
            QJsonArray arr;
            for (const auto& r : rows)
                arr.append(QJsonObject{{"title",r.title},{"url",r.url},{"snippet",r.snippet}});
            cb(ToolResult::ok(QJsonObject{{"results", arr}}));
        };
        tools.push_back(std::move(t));
    }
    { // web_fetch
        ToolDef t;
        t.name = "web_fetch";
        t.description =
            "Fetch a web page (headless Chromium, runs JS) and return its readable text. Use after "
            "web_search to read a result, or for a known URL. Args: url (string), max_chars (int, "
            "default 6000).";
        t.category = "web";
        t.input_schema.properties = QJsonObject{
            {"url", QJsonObject{{"type","string"},{"description","Absolute http(s) URL"}}},
            {"max_chars", QJsonObject{{"type","integer"},{"description","Truncate text (default 6000)"}}},
        };
        t.input_schema.required = QStringList{"url"};
        t.auth_required = AuthLevel::None;
        t.default_timeout_ms = 20000;
        t.async_handler = [](const QJsonObject& args, AsyncToolResult cb) {
            const QUrl url(args.value("url").toString());
            const int cap = args.value("max_chars").toInt(6000);
            if (!url.isValid() || !(url.scheme()=="http"||url.scheme()=="https")) {
                cb(ToolResult::error("web_fetch: a valid http(s) url is required")); return; }
            const QString js =
                "(function(){var t=document.title;"
                "document.querySelectorAll('script,style,nav,header,footer,noscript').forEach(e=>e.remove());"
                "return JSON.stringify({title:t, text:(document.body?document.body.innerText:'')});})()";
            const QString raw = web::HeadlessBrowser::instance().fetch(url, js, 15000);
            if (raw.isEmpty()) { cb(ToolResult::error("web_fetch: load failed or timed out")); return; }
            const QJsonObject o = QJsonDocument::fromJson(raw.toUtf8()).object();
            QString text = o.value("text").toString();
            if (text.size() > cap) text = text.left(cap) + "…";
            cb(ToolResult::ok(QJsonObject{{"title",o.value("title").toString()},
                                          {"url",url.toString()},{"text",text}}));
        };
        tools.push_back(std::move(t));
    }
    return tools;
}
}
```

(Match the exact handler typedefs — `AsyncToolHandler`, `AsyncToolResult`, `ToolResult::ok/error` — to what `NotebookTools.cpp`/`McpTypes.h` actually use; adjust names if they differ, e.g. result built via the same helper those files use.)

- [ ] **Step 3: Register in `McpInitGui.cpp`.** Add the include and the registration call inside `register_gui_tools()`:

```cpp
#include "mcp/tools/WebTools.h"
// ... inside register_gui_tools():
provider.register_tools(tools::get_web_tools());
```

- [ ] **Step 4: Wire CMake + build.** Add `src/mcp/tools/WebTools.cpp` to BOTH app source groups in `CMakeLists.txt`.

```bash
env -u CI cmake --build /tmp/ot-build-clean --target OpenMarketTerminal 2>&1 | grep -iE "error:|undefined" | grep -v "newer version" || echo "build OK"
```

- [ ] **Step 5: Live verify the tools via the bridge** (app must be running this build):

```bash
/tmp/ot-build-clean/openterminalcli --json mcp list 2>/dev/null \
  | python3 -c "import json,sys;d=json.load(sys.stdin);t=d.get('tools',d);n=[x.get('name') for x in t];print('web_search' in n, 'web_fetch' in n)"
# expect: True True
/tmp/ot-build-clean/openterminalcli mcp call web_search '{"query":"apple stock price today","max_results":3}'
# expect: JSON with 3 results (title/url/snippet)
```

- [ ] **Step 6: Commit.**

```bash
git add src/mcp/tools/WebTools.* src/mcp/McpInitGui.cpp CMakeLists.txt
git commit -m "feat(mcp): web_search + web_fetch GUI tools (headless Chromium + DuckDuckGo)"
```

---

### Task 4: Local-essentials toolset + Tool-RAG bypass + anti-hallucination rule

**Files:**
- Modify: `src/mcp/McpProvider.h`/`.cpp` (or `McpService`) — add `local_essentials_tool_names()`
- Modify: `src/services/llm/LlmService.cpp` (lean prompt rule + local tool selection), `src/services/llm/LlmOpenMarketTerminalAsync.cpp` (local branch in `build_tool_catalog_for_prompt`)
- Test: `tests/tst_local_essentials.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `const QSet<QString>& mcp::local_essentials_tool_names();`

- [ ] **Step 1: Failing test for the essentials set.**

```cpp
#include <QtTest>
#include "mcp/McpProvider.h"
class TstLocalEssentials : public QObject {
    Q_OBJECT
private slots:
    void contains_key_live_tools() {
        const auto& s = openmarketterminal::mcp::local_essentials_tool_names();
        for (const char* n : {"get_quote","get_equity_news","web_search","web_fetch","tool_list"})
            QVERIFY2(s.contains(n), n);
        QVERIFY(s.size() >= 12 && s.size() <= 30);   // curated, not all 605
    }
};
QTEST_MAIN(TstLocalEssentials)
#include "tst_local_essentials.moc"
```

- [ ] **Step 2: Run → FAIL** (`local_essentials_tool_names` undefined). Build target `tst_local_essentials`.

- [ ] **Step 3: Implement the set** in `McpProvider.cpp` (declare in `McpProvider.h` in the `mcp` namespace):

```cpp
const QSet<QString>& local_essentials_tool_names() {
    static const QSet<QString> kSet = {
        "get_quote","get_equity_quote","get_equity_news","search_news","get_candles",
        "get_equity_info","get_equity_peers","get_equity_technicals","edgar_get_financials",
        "edgar_search_filings","get_top_news","get_news","get_observations","lookup_symbol",
        "search_equity_symbols","web_search","web_fetch","tool_list",
    };
    return kSet;
}
```

- [ ] **Step 4: Run → PASS.** Then neuter (remove `web_search` from the set) → `contains_key_live_tools` FAILs → revert.

- [ ] **Step 5: Append the anti-hallucination rule** to the lean local prompt in `LlmService.cpp` (the `if (system_prompt_.trimmed().isEmpty() && is_local_model)` block, ~line 158-170), inside the same string literal:

```cpp
        "5. NEVER state a price, quote, market level, holding, or recent news/event "
        "from memory or training data — it is stale. For anything current you MUST "
        "call a tool (get_quote, get_equity_news, web_search) and answer only from "
        "the result. If unsure which tool, call tool_list.\n"
```

- [ ] **Step 6: Bypass Tool-RAG for local models in tool selection.** Where tools are attached to the request, when `is_local_model`, build the tool list from `mcp::local_essentials_tool_names()` instead of the RAG Tier-0 set. Two call sites:
  - `LlmOpenMarketTerminalAsync.cpp::build_tool_catalog_for_prompt` — add, before the `use_rag` branch: if the caller marks local (thread a `bool is_local` param, or check the same provider/base-url condition via `LlmService`), list only tools whose name is in `local_essentials_tool_names()` (legacy-mode formatting), and skip RAG.
  - The structured-`tools` builder for the Ollama OpenAI-compatible request in `LlmService.cpp` — filter the attached tool array to `local_essentials_tool_names()` when `is_local_model`.

  Use a `mcp::ToolFilter` with `name_patterns` set to the essentials names (exact-match), mirroring how `LlmRequestPolicy.h` already builds filters, so both paths share one mechanism.

- [ ] **Step 7: Build the app + targeted tests.**

```bash
env -u CI cmake --build /tmp/ot-build-clean --target OpenMarketTerminal tst_local_essentials 2>&1 | grep -iE "error:" | grep -v "newer version" || echo OK
/tmp/ot-build-clean/tests/tst_local_essentials   # expect pass
```

- [ ] **Step 8: Commit.**

```bash
git add src/mcp/McpProvider.* src/services/llm/LlmService.cpp src/services/llm/LlmOpenMarketTerminalAsync.cpp tests/tst_local_essentials.cpp tests/CMakeLists.txt
git commit -m "feat(ai): local models get curated essentials toolset (RAG bypass) + no-stale-data rule"
```

---

### Task 5: Full regression + live verification

- [ ] **Step 1: Full ctest.**

```bash
env -u CI cmake --build /tmp/ot-build-clean 2>&1 | grep -iE "error:" | grep -v "newer version" || echo "build OK"
ctest --test-dir /tmp/ot-build-clean   # expect 100% (prior count + tst_web_search_parser + tst_local_essentials)
```

- [ ] **Step 2: Live with Ollama llama3.3** (launch the build; Ollama running). In AI Chat:
  - "what's AAPL trading at right now?" → observe a **`get_quote`** call in the tool trace; the answer reflects the live quote (NOT $182/iPhone-15). 
  - "what's the latest news on the Fed?" → a **`web_search`** (or `get_top_news`) call, answer cites fetched results.
  - "open this and summarize: <a real article URL>" → a **`web_fetch`** call returning readable text.
  Capture screenshots.

- [ ] **Step 3: Confirm premium-Claude path unchanged** — switch to a cloud model, ask a report request; verify the full prompt + RAG path still applies (the local-only branches are gated on `is_local_model`).

- [ ] **Step 4: Commit any fixups; update `docs/releases/` notes** if shipping a release (separate version bump).

## Self-Review

- **Spec coverage:** Part 1a (essentials + RAG bypass) → Task 4 steps 3,6; Part 1b (prompt rule) → Task 4 step 5; Part 2 `web_search`/`web_fetch` → Task 3; HeadlessBrowser threading/timeout → Task 2; parser isolation + fixture test → Task 1; testing → Tasks 1,4,5; GUI-tier registration → Task 3 step 3. All covered.
- **Placeholder scan:** code shown for every code step; the only "match the exact typedef" notes (Task 3 step 2, Task 4 step 6) point at concrete existing files/lines to mirror — acceptable, since the precise handler-typedef names must be read from the live headers (`McpTypes.h`, `NotebookTools.cpp`).
- **Type consistency:** `parse_ddg_results(QString,int)` and `WebResult{title,url,snippet}` used identically in Tasks 1 & 3; `HeadlessBrowser::instance().fetch(QUrl,QString,int)` defined Task 2, consumed Task 3; `local_essentials_tool_names()` defined Task 4 step 3, asserted step 1.

## Known follow-ups (out of scope)

- Bing fallback when DuckDuckGo returns empty/rate-limits.
- Full Readability.js extraction for `web_fetch`.
- Optional keyed providers (Brave/Tavily) via Settings→Credentials.
