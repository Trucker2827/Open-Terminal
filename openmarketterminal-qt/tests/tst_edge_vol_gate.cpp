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

    // quant-signals.json consumer: trust must be earned AND fresh; absence
    // and staleness fail to a harmless present=false / trusted=false.
    void model_signal_parses_fresh_trusted() {
        const QJsonObject doc{
            {"model_id", "lgbm_x"}, {"generated_at_ms", 1'000'000.0}, {"trusted", true},
            {"trailing_ic", QJsonObject{{"rank_ic_mean", 0.03}}},
            {"signals", QJsonObject{{"BTC", QJsonObject{
                {"score", 0.42}, {"rank", 1}, {"direction", "up"}}}}}};
        const auto sig = edge_parse_model_signal(doc, "btc", 1'060'000, 900'000);
        QVERIFY(sig.present);
        QVERIFY(sig.fresh);
        QVERIFY(sig.trusted);
        QCOMPARE(sig.direction, QString("up"));
        QCOMPARE(sig.age_ms, qint64(60'000));
        QVERIFY(qAbs(sig.rank_ic - 0.03) < 1e-12);
    }
    void model_signal_stale_is_never_trusted() {
        const QJsonObject doc{
            {"model_id", "lgbm_x"}, {"generated_at_ms", 1'000'000.0}, {"trusted", true},
            {"signals", QJsonObject{{"BTC", QJsonObject{{"score", 0.1}, {"direction", "up"}}}}}};
        const auto sig = edge_parse_model_signal(doc, "BTC", 3'000'000, 900'000);
        QVERIFY(sig.present);
        QVERIFY(!sig.fresh);
        QVERIFY(!sig.trusted);   // publisher said trusted, but stale wins
    }
    void model_signal_missing_symbol_or_doc_is_absent() {
        QCOMPARE(edge_parse_model_signal(QJsonObject{}, "BTC", 0, 900'000).present, false);
        const QJsonObject doc{{"signals", QJsonObject{{"ETH", QJsonObject{{"score", 1.0}}}}}};
        QCOMPARE(edge_parse_model_signal(doc, "BTC", 0, 900'000).present, false);
    }
    void model_signal_untrusted_publisher_stays_untrusted() {
        const QJsonObject doc{
            {"generated_at_ms", 1'000'000.0}, {"trusted", false},
            {"signals", QJsonObject{{"BTC", QJsonObject{{"score", 0.9}, {"direction", "down"}}}}}};
        const auto sig = edge_parse_model_signal(doc, "btc", 1'001'000, 900'000);
        QVERIFY(sig.present && sig.fresh);
        QVERIFY(!sig.trusted);
    }
};
QTEST_MAIN(TstEdgeVolGate)
#include "tst_edge_vol_gate.moc"
