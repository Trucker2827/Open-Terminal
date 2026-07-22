#include <QtTest>
#include "cli/EdgeJournalShared.h"
using namespace openmarketterminal::cli;

// The scalp gate's noise floor: annualized realized vol -> per-minute bps,
// and an observed move expressed in sigmas of window-scaled ambient noise.
class TstEdgeVolGate : public QObject {
    Q_OBJECT
private slots:
    void annual_vol_converts_to_per_minute_bps() {
        // 50% annual over 365*24*60 minutes: 0.5/sqrt(525600)*10000 = 6.8967...
        QVERIFY(qAbs(edge_annual_vol_to_per_min_bps(0.5) - 0.5 / std::sqrt(525600.0) * 10000.0) < 1e-9);
        QVERIFY(qAbs(edge_annual_vol_to_per_min_bps(0.5) - 6.8967) < 1e-3);
        QCOMPARE(edge_annual_vol_to_per_min_bps(0.0), 0.0);
        QCOMPARE(edge_annual_vol_to_per_min_bps(-1.0), 0.0);
    }
    void sigma_scales_with_sqrt_time() {
        // 10bps ambient per minute -> 60s window vol is 10bps: a 20bps move = 2 sigma.
        QVERIFY(qAbs(edge_move_noise_sigma(20.0, 10.0, 60) - 2.0) < 1e-9);
        // Same move over a 15s window: window vol = 10*sqrt(0.25) = 5bps -> 4 sigma.
        QVERIFY(qAbs(edge_move_noise_sigma(20.0, 10.0, 15) - 4.0) < 1e-9);
        // Direction-agnostic.
        QVERIFY(qAbs(edge_move_noise_sigma(-20.0, 10.0, 60) - 2.0) < 1e-9);
    }
    void unavailable_vol_fails_open_as_zero() {
        QCOMPARE(edge_move_noise_sigma(20.0, 0.0, 60), 0.0);
        QCOMPARE(edge_move_noise_sigma(20.0, -1.0, 60), 0.0);
        QCOMPARE(edge_move_noise_sigma(20.0, 10.0, 0), 0.0);
    }
};
QTEST_MAIN(TstEdgeVolGate)
#include "tst_edge_vol_gate.moc"
