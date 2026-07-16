#include <QtTest>
#include "services/ai_strategy/TypedAction.h"

using namespace openmarketterminal;
using ai_strategy::ActionType;
using ai_strategy::ActionChoice;
using ai_strategy::ActionParams;
using ai_strategy::translate_action;

class TstTypedAction : public QObject {
    Q_OBJECT
    static ActionChoice ch(ActionType a, double conv = 1.0, const QString& sym = QStringLiteral("BTC-USD")) {
        return ActionChoice{sym, a, conv};
    }
  private slots:
    void enter_sizes_by_conviction() {
        auto i = translate_action(ch(ActionType::Enter, 0.7), 0.0, ActionParams{});  // max 10
        QVERIFY(i.has_value());
        QCOMPARE(i->value("side").toString(), QStringLiteral("buy"));
        QCOMPARE(i->value("quantity").toDouble(), 7.0);
        QCOMPARE(i->value("order_type").toString(), QStringLiteral("market"));
        QCOMPARE(i->value("symbol").toString(), QStringLiteral("BTC-USD"));
    }
    void enter_full_conviction() {
        auto i = translate_action(ch(ActionType::Enter, 1.0), 0.0, ActionParams{});
        QVERIFY(i.has_value());
        QCOMPARE(i->value("quantity").toDouble(), 10.0);
    }
    void enter_clamps_over_one() {
        auto i = translate_action(ch(ActionType::Enter, 1.5), 0.0, ActionParams{});
        QVERIFY(i.has_value());
        QCOMPARE(i->value("quantity").toDouble(), 10.0);   // clamped to 1.0 * 10
    }
    void enter_zero_conviction_is_skip() {
        QVERIFY(!translate_action(ch(ActionType::Enter, 0.0), 0.0, ActionParams{}).has_value());
    }
    void trim_sells_half_of_long() {
        auto i = translate_action(ch(ActionType::Trim), 10.0, ActionParams{});
        QVERIFY(i.has_value());
        QCOMPARE(i->value("side").toString(), QStringLiteral("sell"));
        QCOMPARE(i->value("quantity").toDouble(), 5.0);
    }
    void trim_flat_is_nothing() {
        QVERIFY(!translate_action(ch(ActionType::Trim), 0.0, ActionParams{}).has_value());
    }
    void trim_short_is_nothing() {
        QVERIFY(!translate_action(ch(ActionType::Trim), -8.0, ActionParams{}).has_value());
    }
    void exit_sells_all_of_long() {
        auto i = translate_action(ch(ActionType::Exit), 8.0, ActionParams{});
        QVERIFY(i.has_value());
        QCOMPARE(i->value("side").toString(), QStringLiteral("sell"));
        QCOMPARE(i->value("quantity").toDouble(), 8.0);
    }
    void exit_flat_and_short_are_nothing() {
        QVERIFY(!translate_action(ch(ActionType::Exit), 0.0, ActionParams{}).has_value());
        QVERIFY(!translate_action(ch(ActionType::Exit), -3.0, ActionParams{}).has_value());
    }
    void skip_is_nothing() {
        QVERIFY(!translate_action(ch(ActionType::Skip), 10.0, ActionParams{}).has_value());
    }
};

QTEST_MAIN(TstTypedAction)
#include "tst_typed_action.moc"
