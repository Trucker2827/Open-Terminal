// NotebookTools.cpp — let the in-app AI author runnable analysis notebooks into the
// Notebooks screen (mirrors the report_* tools). Slice A: create + open; the user runs
// the cells (kernel-run from a tool is a later slice). GUI tool (touches CodeEditorScreen).
#include "mcp/tools/NotebookTools.h"

#include "core/events/EventBus.h"
#include "mcp/tools/ThreadHelper.h"
#include "python/NotebookKernel.h"
#include "screens/code_editor/CodeEditorScreen.h"
#include "services/notebooks/NotebookLibraryService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <optional>

namespace openmarketterminal::mcp::tools {

static QString slugify(const QString& s) {
    QString out;
    for (const QChar c : s.toLower()) {
        if (c.isLetterOrNumber()) out += c;
        else if ((c == ' ' || c == '-' || c == '_') && !out.endsWith('_')) out += '_';
    }
    while (out.endsWith('_')) out.chop(1);
    return out;
}

// Split source into nbformat-style lines (each keeps its trailing newline except the last).
static QJsonArray source_lines(const QString& src) {
    QJsonArray arr;
    const QStringList lines = src.split('\n');
    for (int i = 0; i < lines.size(); ++i)
        arr.append(i + 1 < lines.size() ? lines[i] + "\n" : lines[i]);
    return arr;
}

static std::optional<openmarketterminal::services::NotebookCatalogEntry>
find_notebook_entry(const QString& selector) {
    const QString needle = selector.trimmed();
    if (needle.isEmpty())
        return std::nullopt;
    const auto& catalog = openmarketterminal::services::NotebookLibraryService::instance().catalog();
    for (const auto& e : catalog) {
        if (e.id == needle || e.file == needle || e.title.compare(needle, Qt::CaseInsensitive) == 0)
            return e;
    }
    for (const auto& e : catalog) {
        if (e.id.contains(needle, Qt::CaseInsensitive) ||
            e.title.contains(needle, Qt::CaseInsensitive) ||
            e.file.contains(needle, Qt::CaseInsensitive))
            return e;
    }
    return std::nullopt;
}

std::vector<ToolDef> get_notebook_tools() {
    std::vector<ToolDef> tools;

    ToolDef t;
    t.name = "notebook_create";
    t.description =
        "Create a runnable Python notebook in the Notebooks screen and open it for the user "
        "to run/edit. Use for analysis that benefits from live, editable computation (DCF, comps, "
        "fundamentals): pass an ordered list of markdown + code cells. Code cells run in the "
        "notebook kernel, where edgartools, pandas, and numpy are available — fetch SEC data and "
        "compute there. The USER runs the cells (this tool does not execute them). "
        "Args: title (string), cells (array of {type:'code'|'markdown', source:string}).";
    t.category = "notebook-builder";
    t.input_schema.properties = QJsonObject{
        {"title", QJsonObject{{"type", "string"}, {"description", "Notebook title"}}},
        {"cells", QJsonObject{{"type", "array"},
                              {"description", "Ordered cells: {type:'code'|'markdown', source:string}"}}},
    };
    t.input_schema.required = {"title", "cells"};
    t.handler = [](const QJsonObject& args) -> ToolResult {
        const QString title = args.value("title").toString().trimmed();
        const QJsonArray in_cells = args.value("cells").toArray();
        if (title.isEmpty())
            return ToolResult::fail("Missing 'title'");
        if (in_cells.isEmpty())
            return ToolResult::fail("Missing 'cells' (array of {type, source})");

        QJsonArray nb_cells;
        int code_n = 0, md_n = 0;
        for (const auto& v : in_cells) {
            const QJsonObject c = v.toObject();
            const QString type = c.value("type").toString("code").toLower();
            const QJsonArray src = source_lines(c.value("source").toString());
            if (type == "markdown") {
                nb_cells.append(QJsonObject{{"cell_type", "markdown"}, {"metadata", QJsonObject{}}, {"source", src}});
                ++md_n;
            } else {
                nb_cells.append(QJsonObject{{"cell_type", "code"}, {"execution_count", QJsonValue::Null},
                                            {"metadata", QJsonObject{}}, {"outputs", QJsonArray{}}, {"source", src}});
                ++code_n;
            }
        }
        const QJsonObject nb{
            {"cells", nb_cells},
            {"metadata", QJsonObject{
                {"kernelspec", QJsonObject{{"display_name", "Python 3"}, {"language", "python"}, {"name", "python3"}}},
                {"language_info", QJsonObject{{"name", "python"}}}}},
            {"nbformat", 4}, {"nbformat_minor", 5}};

        QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        dir = (dir.isEmpty() ? QDir::tempPath() + "/openterminal" : dir) + "/ai_notebooks";
        QDir().mkpath(dir);
        QString stem = slugify(title);
        if (stem.isEmpty()) stem = "analysis";
        const QString path = dir + "/" + stem + ".ipynb";
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return ToolResult::fail("Could not write notebook file: " + path);
        f.write(QJsonDocument(nb).toJson(QJsonDocument::Indented));
        f.close();

        // Open it in the Notebooks screen. nav.switch_screen lazily constructs the screen;
        // the open then runs on the main thread (open_notebook_path is a UI mutation).
        openmarketterminal::EventBus::instance().publish(
            "nav.switch_screen", QVariantMap{{"screen_id", "code_editor"}});
        bool opened = false;
        QString path_copy = path;
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [&opened, path_copy]() {
                if (auto* scr = screens::CodeEditorScreen::current())
                    opened = scr->open_notebook_path(path_copy);
            },
            Qt::BlockingQueuedConnection);

        return ToolResult::ok_data(QJsonObject{
            {"path", path}, {"code_cells", code_n}, {"markdown_cells", md_n}, {"opened", opened}});
    };
    tools.push_back(std::move(t));

    ToolDef o;
    o.name = "notebook_open";
    o.description =
        "Open a notebook in the Notebooks screen by bundled catalog id/title/query or by local .ipynb path. "
        "Args: id, title, query, or path.";
    o.category = "notebook-builder";
    o.input_schema.properties = QJsonObject{
        {"id", QJsonObject{{"type", "string"}, {"description", "Bundled notebook catalog id"}}},
        {"title", QJsonObject{{"type", "string"}, {"description", "Bundled notebook title"}}},
        {"query", QJsonObject{{"type", "string"}, {"description", "Catalog id/title/file search text"}}},
        {"path", QJsonObject{{"type", "string"}, {"description", "Absolute or relative .ipynb path"}}},
    };
    o.handler = [](const QJsonObject& args) -> ToolResult {
        QString path = args.value("path").toString().trimmed();
        QJsonObject data;
        if (path.isEmpty()) {
            const QString selector = !args.value("id").toString().trimmed().isEmpty()
                                         ? args.value("id").toString().trimmed()
                                         : (!args.value("title").toString().trimmed().isEmpty()
                                                ? args.value("title").toString().trimmed()
                                                : args.value("query").toString().trimmed());
            const auto entry = find_notebook_entry(selector);
            if (!entry)
                return ToolResult::fail("Notebook not found: " + selector);
            path = openmarketterminal::services::NotebookLibraryService::instance().working_copy_for(*entry);
            data["id"] = entry->id;
            data["title"] = entry->title;
            data["category"] = entry->category;
        }
        if (path.isEmpty())
            return ToolResult::fail("Missing notebook path or catalog selector");
        QFileInfo fi(path);
        if (fi.exists())
            path = fi.absoluteFilePath();
        if (!QFileInfo::exists(path))
            return ToolResult::fail("Notebook file not found: " + path);

        openmarketterminal::EventBus::instance().publish(
            "nav.switch_screen", QVariantMap{{"screen_id", "code_editor"}});
        bool opened = false;
        const QString path_copy = path;
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [&opened, path_copy]() {
                if (auto* scr = screens::CodeEditorScreen::current())
                    opened = scr->open_notebook_path(path_copy);
            },
            Qt::BlockingQueuedConnection);
        data["path"] = path;
        data["opened"] = opened;
        return ToolResult::ok_data(data);
    };
    tools.push_back(std::move(o));

    // ── notebook_run — execute code in the live, stateful notebook kernel ───────
    ToolDef r;
    r.name = "notebook_run";
    r.description =
        "Execute Python in the live Notebooks kernel and return its output. The kernel is "
        "STATEFUL and SHARED with the open notebook — variables persist across calls and across "
        "the user's own cell runs — and edgartools, pandas, and numpy are available. Use to run or "
        "verify analysis code and read the result, then interpret it for the user. Returns ok, "
        "stdout, result (repr of the last expression), and error/traceback on failure. Arg: code.";
    r.category = "notebook-builder";
    r.default_timeout_ms = 120000;  // analysis code may hit SEC EDGAR
    r.input_schema.properties = QJsonObject{
        {"code", QJsonObject{{"type", "string"}, {"description", "Python to run in the notebook kernel"}}}};
    r.input_schema.required = {"code"};
    r.handler = [](const QJsonObject& args) -> ToolResult {
        const QString code = args.value("code").toString();
        if (code.trimmed().isEmpty())
            return ToolResult::fail("Missing 'code'");
        auto* kernel = &python::NotebookKernel::instance();
        python::NotebookKernel::Result res;
        detail::run_async_wait(kernel, [&](auto signal_done) {
            kernel->execute(code, [&, signal_done](python::NotebookKernel::Result rr) {
                res = std::move(rr);
                signal_done();
            });
        });
        QJsonObject data{{"ok", res.ok}, {"stdout", res.stdout_text}, {"result", res.result_repr}};
        if (!res.stderr_text.isEmpty()) data["stderr"] = res.stderr_text;
        if (!res.error.isEmpty()) data["error"] = res.error;
        if (!res.traceback.isEmpty()) {
            QJsonArray tb;
            for (const auto& l : res.traceback) tb.append(l);
            data["traceback"] = tb;
        }
        return ToolResult::ok_data(data);
    };
    tools.push_back(std::move(r));

    return tools;
}

} // namespace openmarketterminal::mcp::tools
