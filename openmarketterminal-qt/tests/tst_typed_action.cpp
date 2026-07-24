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
    // --- Enter: opening from flat, both directions (symmetry) ---
    void enter_short_opening_sizes_by_conviction() {
        auto i = translate_action(ch(ActionType::Enter, 0.7), 0.0, -1, ActionParams{});  // max 10
        QVERIFY(i.has_value());
        QCOMPARE(i->value("side").toString(), QStringLiteral("sell"));
        QCOMPARE(i->value("quantity").toDouble(), 7.0);
        QCOMPARE(i->value("order_type").toString(), QStringLiteral("market"));
        QCOMPARE(i->value("symbol").toString(), QStringLiteral("BTC-USD"));
    }
    void enter_long_opening_sizes_by_conviction() {
        auto i = translate_action(ch(ActionType::Enter, 0.7), 0.0, +1, ActionParams{});  // max 10
        QVERIFY(i.has_value());
        QCOMPARE(i->value("side").toString(), QStringLiteral("buy"));
        QCOMPARE(i->value("quantity").toDouble(), 7.0);
        QCOMPARE(i->value("order_type").toString(), QStringLiteral("market"));
        QCOMPARE(i->value("symbol").toString(), QStringLiteral("BTC-USD"));
    }
    void enter_long_short_symmetry_on_open() {
        auto is = translate_action(ch(ActionType::Enter, 0.7), 0.0, -1, ActionParams{});
        auto il = translate_action(ch(ActionType::Enter, 0.7), 0.0, +1, ActionParams{});
        QVERIFY(is.has_value() && il.has_value());
        QCOMPARE(is->value("quantity").toDouble(), il->value("quantity").toDouble());
        QVERIFY(is->value("side").toString() != il->value("side").toString());
        QCOMPARE(il->value("side").toString(), QStringLiteral("buy"));
        QCOMPARE(is->value("side").toString(), QStringLiteral("sell"));
    }

    // --- Enter: conviction clamp ---
    void enter_conviction_clamps_over_one_long() {
        auto i = translate_action(ch(ActionType::Enter, 1.5), 0.0, +1, ActionParams{});
        QVERIFY(i.has_value());
        QCOMPARE(i->value("side").toString(), QStringLiteral("buy"));
        QCOMPARE(i->value("quantity").toDouble(), 10.0);  // clamped to 1.0 * 10
    }
    void enter_conviction_clamps_over_one_short() {
        auto i = translate_action(ch(ActionType::Enter, 1.5), 0.0, -1, ActionParams{});
        QVERIFY(i.has_value());
        QCOMPARE(i->value("side").toString(), QStringLiteral("sell"));
        QCOMPARE(i->value("quantity").toDouble(), 10.0);  // clamped to 1.0 * 10
    }
    void enter_zero_conviction_is_skip() {
        QVERIFY(!translate_action(ch(ActionType::Enter, 0.0), 0.0, +1, ActionParams{}).has_value());
        QVERIFY(!translate_action(ch(ActionType::Enter, 0.0), 0.0, -1, ActionParams{}).has_value());
    }

    // --- Enter: neutral/stale/missing/conflicting edge collapses to dir=0 -> nullopt ---
    void enter_neutral_direction_is_rejected_when_flat() {
        QVERIFY(!translate_action(ch(ActionType::Enter, 1.0), 0.0, 0, ActionParams{}).has_value());
    }
    void enter_neutral_direction_is_rejected_when_long() {
        QVERIFY(!translate_action(ch(ActionType::Enter, 1.0), 5.0, 0, ActionParams{}).has_value());
    }
    void enter_neutral_direction_is_rejected_when_short() {
        QVERIFY(!translate_action(ch(ActionType::Enter, 1.0), -5.0, 0, ActionParams{}).has_value());
    }

    // --- Enter: add to an existing position in the same direction ---
    void enter_adds_to_existing_long() {
        auto i = translate_action(ch(ActionType::Enter, 0.7), 5.0, +1, ActionParams{});
        QVERIFY(i.has_value());
        QCOMPARE(i->value("side").toString(), QStringLiteral("buy"));
        QCOMPARE(i->value("quantity").toDouble(), 7.0);
    }
    void enter_adds_to_existing_short() {
        auto i = translate_action(ch(ActionType::Enter, 0.7), -5.0, -1, ActionParams{});
        QVERIFY(i.has_value());
        QCOMPARE(i->value("side").toString(), QStringLiteral("sell"));
        QCOMPARE(i->value("quantity").toDouble(), 7.0);
    }

    // --- Enter: NO one-step reversal, both sides ---
    void enter_no_one_step_reversal_long_to_short() {
        QVERIFY(!translate_action(ch(ActionType::Enter, 1.0), 10.0, -1, ActionParams{}).has_value());
    }
    void enter_no_one_step_reversal_short_to_long() {
        QVERIFY(!translate_action(ch(ActionType::Enter, 1.0), -10.0, +1, ActionParams{}).has_value());
    }

    // --- Trim: sign-agnostic ---
    void trim_sells_half_of_long() {
        auto i = translate_action(ch(ActionType::Trim), 10.0, 0, ActionParams{});
        QVERIFY(i.has_value());
        QCOMPARE(i->value("side").toString(), QStringLiteral("sell"));
        QCOMPARE(i->value("quantity").toDouble(), 5.0);
    }
    void trim_buys_to_cover_half_of_short() {
        auto i = translate_action(ch(ActionType::Trim), -10.0, 0, ActionParams{});
        QVERIFY(i.has_value());
        QCOMPARE(i->value("side").toString(), QStringLiteral("buy"));
        QCOMPARE(i->value("quantity").toDouble(), 5.0);
    }
    void trim_flat_is_nothing() {
        QVERIFY(!translate_action(ch(ActionType::Trim), 0.0, 0, ActionParams{}).has_value());
    }

    // --- Exit: sign-agnostic, short-covering ---
    void exit_sells_all_of_long() {
        auto i = translate_action(ch(ActionType::Exit), 8.0, 0, ActionParams{});
        QVERIFY(i.has_value());
        QCOMPARE(i->value("side").toString(), QStringLiteral("sell"));
        QCOMPARE(i->value("quantity").toDouble(), 8.0);
    }
    void exit_buys_to_cover_all_of_short() {
        auto i = translate_action(ch(ActionType::Exit), -8.0, 0, ActionParams{});
        QVERIFY(i.has_value());
        QCOMPARE(i->value("side").toString(), QStringLiteral("buy"));
        QCOMPARE(i->value("quantity").toDouble(), 8.0);
    }
    void exit_flat_is_nothing() {
        QVERIFY(!translate_action(ch(ActionType::Exit), 0.0, 0, ActionParams{}).has_value());
    }
    void exit_long_short_symmetry() {
        auto il = translate_action(ch(ActionType::Exit), 8.0, 0, ActionParams{});
        auto is = translate_action(ch(ActionType::Exit), -8.0, 0, ActionParams{});
        QVERIFY(il.has_value() && is.has_value());
        QCOMPARE(il->value("quantity").toDouble(), is->value("quantity").toDouble());
        QVERIFY(il->value("side").toString() != is->value("side").toString());
        QCOMPARE(il->value("side").toString(), QStringLiteral("sell"));
        QCOMPARE(is->value("side").toString(), QStringLiteral("buy"));
    }

    // --- Hold/Skip ---
    void skip_is_nothing() {
        QVERIFY(!translate_action(ch(ActionType::Skip), 10.0, +1, ActionParams{}).has_value());
        QVERIFY(!translate_action(ch(ActionType::Skip), -10.0, -1, ActionParams{}).has_value());
        QVERIFY(!translate_action(ch(ActionType::Skip), 0.0, 0, ActionParams{}).has_value());
    }
};

QTEST_MAIN(TstTypedAction)
#include "tst_typed_action.moc"
