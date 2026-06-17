#pragma once
// Salvage "bare JSON" tool calls that weak/local models (e.g. llama3.x via Ollama)
// emit as plain content instead of structured tool_calls — so the analysis loop
// continues instead of silently stalling. Pure + testable; the LLM tool loop calls
// this as an extra detection pattern.
//
// Handles shapes like:
//   {"name":"edgar_get_financials","arguments":{"ticker":"AAPL"}}
//   {"type":"function","name":"get_quote","parameters":{"symbol":"AAPL"}}
//   {"function":{"name":"t","arguments":{...}}}
// also inside a ```json fenced block. Args may be under arguments|parameters|input
// (object or stringified JSON). A call is only returned when its tool name is in
// `known` — this prevents treating a legitimate JSON answer as a tool call.
#include <QJsonObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QVector>

namespace openmarketterminal::ai_chat {

QVector<QPair<QString, QJsonObject>> parse_bare_json_tool_calls(const QString& content,
                                                                const QSet<QString>& known);

} // namespace openmarketterminal::ai_chat
