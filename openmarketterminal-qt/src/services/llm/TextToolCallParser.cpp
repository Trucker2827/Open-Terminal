#include "services/llm/TextToolCallParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace openmarketterminal::ai_chat {

namespace {

QString name_of(const QJsonObject& o) {
    if (o.value("name").isString()) return o.value("name").toString();
    if (o.value("function").isString()) return o.value("function").toString();
    if (o.value("function").isObject()) return o.value("function").toObject().value("name").toString();
    return {};
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
