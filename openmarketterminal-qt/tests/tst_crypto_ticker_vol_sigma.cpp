#include "screens/crypto_trading/CryptoTickerVolSigma.h"
#include <QtTest/QtTest>

using namespace openmarketterminal::crypto;
namespace radar = openmarketterminal::services::edge_radar;

// Issue #97: the crypto ticker bar surfaces ambient realized vol and the
// recent move in sigmas beside bid/ask/spread. The readout must be honest —
// warming and unavailable states render "--" (never a fabricated 0.0σ) and
// the sigma math is the shared scalp-gate helper, not a re-derivation.
class TstCryptoTickerVolSigma : public QObject {
    Q_OBJECT

    static TickerVolState ready_vol(double per_min_bps, int samples) {
        TickerVolState vol;
        vol.ready = true;
        vol.vol_per_min_bps = per_min_bps;
        vol.sample_count = samples;
        return vol;
    }

    // Ticks 1s apart ending at start + count seconds, drifting up `step` per tick.
    static TickerMoveWindow fed_window(qint64 start_ms, int count, double base, double step) {
        TickerMoveWindow w;
        for (int i = 0; i <= count; ++i)
            w.record(start_ms + qint64(i) * 1000, base + step * i);
        return w;
    }

  private slots:
    void available_shows_vol_and_sigma_from_shared_helper() {
        const qint64 t0 = 1'750'000'000'000;
        auto w = fed_window(t0, 60, 100000.0, 1.0);
        const qint64 now = t0 + 60'000;
        const auto m = w.move(now);
        QVERIFY(m.available);
        QCOMPARE(m.window_sec, 60);
        // move: (last / baseline − 1) in bps over the 60s span
        QVERIFY(std::abs(m.move_bps - (100060.0 / 100000.0 - 1.0) * 10000.0) < 1e-9);

        const auto d = format_ticker_vol_sigma(ready_vol(4.2, 480), m);
        QVERIFY2(d.text.contains("VOL 4.2bps/m"), qPrintable(d.text));
        // Sigma must equal the scalp gate's helper on the same inputs — the
        // gate computes edge_move_noise_sigma == services::edge_radar::move_noise_sigma.
        const double expect = radar::move_noise_sigma(m.move_bps, 4.2, m.window_sec);
        QVERIFY(expect > 0.0);
        QVERIFY2(d.text.contains(QString("1m %1σ").arg(expect, 0, 'f', 1)), qPrintable(d.text));
        QVERIFY2(d.tooltip.contains("480"), qPrintable(d.tooltip));
    }

    void warming_move_window_shows_dashes_not_zero_sigma() {
        const qint64 t0 = 1'750'000'000'000;
        auto w = fed_window(t0, 10, 100000.0, 1.0); // only 10s of history
        const auto m = w.move(t0 + 10'000);
        QVERIFY(!m.available);
        QCOMPARE(m.coverage_sec, 10);

        const auto d = format_ticker_vol_sigma(ready_vol(4.2, 480), m);
        QVERIFY2(d.text.contains("VOL 4.2bps/m"), qPrintable(d.text));
        QVERIFY2(d.text.contains("--σ"), qPrintable(d.text));
        QVERIFY2(!d.text.contains("0.0σ"), qPrintable(d.text));
        QVERIFY2(d.tooltip.contains("warming"), qPrintable(d.tooltip));
        QVERIFY2(d.tooltip.contains("10"), qPrintable(d.tooltip)); // coverage so far
    }

    void unavailable_vol_shows_dashes_with_reason_and_sample_count() {
        TickerVolState vol; // not ready
        vol.sample_count = 12;
        vol.reason = QStringLiteral("need at least 31 timestamped minute prices");

        // Even with a fully warmed move window, no vol estimate → no sigma.
        const qint64 t0 = 1'750'000'000'000;
        auto w = fed_window(t0, 60, 100000.0, 1.0);
        const auto d = format_ticker_vol_sigma(vol, w.move(t0 + 60'000));
        QVERIFY2(d.text.contains("VOL --"), qPrintable(d.text));
        QVERIFY2(d.text.contains("--σ"), qPrintable(d.text));
        QVERIFY2(d.tooltip.contains("need at least 31"), qPrintable(d.tooltip));
        QVERIFY2(d.tooltip.contains("12"), qPrintable(d.tooltip));
    }

    void stale_window_does_not_fabricate_a_move() {
        const qint64 t0 = 1'750'000'000'000;
        auto w = fed_window(t0, 60, 100000.0, 1.0);
        // Stream stalls: everything in the window is now older than the max age.
        const auto m = w.move(t0 + 60'000 + qint64(kTickerMoveMaxAgeSec + 30) * 1000);
        QVERIFY(!m.available);
    }

    void clear_resets_the_window_on_symbol_switch() {
        const qint64 t0 = 1'750'000'000'000;
        auto w = fed_window(t0, 60, 100000.0, 1.0);
        QVERIFY(w.move(t0 + 60'000).available);
        w.clear();
        QVERIFY(!w.move(t0 + 60'000).available);
        QCOMPARE(w.move(t0 + 60'000).coverage_sec, 0);
    }

    void flat_prices_render_a_true_zero_sigma() {
        // A genuine 0.0σ (flat tape, vol available) is real data, not a
        // fabrication — it must render as 0.0σ, distinct from the "--" states.
        const qint64 t0 = 1'750'000'000'000;
        auto w = fed_window(t0, 60, 100000.0, 0.0);
        const auto m = w.move(t0 + 60'000);
        QVERIFY(m.available);
        QCOMPARE(m.move_bps, 0.0);
        const auto d = format_ticker_vol_sigma(ready_vol(4.2, 480), m);
        QVERIFY2(d.text.contains("1m 0.0σ"), qPrintable(d.text));
    }

    void short_but_sufficient_coverage_uses_actual_window_sec() {
        // Baseline at minimum coverage: sigma scales by the real elapsed span,
        // not a hardcoded 60 — sqrt-time honesty for partial windows.
        const qint64 t0 = 1'750'000'000'000;
        auto w = fed_window(t0, kTickerMoveMinCoverageSec, 100000.0, 1.0);
        const auto m = w.move(t0 + qint64(kTickerMoveMinCoverageSec) * 1000);
        QVERIFY(m.available);
        QCOMPARE(m.window_sec, kTickerMoveMinCoverageSec);
    }
};

QTEST_MAIN(TstCryptoTickerVolSigma)
#include "tst_crypto_ticker_vol_sigma.moc"
