#include <QtTest>
#include "services/llm/TextToolCallParser.h"

using openmarketterminal::ai_chat::parse_bare_json_tool_calls;

class TstTextToolCallParser : public QObject {
    Q_OBJECT
    QSet<QString> known_{"edgar_get_financials", "get_quote"};

private slots:
    void llama_shape_type_function_parameters() {
        // The exact shape llama3.3 emitted as plain text in the test.
        const auto c = parse_bare_json_tool_calls(
            R"({"type": "function", "name": "edgar_get_financials", "parameters": {"ticker": "AAPL"}})", known_);
        QCOMPARE(c.size(), 1);
        QCOMPARE(c[0].first, QString("edgar_get_financials"));
        QCOMPARE(c[0].second.value("ticker").toString(), QString("AAPL"));
    }

    void name_arguments_shape() {
        const auto c = parse_bare_json_tool_calls(
            R"({"name":"get_quote","arguments":{"symbol":"MSFT"}})", known_);
        QCOMPARE(c.size(), 1);
        QCOMPARE(c[0].first, QString("get_quote"));
        QCOMPARE(c[0].second.value("symbol").toString(), QString("MSFT"));
    }

    void arguments_as_stringified_json() {
        // NOTE: a plain escaped literal, not a raw string — moc's lexer mis-parses
        // `\"` inside R"(...)" and then fails to see this QObject at all.
        const auto c = parse_bare_json_tool_calls(
            "{\"name\":\"get_quote\",\"arguments\":\"{\\\"symbol\\\":\\\"NVDA\\\"}\"}", known_);
        QCOMPARE(c.size(), 1);
        QCOMPARE(c[0].second.value("symbol").toString(), QString("NVDA"));
    }

    void nested_function_object() {
        const auto c = parse_bare_json_tool_calls(
            R"({"function":{"name":"edgar_get_financials","arguments":{"ticker":"AMZN"}}})", known_);
        QCOMPARE(c.size(), 1);
        QCOMPARE(c[0].first, QString("edgar_get_financials"));
        QCOMPARE(c[0].second.value("ticker").toString(), QString("AMZN"));
    }

    void fenced_json_block() {
        const auto c = parse_bare_json_tool_calls(
            "Sure, let me do that:\n```json\n{\"name\":\"get_quote\",\"parameters\":{\"symbol\":\"TSLA\"}}\n```", known_);
        QCOMPARE(c.size(), 1);
        QCOMPARE(c[0].second.value("symbol").toString(), QString("TSLA"));
    }

    void unknown_tool_name_is_rejected() {
        // Guard: a name not in `known` must NOT be treated as a tool call.
        const auto c = parse_bare_json_tool_calls(
            R"({"name":"rm_rf_everything","arguments":{"x":1}})", known_);
        QVERIFY(c.isEmpty());
    }

    void legit_json_answer_is_not_a_tool_call() {
        // A normal JSON data answer (no tool name) must be left alone.
        const auto c = parse_bare_json_tool_calls(
            R"({"revenue": 416000000000, "ticker": "AAPL", "note": "fundamentals"})", known_);
        QVERIFY(c.isEmpty());
    }

    void plain_prose_yields_nothing() {
        QVERIFY(parse_bare_json_tool_calls("Apple's revenue grew 8% to $416B last year.", known_).isEmpty());
    }

    void empty_known_set_is_safe() {
        QVERIFY(parse_bare_json_tool_calls(R"({"name":"get_quote","arguments":{}})", {}).isEmpty());
    }
};

QTEST_MAIN(TstTextToolCallParser)
#include "tst_text_tool_call_parser.moc"
