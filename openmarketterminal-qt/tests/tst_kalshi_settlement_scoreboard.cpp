#include "services/prediction/kalshi/KalshiSettlementScoreboard.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

using openmarketterminal::services::prediction::kalshi_ns::KalshiSettlementScoreboard;

namespace {

/// One exact-accounting one-sided settlement in the settlements_ready shape.
/// `average_entry` is dollars per contract; stake = average_entry * contracts.
QJsonObject settlement(double average_entry, double pnl, const QString& settled_time,
                       double contracts = 10.0, const QString& side = QStringLiteral("YES")) {
    const bool yes = side == QStringLiteral("YES");
    return QJsonObject{
        {QStringLiteral("market_id"), QStringLiteral("KXTEST-1")},
        {QStringLiteral("accounting_status"), QStringLiteral("exact_one_sided")},
        {QStringLiteral("realized_pnl"), pnl},
        {QStringLiteral("stake"), average_entry * contracts},
        {QStringLiteral("yes_count"), yes ? contracts : 0.0},
        {QStringLiteral("no_count"), yes ? 0.0 : contracts},
        {QStringLiteral("side"), side},
        {QStringLiteral("market_result"), pnl >= 0.0 ? side : QStringLiteral("OTHER")},
        {QStringLiteral("settled_time"), settled_time},
    };
}

QJsonObject netted_mixed_settlement(const QString& settled_time) {
    QJsonObject row = settlement(0.50, -3.78, settled_time);
    row.insert(QStringLiteral("accounting_status"), QStringLiteral("exact_netted_mixed"));
    row.insert(QStringLiteral("yes_count"), 141.78);
    row.insert(QStringLiteral("no_count"), 141.78);
    return row;
}

QString iso(int second) {
    return QStringLiteral("2026-07-20T19:45:%1.000000Z")
        .arg(second, 2, 10, QLatin1Char('0'));
}

} // namespace

class TestKalshiSettlementScoreboard : public QObject {
    Q_OBJECT

  private slots:
    void cohort_boundaries();
    void cohort_assignment_and_hit_rates();
    void streaks_ordered_by_settled_time();
    void pushes_count_in_sample_but_not_streaks();
    void exclusions_are_counted_never_cohorted();
    void format_states_samples_and_exclusions();
    void format_reads_missing_when_empty();
};

void TestKalshiSettlementScoreboard::cohort_boundaries() {
    QCOMPARE(KalshiSettlementScoreboard::cohort_for_price(0.10), QStringLiteral("longshot"));
    QCOMPARE(KalshiSettlementScoreboard::cohort_for_price(0.35), QStringLiteral("longshot"));
    QCOMPARE(KalshiSettlementScoreboard::cohort_for_price(0.351), QStringLiteral("coinflip"));
    QCOMPARE(KalshiSettlementScoreboard::cohort_for_price(0.50), QStringLiteral("coinflip"));
    QCOMPARE(KalshiSettlementScoreboard::cohort_for_price(0.649), QStringLiteral("coinflip"));
    QCOMPARE(KalshiSettlementScoreboard::cohort_for_price(0.65), QStringLiteral("favorite"));
    QCOMPARE(KalshiSettlementScoreboard::cohort_for_price(0.95), QStringLiteral("favorite"));
}

void TestKalshiSettlementScoreboard::cohort_assignment_and_hit_rates() {
    // Longshots: 2 wins, 1 loss. Coin-flips: 1 loss (NO side). Favorites: 1 win.
    const QJsonArray rows{
        settlement(0.20, 8.00, iso(1)),
        settlement(0.30, 7.00, iso(2)),
        settlement(0.25, -2.50, iso(3)),
        settlement(0.50, -5.00, iso(4), 10.0, QStringLiteral("NO")),
        settlement(0.80, 2.00, iso(5)),
    };
    const auto board = KalshiSettlementScoreboard::compute(rows);
    QCOMPARE(board.longshot.wins, 2);
    QCOMPARE(board.longshot.losses, 1);
    QCOMPARE(board.longshot.sample(), 3);
    QCOMPARE(board.longshot.pnl, 12.50);
    QCOMPARE(board.longshot.hit_rate(), 2.0 / 3.0);
    QCOMPARE(board.coinflip.losses, 1);
    QCOMPARE(board.coinflip.sample(), 1);
    QCOMPARE(board.coinflip.hit_rate(), 0.0);
    QCOMPARE(board.favorite.wins, 1);
    QCOMPARE(board.favorite.sample(), 1);
    QCOMPARE(board.favorite.hit_rate(), 1.0);
    QCOMPARE(board.scored(), 5);
    QCOMPARE(board.excluded(), 0);
}

void TestKalshiSettlementScoreboard::streaks_ordered_by_settled_time() {
    // Chronological outcomes: W W W L L W W — but fed newest-first, the way
    // the settlements bridge emits them. Streaks must follow settled_time.
    const QJsonArray rows{
        settlement(0.50, 1.0, iso(7)),
        settlement(0.50, 1.0, iso(6)),
        settlement(0.50, -1.0, iso(5)),
        settlement(0.50, -1.0, iso(4)),
        settlement(0.50, 1.0, iso(3)),
        settlement(0.50, 1.0, iso(2)),
        settlement(0.50, 1.0, iso(1)),
    };
    const auto board = KalshiSettlementScoreboard::compute(rows);
    QCOMPARE(board.current_streak, 2);
    QCOMPARE(board.best_win_streak, 3);
    QCOMPARE(board.best_loss_streak, 2);

    // Same rows ending on losses: current streak flips negative.
    const QJsonArray losing{
        settlement(0.50, -1.0, iso(3)),
        settlement(0.50, -1.0, iso(2)),
        settlement(0.50, 1.0, iso(1)),
    };
    const auto losing_board = KalshiSettlementScoreboard::compute(losing);
    QCOMPARE(losing_board.current_streak, -2);
    QCOMPARE(losing_board.best_win_streak, 1);
    QCOMPARE(losing_board.best_loss_streak, 2);
}

void TestKalshiSettlementScoreboard::pushes_count_in_sample_but_not_streaks() {
    // W, push, W in chronological order: the zero-P/L settlement neither
    // breaks nor extends the win streak, but it is in the cohort sample.
    const QJsonArray rows{
        settlement(0.50, 1.0, iso(1)),
        settlement(0.50, 0.0, iso(2)),
        settlement(0.50, 1.0, iso(3)),
    };
    const auto board = KalshiSettlementScoreboard::compute(rows);
    QCOMPARE(board.coinflip.wins, 2);
    QCOMPARE(board.coinflip.pushes, 1);
    QCOMPARE(board.coinflip.sample(), 3);
    QCOMPARE(board.coinflip.hit_rate(), 1.0);
    QCOMPARE(board.current_streak, 2);
    QCOMPARE(board.best_win_streak, 2);
    QCOMPARE(board.best_loss_streak, 0);
}

void TestKalshiSettlementScoreboard::exclusions_are_counted_never_cohorted() {
    // A legacy raw row (no accounting_status/realized_pnl) and a zero-contract
    // row are incomplete; two netted mixed rows are inexact. None may reach a
    // cohort or the streaks — and every one of them must be counted.
    QJsonObject legacy_raw{{QStringLiteral("ticker"), QStringLiteral("KXOLD-1")},
                           {QStringLiteral("yes_count_fp"), QStringLiteral("5.00")},
                           {QStringLiteral("settled_time"), iso(1)}};
    QJsonObject zero_contracts = settlement(0.50, 1.0, iso(2));
    zero_contracts.insert(QStringLiteral("yes_count"), 0.0);
    zero_contracts.insert(QStringLiteral("no_count"), 0.0);
    const QJsonArray rows{
        netted_mixed_settlement(iso(3)),
        netted_mixed_settlement(iso(4)),
        legacy_raw,
        zero_contracts,
        settlement(0.50, -1.0, iso(5)),
    };
    const auto board = KalshiSettlementScoreboard::compute(rows);
    QCOMPARE(board.excluded_mixed, 2);
    QCOMPARE(board.excluded_incomplete, 2);
    QCOMPARE(board.excluded(), 4);
    QCOMPARE(board.scored(), 1);
    QCOMPARE(board.coinflip.losses, 1);
    QCOMPARE(board.current_streak, -1);
}

void TestKalshiSettlementScoreboard::format_states_samples_and_exclusions() {
    const QJsonArray rows{
        settlement(0.20, 8.00, iso(1)),
        settlement(0.20, -2.00, iso(2)),
        settlement(0.80, 2.00, iso(3)),
        netted_mixed_settlement(iso(4)),
    };
    const QString text =
        KalshiSettlementScoreboard::format(KalshiSettlementScoreboard::compute(rows));
    QVERIFY(text.contains(QStringLiteral("LONGSHOT ≤35c · W 1 / L 1 · HIT 50.0% · P/L +$6.00 · n=2")));
    QVERIFY(text.contains(QStringLiteral("COIN-FLIP 35–65c · no settlements · n=0")));
    QVERIFY(text.contains(QStringLiteral("FAVORITE ≥65c · W 1 / L 0 · HIT 100.0% · P/L +$2.00 · n=1")));
    QVERIFY(text.contains(QStringLiteral("STREAK now W1 · best W1 / worst L1")));
    QVERIFY(text.contains(QStringLiteral("EXCLUDED 1: 1 mixed YES/NO turnover")));
}

void TestKalshiSettlementScoreboard::format_reads_missing_when_empty() {
    const QString text =
        KalshiSettlementScoreboard::format(KalshiSettlementScoreboard::compute(QJsonArray{}));
    QVERIFY(text.contains(QStringLiteral("no exact-accounting settlements yet")));
    QVERIFY(text.contains(QStringLiteral("STREAK · no decided settlements yet")));
    QVERIFY(text.contains(QStringLiteral("EXCLUDED 0")));
    // An empty scoreboard must never fabricate a 0% hit rate.
    QVERIFY(!text.contains(QStringLiteral("HIT 0.0%")));
}

QTEST_APPLESS_MAIN(TestKalshiSettlementScoreboard)
#include "tst_kalshi_settlement_scoreboard.moc"
