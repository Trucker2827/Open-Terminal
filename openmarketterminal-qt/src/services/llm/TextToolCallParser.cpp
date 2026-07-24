#include "services/llm/TextToolCallParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace openmarketterminal::ai_chat {

namespace {

// Weak local models copy tool names from whatever catalog spelling they last
// saw — often the MCP-client prefixed form ("openmarketterminal__web_search",
// "mcp__openmarketterminal__get_quote") or an OpenAI-style "functions." path.
// Normalize to the bare tool name before the known-set guard.
QString normalize_tool_name(QString name) {
    for (const char* prefix : {"mcp__openmarketterminal__", "openmarketterminal__",
                               "openmarketterminal:", "functions."}) {
        if (name.startsWith(QLatin1String(prefix))) {
            name = name.mid(int(qstrlen(prefix)));
            break;
        }
    }
    return name;
}

QString name_of(const QJsonObject& o) {
    QString raw;
    if (o.value("name").isString()) raw = o.value("name").toString();
    else if (o.value("function").isString()) raw = o.value("function").toString();
    else if (o.value("function").isObject()) raw = o.value("function").toObject().value("name").toString();
    else if (o.value("tool").isString()) raw = o.value("tool").toString();
    return normalize_tool_name(raw);
}

QJsonObject args_of(const QJsonObject& o) {
    // Args may live under arguments | parameters | input; nested under function;
    // and may be a JSON object or a stringified JSON object.
    const QJsonObject fn = o.value("function").toObject();
    for (const QJsonObject& src : {o, fn}) {
        for (const char* key : {"arguments", "parameters", "input"}) {
            const QJsonValue v = src.value(key);
            if (v.isObject()) return v.toObject();
            if (v.isString()) {
                const auto d = QJsonDocument::fromJson(v.toString().toUtf8());
                if (d.isObject()) return d.object();
            }
        }
    }
    return {};
}

void collect(const QJsonValue& v, const QSet<QString>& known,
             QVector<QPair<QString, QJsonObject>>& out) {
    if (v.isArray()) {
        for (const auto& e : v.toArray()) collect(e, known, out);
        return;
    }
    // A string element that is itself JSON: weak models sometimes emit a list of
    // STRINGIFIED tool-call objects, e.g. ["{\"name\":\"get_quote\",...}", "..."].
    // Recurse into it. fromJson on a non-JSON string yields a null doc → no recursion,
    // and a parsed value is never a string, so this cannot loop.
    if (v.isString()) {
        const auto d = QJsonDocument::fromJson(v.toString().toUtf8());
        if (d.isObject()) collect(d.object(), known, out);
        else if (d.isArray()) collect(d.array(), known, out);
        return;
    }
    if (!v.isObject()) return;
    const QJsonObject o = v.toObject();
    const QString nm = name_of(o);
    if (!nm.isEmpty() && known.contains(nm))   // guard: only known tools
        out.push_back({nm, args_of(o)});
}

} // namespace

QVector<QPair<QString, QJsonObject>> parse_bare_json_tool_calls(const QString& content,
                                                                const QSet<QString>& known) {
    QVector<QPair<QString, QJsonObject>> calls;
    if (known.isEmpty())
        return calls;

    // Candidate JSON snippets: the whole trimmed content (if it looks like JSON),
    // plus any ```json / ``` fenced blocks containing an object/array.
    QStringList candidates;
    const QString trimmed = content.trimmed();
    if (trimmed.startsWith('{') || trimmed.startsWith('['))
        candidates << trimmed;
    static const QRegularExpression rx_fence(
        "```(?:json|tool_call)?\\s*\\n?([\\[{][\\s\\S]*?[\\]}])\\s*```",
        QRegularExpression::MultilineOption | QRegularExpression::DotMatchesEverythingOption);
    auto it = rx_fence.globalMatch(content);
    while (it.hasNext())
        candidates << it.next().captured(1).trimmed();

    // Un-fenced JSON embedded mid-prose ("I would call ... as follows:
    // {\"name\": ...}") — llama-class models narrate the call instead of
    // emitting it. Extract every balanced top-level {...} span and let the
    // known-set guard in collect() reject anything that isn't a tool call.
    for (int i = 0; i < content.size(); ++i) {
        if (content.at(i) != QLatin1Char('{'))
            continue;
        int depth = 0;
        bool in_string = false;
        for (int j = i; j < content.size(); ++j) {
            const QChar c = content.at(j);
            if (in_string) {
                if (c == QLatin1Char('\\')) { ++j; continue; }
                if (c == QLatin1Char('"')) in_string = false;
                continue;
            }
            if (c == QLatin1Char('"')) { in_string = true; continue; }
            if (c == QLatin1Char('{')) ++depth;
            else if (c == QLatin1Char('}')) {
                if (--depth == 0) {
                    candidates << content.mid(i, j - i + 1);
                    i = j;  // resume scan after this object
                    break;
                }
            }
        }
    }

    for (const QString& cand : candidates) {
        const auto doc = QJsonDocument::fromJson(cand.toUtf8());
        if (doc.isObject())
            collect(doc.object(), known, calls);
        else if (doc.isArray())
            collect(doc.array(), known, calls);
        if (!calls.isEmpty())
            break;  // first candidate that yields a known call wins
    }
    return calls;
}

} // namespace openmarketterminal::ai_chat
