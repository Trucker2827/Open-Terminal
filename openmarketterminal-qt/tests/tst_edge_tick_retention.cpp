#include "services/edge_radar/EdgePredictionModel.h"

#include <QtTest>

using namespace openmarketterminal::services::edge_radar;

// Retention on edge_prediction_raw_ticks. The table grew to 43.3M rows / 10GB
// unpruned, which is what made an unindexed scan expensive enough to starve the
// Kalshi planner. These tests pin WHICH sources may be deleted -- getting that
// wrong destroys irreplaceable research data.
class TstEdgeTickRetention : public QObject {
    Q_OBJECT
  private slots:
    // The settlement feed must never be prunable. Our BRTI capture IS the
    // Kalshi settlement reference; losing it invalidates every settlement
    // -average and lead-lag result we can still reproduce.
    void settlement_feed_is_never_prunable() {
        QVERIFY(!edge_tick_source_is_prunable(QStringLiteral("cfbenchmarks:BRTI")));
        QVERIFY(!edge_tick_source_is_prunable(QStringLiteral("CFBENCHMARKS:BRTI")));
    }

    // The long-history series and the spot aggregate the live planner reads.
    void long_history_and_spot_aggregate_sources_are_kept() {
        QVERIFY(!edge_tick_source_is_prunable(QStringLiteral("coinbase-1m-close")));
        QVERIFY(!edge_tick_source_is_prunable(QStringLiteral("coinbase")));
        QVERIFY(!edge_tick_source_is_prunable(QStringLiteral("kraken")));
    }

    // Only the high-volume tape whose research value is already settled:
    // binanceperp lead-lag was closed as "wrong timescale, needs HFT infra",
    // gemini is unused. Together they were 46% of the table.
    void only_settled_high_volume_tape_is_prunable() {
        QVERIFY(edge_tick_source_is_prunable(QStringLiteral("binanceperp")));
        QVERIFY(edge_tick_source_is_prunable(QStringLiteral("gemini")));
        QVERIFY(edge_tick_source_is_prunable(QStringLiteral("  BinancePerp  ")));
        const QStringList sources = edge_tick_prunable_sources();
        QCOMPARE(sources.size(), 2);
        QVERIFY(sources.contains(QStringLiteral("binanceperp")));
        QVERIFY(sources.contains(QStringLiteral("gemini")));
    }

    // An unknown source is kept. New collectors must be opted IN to deletion
    // deliberately, never swept up by default.
    void unknown_sources_default_to_kept() {
        QVERIFY(!edge_tick_source_is_prunable(QStringLiteral("some-new-venue")));
        QVERIFY(!edge_tick_source_is_prunable(QString()));
    }

    void cutoff_is_now_minus_the_window() {
        constexpr qint64 kNow = 1'786'000'000'000LL;
        QCOMPARE(edge_tick_retention_cutoff_ms(kNow, 7), kNow - 7LL * 86'400'000LL);
        QCOMPARE(edge_tick_retention_cutoff_ms(kNow, 1), kNow - 86'400'000LL);
    }

    // A misconfigured window must delete NOTHING rather than everything --
    // cutoff 0 means "no row is older than the cutoff".
    void non_positive_window_deletes_nothing() {
        constexpr qint64 kNow = 1'786'000'000'000LL;
        QCOMPARE(edge_tick_retention_cutoff_ms(kNow, 0), 0LL);
        QCOMPARE(edge_tick_retention_cutoff_ms(kNow, -30), 0LL);
    }
};

QTEST_APPLESS_MAIN(TstEdgeTickRetention)
#include "tst_edge_tick_retention.moc"
