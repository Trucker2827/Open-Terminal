// NotebookTools.cpp — let the in-app AI author runnable analysis notebooks into the
// Notebooks screen (mirrors the report_* tools). Slice A: create + open; the user runs
// the cells (kernel-run from a tool is a later slice). GUI tool (touches CodeEditorScreen).
#include "mcp/tools/NotebookTools.h"

#include "core/events/EventBus.h"
#include "screens/code_editor/CodeEditorScreen.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

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
    return tools;
}

} // namespace openmarketterminal::mcp::tools
