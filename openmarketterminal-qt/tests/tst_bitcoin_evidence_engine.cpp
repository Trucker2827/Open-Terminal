#include "services/edge_radar/BitcoinEvidenceEngine.h"

#include <QtTest>

using namespace openmarketterminal::services::edge_radar;

namespace {

constexpr qint64 kNow = 1'800'000'000'000LL;

QVector<EvidenceTimedValue> prices(double first, double last, qint64 end = kNow) {
    QVector<EvidenceTimedValue> out;
    for (int i = 0; i < 12; ++i)
        out.append({end - (11 - i) * 10'000, first + (last - first) * i / 11.0});
    return out;
}

BitcoinEvidenceInput base_input() {
    BitcoinEvidenceInput input;
    input.decision_ts_ms = kNow;
    input.news_ts_ms = kNow - 60'000;
    input.news_verdict = QStringLiteral("UP");
    input.catalysts = {QStringLiteral("ETF/FLOWS")};
    input.current_headlines = {QStringLiteral("Bitcoin ETF records new inflow")};
    input.news_score = 45.0;
    input.news_confidence = 0.80;
    input.reference_spot = 64'000.0;
    input.current_spot = 64'100.0;
    input.target_price = 64'000.0;
    input.model_probability = 0.80;
    input.market_ask = 0.70;
    input.round_trip_cost = 0.01;
    input.seconds_left = 900;
    input.btc_prices = prices(64'000.0, 64'100.0);
    return input;
}

EvidenceHistorySample sample(qint64 ts, bool up, bool traded = true) {
    EvidenceHistorySample out;
    out.observed_at_ms = ts;
    out.verdict = QStringLiteral("UP");
    out.regime = QStringLiteral("TRENDING UP");
    out.time_bucket = QStringLiteral("10_30M");
    out.catalysts = {QStringLiteral("ETF/FLOWS")};
    out.predicted_probability = up ? 0.80 : 0.65;
    out.news_score = up ? 50.0 : -20.0;
    out.novelty = up ? 0.9 : 0.2;
    out.reaction = up ? 0.8 : -0.6;
    out.cross_confirmation = up ? 0.7 : -0.5;
    out.actual_return_pct = up ? 0.4 : -0.3;
    out.net_pnl = up ? 2.0 : -1.0;
    out.outcome_up = up;
    out.outcome_success = up;
    out.traded = traded;
    return out;
}

} // namespace

class TstBitcoinEvidenceEngine : public QObject {
    Q_OBJECT
  private slots:
    void narrative_impact_memory_uses_matching_analogs() {
        auto input = base_input();
        for (int i = 0; i < 12; ++i) input.history.append(sample(kNow - (i + 1) * 1000, true));
        const auto result = BitcoinEvidenceEngine::analyze(input);
        QCOMPARE(result.impact_memory.value("analog_samples").toInt(), 12);
        QCOMPARE(result.impact_memory.value("status").toString(), QStringLiteral("USABLE"));
        QVERIFY(result.impact_memory.value("average_return_pct").toDouble() > 0.0);
        QCOMPARE(result.impact_memory.value("horizons").toArray().size(), 5);
    }

    void market_reaction_distinguishes_confirmation_and_rejection() {
        auto input = base_input();
        QCOMPARE(BitcoinEvidenceEngine::analyze(input).market_reaction.value("label").toString(),
                 QStringLiteral("CONFIRMED"));
        input.current_spot = 63'800.0;
        QCOMPARE(BitcoinEvidenceEngine::analyze(input).market_reaction.value("label").toString(),
                 QStringLiteral("REJECTED"));
    }

    void novelty_decays_repeated_headlines() {
        auto input = base_input();
        input.historical_headlines = {QStringLiteral("Bitcoin ETF records a new inflow")};
        const auto repeated = BitcoinEvidenceEngine::analyze(input).novelty;
        input.historical_headlines = {QStringLiteral("Unrelated mining hardware earnings")};
        const auto fresh = BitcoinEvidenceEngine::analyze(input).novelty;
        QVERIFY(fresh.value("score").toDouble() > repeated.value("score").toDouble());
    }

    void regime_engine_detects_trend_and_volatility() {
        auto input = base_input();
        input.btc_prices = prices(63'000.0, 64'100.0);
        QCOMPARE(BitcoinEvidenceEngine::analyze(input).regime.value("label").toString(),
                 QStringLiteral("TRENDING UP"));
        input.btc_prices = {{kNow - 50'000, 64'000}, {kNow - 40'000, 64'500},
                            {kNow - 30'000, 63'500}, {kNow - 20'000, 64'700},
                            {kNow - 10'000, 63'400}, {kNow, 64'100}};
        QCOMPARE(BitcoinEvidenceEngine::analyze(input).regime.value("label").toString(),
                 QStringLiteral("HIGH VOLATILITY"));
    }

    void cross_market_confirmation_counts_agreement() {
        auto input = base_input();
        input.cross_market_prices.insert(QStringLiteral("ETH"), prices(1800, 1810));
        input.cross_market_prices.insert(QStringLiteral("SOL"), prices(100, 101));
        input.order_book_imbalance = 0.3;
        const auto cross = BitcoinEvidenceEngine::analyze(input).cross_market;
        QCOMPARE(cross.value("confirming").toInt(), 3);
        QVERIFY(cross.value("score").toDouble() > 0.9);
    }

    void kalshi_lag_is_timestamped() {
        auto input = base_input();
        input.btc_prices = {{kNow - 5000, 64000}, {kNow - 4000, 64040}, {kNow - 3000, 64050}};
        input.kalshi_probabilities = {{kNow - 5000, 0.50}, {kNow - 2500, 0.54}};
        const auto lag = BitcoinEvidenceEngine::analyze(input).kalshi_lag;
        QCOMPARE(lag.value("label").toString(), QStringLiteral("KALSHI LAGGED"));
        QCOMPARE(lag.value("lag_ms").toInteger(), 1500);
    }

    void replay_reports_opposite_and_early_exit_counterfactuals() {
        auto input = base_input();
        input.history = {sample(kNow - 2000, true), sample(kNow - 1000, false)};
        const auto replay = BitcoinEvidenceEngine::analyze(input).replay;
        QCOMPARE(replay.value("chosen_net_pnl").toDouble(), 1.0);
        QCOMPARE(replay.value("opposite_side_net_pnl").toDouble(), -1.0);
        QVERIFY(replay.contains("early_exit_proxy_pnl"));
    }

    void adaptive_weights_reward_proven_features() {
        auto input = base_input();
        for (int i = 0; i < 30; ++i) input.history.append(sample(kNow - (i + 1) * 1000, i % 2 == 0));
        const auto adaptive = BitcoinEvidenceEngine::analyze(input).adaptive_weights;
        QCOMPARE(adaptive.value("status").toString(), QStringLiteral("LEARNED"));
        const auto weights = adaptive.value("weights").toObject();
        QVERIFY(weights.value("reaction").toDouble() > 0.0);
    }

    void abstention_scores_avoided_losses() {
        auto input = base_input();
        for (int i = 0; i < 12; ++i) input.history.append(sample(kNow - (i + 1) * 1000, false, false));
        const auto abstention = BitcoinEvidenceEngine::analyze(input).abstention;
        QCOMPARE(abstention.value("avoided_losses").toInt(), 12);
        QVERIFY(abstention.value("value").toDouble() > 0.0);
        QCOMPARE(abstention.value("status").toString(), QStringLiteral("MEASURED"));
    }

    void calibration_is_separated_by_time_to_close() {
        auto input = base_input();
        for (int i = 0; i < 30; ++i) {
            auto row = sample(kNow - (i + 1) * 1000, i < 24);
            row.time_bucket = QStringLiteral("10_30M");
            input.history.append(row);
        }
        const auto calibration = BitcoinEvidenceEngine::analyze(input).calibration;
        QCOMPARE(calibration.value("time_bucket").toString(), QStringLiteral("10_30M"));
        QCOMPARE(calibration.value("samples").toInt(), 30);
        QCOMPARE(calibration.value("status").toString(), QStringLiteral("CALIBRATED SAMPLE"));
        QCOMPARE(calibration.value("bins").toArray().size(), 4);
    }

    void directional_returns_do_not_calibrate_contract_probabilities() {
        auto input = base_input();
        for (int i = 0; i < 30; ++i) {
            auto row = sample(kNow - (i + 1) * 1000, true);
            row.kind = QStringLiteral("directional");
            row.time_bucket = QStringLiteral("10_30M");
            input.history.append(row);
        }
        QCOMPARE(BitcoinEvidenceEngine::analyze(input).calibration.value("samples").toInt(), 0);
        for (auto& row : input.history) row.kind = QStringLiteral("settlement");
        QCOMPARE(BitcoinEvidenceEngine::analyze(input).calibration.value("samples").toInt(), 30);
    }

    void same_eighty_percent_changes_with_time_remaining() {
        auto long_time = base_input();
        long_time.seconds_left = 2700;
        long_time.current_spot = 64'050.0;
        auto short_time = long_time;
        short_time.seconds_left = 60;
        const double long_conf = BitcoinEvidenceEngine::analyze(long_time).calibration
                                     .value("time_conditioned_confidence").toDouble();
        const double short_conf = BitcoinEvidenceEngine::analyze(short_time).calibration
                                      .value("time_conditioned_confidence").toDouble();
        QVERIFY(short_conf > long_conf);
    }

    void future_evidence_is_rejected_everywhere() {
        auto input = base_input();
        input.history = {sample(kNow + 1, true)};
        input.btc_prices.append({kNow + 1, 70'000.0});
        input.kalshi_probabilities = {{kNow - 10, 0.5}, {kNow + 1, 0.9}};
        const auto result = BitcoinEvidenceEngine::analyze(input);
        QCOMPARE(result.impact_memory.value("analog_samples").toInt(), 0);
        QCOMPARE(result.replay.value("samples").toInt(), 0);
        QCOMPARE(result.kalshi_lag.value("label").toString(), QStringLiteral("NOT ENOUGH DATA"));
        QCOMPARE(BitcoinEvidenceEngine::to_json(result).value("no_lookahead_cutoff_ms").toString(),
                 QString::number(kNow));
    }

    void output_remains_advisory_only() {
        const auto json = BitcoinEvidenceEngine::to_json(BitcoinEvidenceEngine::analyze(base_input()));
        QCOMPARE(json.value("model_role").toString(), QStringLiteral("advisory_only"));
        QVERIFY(!json.value("can_trigger_order").toBool(true));
        QVERIFY(!json.value("gate").toObject().value("can_trigger_order").toBool(true));
    }
};

QTEST_APPLESS_MAIN(TstBitcoinEvidenceEngine)
#include "tst_bitcoin_evidence_engine.moc"
