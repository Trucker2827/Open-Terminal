#include <QtTest>

#include <cmath>

#include "services/edge_radar/KalshiAutoEngine.h"

using namespace openmarketterminal::services::edge_radar;
namespace pr = openmarketterminal::services::prediction;

namespace {

pr::PredictionMarket above_market(const QString& ticker, const QString& event,
                                  double strike, double yes_bid, double yes_ask,
                                  qint64 close_ms) {
    pr::PredictionMarket market;
    market.key.exchange_id = QStringLiteral("kalshi");
    market.key.market_id = ticker;
    market.key.event_id = event;
    market.question = QStringLiteral("BTC price above %1").arg(strike);
    market.end_date_iso = QDateTime::fromMSecsSinceEpoch(close_ms, QTimeZone::UTC).toString(Qt::ISODate);
    market.active = true;
    market.outcomes = {{QStringLiteral("Yes"), ticker + QStringLiteral(":yes"), yes_bid},
                       {QStringLiteral("No"), ticker + QStringLiteral(":no"), 1.0 - yes_ask}};
    market.extras.insert(QStringLiteral("floor_strike"), strike);
    market.extras.insert(QStringLiteral("yes_ask_dollars"), yes_ask);
    market.extras.insert(QStringLiteral("no_ask_dollars"), 1.0 - yes_bid);
    market.extras.insert(QStringLiteral("status"), QStringLiteral("active"));
    market.extras.insert(QStringLiteral("price_level_structure"), QStringLiteral("linear_cent"));
    market.extras.insert(QStringLiteral("fractional_trading_enabled"), true);
    return market;
}

KalshiAutoContext context(qint64 now, double spot = 64000.0) {
    KalshiAutoContext out;
    out.spot = spot;
    out.annual_volatility = 0.65;
    out.volatility_ready = true;
    out.volatility_sample_count = 120;
    out.volatility_observed_at_ms = now - 100;
    out.volatility_source = QStringLiteral("test");
    out.decision_ts_ms = now;
    out.spot_observed_at_ms = now - 100;
    return out;
}

QVector<KalshiTimedPrice> minute_prices(qint64 now, int count, double return_size) {
    QVector<KalshiTimedPrice> out;
    out.reserve(count);
    double price = 64'000.0;
    for (int i = 0; i < count; ++i) {
        if (i > 0)
            price *= std::exp((i % 2 == 0 ? 1.0 : -1.0) * return_size);
        out.append(KalshiTimedPrice{now - (count - 1 - i) * 60'000LL, price});
    }
    return out;
}

} // namespace

class TstKalshiAutoEngine : public QObject {
    Q_OBJECT

  private slots:
    void realized_volatility_is_live_scaled_and_no_lookahead() {
        const qint64 now = 1'800'000'000'000LL;
        const auto low = KalshiAutoEngine::estimate_realized_volatility(
            minute_prices(now, 360, 0.0002), now);
        const auto high = KalshiAutoEngine::estimate_realized_volatility(
            minute_prices(now, 360, 0.0020), now);
        QVERIFY(low.ready);
        QVERIFY(high.ready);
        QVERIFY(high.annual_volatility > low.annual_volatility * 5.0);
        QVERIFY(low.sample_count >= 30);

        auto contaminated = minute_prices(now, 360, 0.0002);
        contaminated.append(KalshiTimedPrice{now + 60'000, 128'000.0});
        const auto without_future = KalshiAutoEngine::estimate_realized_volatility(
            contaminated, now);
        QCOMPARE(without_future.annual_volatility, low.annual_volatility);
        QCOMPARE(without_future.observed_at_ms, low.observed_at_ms);
    }

    void realized_volatility_fails_closed_when_stale_or_short() {
        const qint64 now = 1'800'000'000'000LL;
        const auto short_history = KalshiAutoEngine::estimate_realized_volatility(
            minute_prices(now, 20, 0.001), now);
        QVERIFY(!short_history.ready);
        QVERIFY(short_history.reason.contains(QStringLiteral("31")));

        const auto stale = KalshiAutoEngine::estimate_realized_volatility(
            minute_prices(now - 4 * 60'000, 60, 0.001), now);
        QVERIFY(!stale.ready);
        QVERIFY(stale.reason.contains(QStringLiteral("older")));
    }

    void distribution_separates_jumps_and_ignores_future() {
        const qint64 now = 1'800'000'000'000LL;
        auto prices = minute_prices(now, 180, 0.0004);
        prices[120].price *= 1.03;
        prices[121].price = prices[120].price * std::exp(-0.0004);
        const auto estimate = KalshiAutoEngine::estimate_distribution(prices, now);
        QVERIFY(estimate.ready);
        QVERIFY(estimate.diffusion_annual_volatility > 0.0);
        QVERIFY(estimate.jump_sample_count > 0);

        prices.append(KalshiTimedPrice{now + 60'000, 1'000'000.0});
        const auto no_lookahead = KalshiAutoEngine::estimate_distribution(prices, now);
        QCOMPARE(no_lookahead.diffusion_annual_volatility,
                 estimate.diffusion_annual_volatility);
        QCOMPARE(no_lookahead.jump_sample_count, estimate.jump_sample_count);
    }

    void bipower_variation_keeps_pi_over_two_constant() {
        const qint64 now = 1'800'000'000'000LL;
        constexpr double one_minute_return = 0.0005;
        const auto estimate = KalshiAutoEngine::estimate_distribution(
            minute_prices(now, 80, one_minute_return), now);
        const double expected = std::sqrt(M_PI / 2.0) * one_minute_return *
                                std::sqrt(365.0 * 24.0 * 60.0);
        QVERIFY(estimate.ready);
        QVERIFY(std::abs(estimate.diffusion_annual_volatility - expected) < 1e-10);
    }

    void settlement_average_uses_observed_final_window_not_endpoint_only() {
        const qint64 now = 1'800'000'000'000LL;
        auto ctx = context(now, 64'200.0);
        ctx.settlement_average.available = true;
        ctx.settlement_average.latest_index = 64'200.0;
        ctx.settlement_average.latest_index_observed_at_ms = now - 10;
        ctx.settlement_average.window_seconds = 60;
        ctx.settlement_average.observed_samples = 50;
        ctx.settlement_average.observed_sum = 50.0 * 63'900.0;
        double mean = 0.0;
        double stddev = 0.0;
        const double probability = KalshiAutoEngine::settlement_average_probability_above(
            ctx, 64'000.0, 10, &mean, &stddev);
        QVERIFY(mean < 64'000.0);
        QVERIFY(probability < 0.5);
        QVERIFY(stddev > 0.0);
    }

    void settlement_average_excludes_observations_before_final_window() {
        const qint64 now = 1'800'000'000'000LL;
        auto ctx = context(now, 64'200.0);
        ctx.settlement_average.available = true;
        ctx.settlement_average.latest_index = 64'200.0;
        ctx.settlement_average.latest_index_observed_at_ms = now - 1'000;
        ctx.settlement_average.window_seconds = 60;
        for (int seconds_ago = 60; seconds_ago >= 31; --seconds_ago)
            ctx.settlement_average.recent_observations.append(
                KalshiTimedPrice{now - seconds_ago * 1000LL, 63'900.0});
        for (int seconds_ago = 30; seconds_ago >= 1; --seconds_ago)
            ctx.settlement_average.recent_observations.append(
                KalshiTimedPrice{now - seconds_ago * 1000LL, 64'200.0});
        double mean = 0.0;
        const double probability = KalshiAutoEngine::settlement_average_probability_above(
            ctx, 64'150.0, 30, &mean, nullptr);
        QVERIFY(mean > 64'150.0);
        QVERIFY(probability > 0.5);
    }

    void settlement_average_never_counts_elapsed_plus_one_seconds() {
        const qint64 now = 1'800'000'000'000LL;
        auto ctx = context(now, 64'000.0);
        ctx.settlement_average.available = true;
        ctx.settlement_average.latest_index = 64'000.0;
        ctx.settlement_average.latest_index_observed_at_ms = now;
        ctx.settlement_average.window_seconds = 60;
        for (int seconds_ago = 30; seconds_ago >= 0; --seconds_ago)
            ctx.settlement_average.recent_observations.append(
                KalshiTimedPrice{now - seconds_ago * 1000LL, 64'000.0});
        double mean = 0.0;
        KalshiAutoEngine::settlement_average_probability_above(
            ctx, 64'000.0, 30, &mean, nullptr);
        QVERIFY2(std::abs(mean - 64'000.0) < 1e-6,
                 qPrintable(QStringLiteral("fencepost mean was %1").arg(mean, 0, 'f', 6)));
    }

    void settlement_average_variance_keeps_one_third_constant() {
        const qint64 now = 1'800'000'000'000LL;
        auto ctx = context(now, 64'000.0);
        ctx.annual_volatility = 0.60;
        ctx.settlement_average.available = true;
        ctx.settlement_average.latest_index = 64'000.0;
        ctx.settlement_average.latest_index_observed_at_ms = now;
        ctx.settlement_average.window_seconds = 60;
        double stddev = 0.0;
        KalshiAutoEngine::settlement_average_probability_above(
            ctx, 64'000.0, 60, nullptr, &stddev);
        const double expected = 64'000.0 * 0.60 *
            std::sqrt((60.0 / 3.0) / (365.0 * 24.0 * 60.0 * 60.0));
        QVERIFY(std::abs(stddev - expected) < 1e-10);
    }

    void empirical_distribution_loses_authority_as_settlement_locks() {
        const qint64 now = 1'800'000'000'000LL;
        auto ctx = context(now, 64'200.0);
        ctx.annual_volatility = 0.05;
        ctx.conditioned_returns_by_horizon.insert(
            QStringLiteral("15m"), QVector<double>(100, -0.10));
        const double probability = KalshiAutoEngine::settlement_average_probability_above(
            ctx, 64'000.0, 1, nullptr, nullptr);
        QVERIFY(probability >= 0.999 - 1e-12);
    }

    void isotonic_calibration_is_monotone_and_requires_market_outperformance() {
        QVector<KalshiCalibrationSample> strong;
        for (int i = 0; i < 80; ++i) {
            const double predicted = i < 40 ? 0.20 : 0.80;
            const double outcome = i < 40 ? (i % 10 == 0 ? 1.0 : 0.0)
                                          : (i % 10 == 0 ? 0.0 : 1.0);
            strong.append({predicted, 0.50, outcome, i + 1,
                           QStringLiteral("event-%1").arg(i)});
        }
        const auto model = KalshiAutoEngine::fit_isotonic_calibration(strong, 30);
        QVERIFY(model.ready);
        QVERIFY(model.learned_model_weight > 0.0);
        QVERIFY(KalshiAutoEngine::calibrated_probability(0.2, model.points) <=
                KalshiAutoEngine::calibrated_probability(0.8, model.points));

        QVector<KalshiCalibrationSample> too_small = strong.mid(0, 20);
        const auto unready = KalshiAutoEngine::fit_isotonic_calibration(too_small, 30);
        QVERIFY(!unready.ready);
        QCOMPARE(unready.learned_model_weight, 0.0);
    }

    void calibration_authority_requires_independent_events_and_oof_advantage() {
        QVector<KalshiCalibrationSample> repeated;
        QVector<KalshiCalibrationSample> null_model;
        for (int i = 0; i < 80; ++i) {
            repeated.append({i % 2 ? 0.8 : 0.2, 0.5, i % 2 ? 1.0 : 0.0,
                             i + 1, QStringLiteral("same-event")});
            null_model.append({0.5, 0.5, i % 2 ? 1.0 : 0.0, i + 1,
                               QStringLiteral("event-%1").arg(i)});
        }
        const auto correlated = KalshiAutoEngine::fit_isotonic_calibration(repeated, 30);
        QVERIFY(!correlated.ready);
        QCOMPARE(correlated.sample_count, 1);

        const auto skill_less = KalshiAutoEngine::fit_isotonic_calibration(null_model, 30);
        QVERIFY(skill_less.ready);
        QCOMPARE(skill_less.learned_model_weight, 0.0);
        QVERIFY(skill_less.conservative_advantage <= 0.0);
    }

    void cohort_calibration_groups_and_scores_each_cohort_independently() {
        QVector<QPair<QString, KalshiCalibrationSample>> tagged;
        for (int i = 0; i < 80; ++i) {
            const double outcome = i % 2 ? 1.0 : 0.0;
            // Skilled cohort: prediction tracks the outcome; market is uninformative.
            tagged.append({QStringLiteral("test/skilled"),
                           KalshiCalibrationSample{outcome > 0.5 ? 0.85 : 0.15, 0.5, outcome,
                                                   0, QStringLiteral("skilled-%1").arg(i)}});
            // Null cohort: prediction and market both flat; no edge.
            tagged.append({QStringLiteral("test/null"),
                           KalshiCalibrationSample{0.5, 0.5, outcome, 0,
                                                   QStringLiteral("null-%1").arg(i)}});
        }
        // Thin cohort: too few independent events to earn a verdict.
        for (int i = 0; i < 5; ++i)
            tagged.append({QStringLiteral("test/thin"),
                           KalshiCalibrationSample{0.6, 0.5, i % 2 ? 1.0 : 0.0, 0,
                                                   QStringLiteral("thin-%1").arg(i)}});

        const auto cohorts = KalshiAutoEngine::fit_cohort_calibration(tagged, 30);
        QCOMPARE(cohorts.size(), 3);  // grouping keeps the three cohorts distinct

        QMap<QString, KalshiCalibrationModel> by_name;
        for (const auto& entry : cohorts)
            by_name.insert(entry.cohort, entry.model);
        // Sorted by cohort key.
        QCOMPARE(cohorts.first().cohort, QStringLiteral("test/null"));

        QVERIFY(by_name.value(QStringLiteral("test/skilled")).ready);
        QVERIFY(by_name.value(QStringLiteral("test/skilled")).conservative_advantage > 0.0);

        QVERIFY(by_name.value(QStringLiteral("test/null")).ready);
        QVERIFY(by_name.value(QStringLiteral("test/null")).conservative_advantage <= 0.0);

        QVERIFY(!by_name.value(QStringLiteral("test/thin")).ready);
    }

    void unproven_calibration_bucket_shrinks_entirely_to_market() {
        const qint64 now = 1'800'000'000'000LL;
        auto ctx = context(now);
        ctx.calibration_gate_enabled = true;
        ctx.calibration_sample_count = 100;
        ctx.learned_model_weight = 0.75;
        KalshiCalibrationModel unrelated;
        unrelated.ready = true;
        unrelated.sample_count = 100;
        unrelated.learned_model_weight = 0.75;
        unrelated.points = {{0.2, 0.1, 50}, {0.8, 0.9, 50}};
        ctx.calibration_models.insert(QStringLiteral("15m/itm/early"), unrelated);
        const auto market = above_market(QStringLiteral("SHRINK"), QStringLiteral("EV"),
                                         64'000.0, 0.49, 0.51, now + 3'600'000);
        const auto point = KalshiAutoEngine::build_surface({market}, {}, ctx).first();
        QCOMPARE(point.model_weight, 0.0);
        QVERIFY(std::abs(point.fair_yes - 0.50) < 1e-9);
        QVERIFY(point.net_edge < 0.0);
    }

    void causal_timing_is_recorded_but_does_not_veto_an_otherwise_valid_entry() {
        const qint64 now = 1'800'000'000'000LL;
        auto ctx = context(now);
        ctx.causal_gate_enabled = true;
        ctx.minimum_causal_lag_ms = 250;
        const auto market = above_market(QStringLiteral("CAUSAL"), QStringLiteral("EV"),
                                         64'000.0, 0.49, 0.51, now + 3'600'000);
        pr::PredictionOrderBook yes;
        yes.asset_id = QStringLiteral("CAUSAL:yes");
        yes.last_update_ms = now - 10;
        yes.bids = {{0.49, 10.0}};
        yes.asks = {{0.51, 10.0}};
        pr::PredictionOrderBook no = yes;
        no.asset_id = QStringLiteral("CAUSAL:no");
        const auto rejected = KalshiAutoEngine::build_surface(
            {market}, {{yes.asset_id, yes}, {no.asset_id, no}}, ctx).first();
        QVERIFY(rejected.valid);
        QVERIFY(!rejected.causal_eligible);
        QVERIFY(rejected.causal_reason.contains(QStringLiteral("precede")));

        // Lead/lag did not demonstrate predictive skill in the independent
        // cohort analysis. It remains an auditable diagnostic, not a veto on
        // a candidate that clears executable economics and portfolio limits.
        auto candidate = rejected;
        candidate.selected_side = QStringLiteral("yes");
        candidate.selected_ask = 0.20;
        candidate.selected_bid = 0.19;
        candidate.selected_fair = 0.60;
        candidate.net_edge = 0.40;
        candidate.cross_horizon_consistent = true;
        KalshiPortfolioConstraints limits;
        limits.unit_notional = 1.0;
        limits.max_total_cost = 2.0;
        limits.max_worst_case_loss = 2.0;
        const auto diagnostic_plan = KalshiAutoEngine::optimize({candidate}, ctx, limits);
        QCOMPARE(diagnostic_plan.legs.size(), 1);
        QCOMPARE(diagnostic_plan.legs.first().ticker, QStringLiteral("CAUSAL"));

        yes.last_update_ms = now - 400;
        no.last_update_ms = now - 400;
        const auto eligible = KalshiAutoEngine::build_surface(
            {market}, {{yes.asset_id, yes}, {no.asset_id, no}}, ctx).first();
        QVERIFY(eligible.causal_eligible);
    }

    void surface_refuses_a_missing_live_volatility_estimate() {
        const qint64 now = 1'800'000'000'000LL;
        auto no_volatility = context(now);
        no_volatility.volatility_ready = false;
        no_volatility.annual_volatility = 0.0;
        no_volatility.volatility_reason = QStringLiteral("tick history stale");
        const auto market = above_market(QStringLiteral("NOVOL"), QStringLiteral("EV"), 64000,
                                         0.49, 0.50, now + 3600000);
        const auto surface = KalshiAutoEngine::build_surface({market}, {}, no_volatility);
        QCOMPARE(surface.size(), 1);
        QVERIFY(!surface.first().valid);
        QVERIFY(surface.first().rejection_reason.contains(QStringLiteral("tick history stale")));
    }

    void surface_refuses_missing_cf_reference_in_final_window() {
        const qint64 now = 1'800'000'000'000LL;
        auto ctx = context(now);
        ctx.settlement_average.available = false;
        ctx.settlement_average.window_seconds = 60;
        const auto market = above_market(QStringLiteral("NOCF"), QStringLiteral("EV"), 64000,
                                         0.49, 0.50, now + 30'000);
        const auto surface = KalshiAutoEngine::build_surface({market}, {}, ctx);
        QCOMPARE(surface.size(), 1);
        QVERIFY(!surface.first().valid);
        QVERIFY(surface.first().rejection_reason.contains(
            QStringLiteral("official settlement reference")));
    }

    void surface_is_monotone_across_hourly_thresholds() {
        const qint64 now = 1'800'000'000'000LL;
        QVector<pr::PredictionMarket> markets{
            above_market(QStringLiteral("LOW"), QStringLiteral("E1"), 63800, 0.55, 0.56, now + 3600000),
            above_market(QStringLiteral("MID"), QStringLiteral("E1"), 64000, 0.70, 0.71, now + 3600000),
            above_market(QStringLiteral("HIGH"), QStringLiteral("E1"), 64200, 0.20, 0.21, now + 3600000)};
        auto surface = KalshiAutoEngine::build_surface(markets, {}, context(now), QStringLiteral("E1"));
        QCOMPARE(surface.size(), 3);
        std::sort(surface.begin(), surface.end(), [](const auto& a, const auto& b) {
            return a.floor < b.floor;
        });
        QVERIFY(surface[0].fair_yes >= surface[1].fair_yes);
        QVERIFY(surface[1].fair_yes >= surface[2].fair_yes);
        QVERIFY(surface[0].market_curve_probability >= surface[1].market_curve_probability);
        QVERIFY(surface[1].market_curve_probability >= surface[2].market_curve_probability);
    }

    void future_cross_horizon_signal_is_excluded() {
        const qint64 now = 1'800'000'000'000LL;
        const auto market = above_market(QStringLiteral("ATM"), QStringLiteral("E2"), 64000,
                                         0.49, 0.50, now + 3600000);
        auto base = context(now);
        const double without_future = KalshiAutoEngine::build_surface({market}, {}, base).first().fair_yes;
        base.horizon_signals.append(KalshiHorizonSignal{
            QStringLiteral("15m"), 0.99, 1.0, 0.08, 200, QStringLiteral("future"), now + 1});
        const double with_future = KalshiAutoEngine::build_surface({market}, {}, base).first().fair_yes;
        QCOMPARE(with_future, without_future);

        base.horizon_signals.clear();
        base.horizon_signals.append(KalshiHorizonSignal{
            QStringLiteral("15m"), 0.99, 1.0, 0.08, 200, QStringLiteral("chronos2"), now - 1});
        const double with_past = KalshiAutoEngine::build_surface({market}, {}, base).first().fair_yes;
        QVERIFY(with_past > without_future);
        const auto audited = KalshiAutoEngine::build_surface({market}, {}, base).first();
        QCOMPARE(audited.context_sample_count, 200);
        QCOMPARE(audited.context_sources, QStringLiteral("chronos2"));
    }

    void conflicting_fresh_horizons_are_saved_but_not_traded() {
        const qint64 now = 1'800'000'000'000LL;
        auto ctx = context(now);
        ctx.horizon_signals = {
            KalshiHorizonSignal{QStringLiteral("15m"), 0.75, 0.90, 0.08, 200,
                                QStringLiteral("short"), now - 100},
            KalshiHorizonSignal{QStringLiteral("1h"), 0.25, 0.90, 0.08, 200,
                                QStringLiteral("long"), now - 100}};
        const auto market = above_market(QStringLiteral("CONFLICT"), QStringLiteral("E2"),
                                         64000, 0.20, 0.21, now + 3600000);
        const auto point = KalshiAutoEngine::build_surface({market}, {}, ctx).first();
        QVERIFY(point.valid);
        QVERIFY(!point.cross_horizon_consistent);
        const auto plan = KalshiAutoEngine::optimize({point}, ctx);
        QVERIFY(plan.legs.isEmpty());
    }

    void optimizer_builds_distinct_risk_bounded_portfolio() {
        const qint64 now = 1'800'000'000'000LL;
        QVector<pr::PredictionMarket> markets;
        for (int i = 0; i < 8; ++i) {
            markets.append(above_market(QStringLiteral("T%1").arg(i), QStringLiteral("E3"),
                                        63500 + i * 150, 0.05 + i * 0.06,
                                        0.06 + i * 0.06, now + 3600000));
        }
        auto surface = KalshiAutoEngine::build_surface(markets, {}, context(now), QStringLiteral("E3"));
        KalshiPortfolioConstraints limits;
        limits.max_positions = 5;
        limits.max_same_side = 4;
        limits.max_total_cost = 125.0;
        limits.max_worst_case_loss = 75.0;
        limits.minimum_net_edge = 0.01;
        const auto plan = KalshiAutoEngine::optimize(surface, context(now), limits);
        QVERIFY(plan.legs.size() <= 5);
        QVERIFY(plan.total_cost <= 125.0 + 1e-9);
        QVERIFY(plan.worst_case_pnl >= -75.0 - 1e-9);
        QSet<QString> tickers;
        for (const auto& leg : plan.legs) tickers.insert(leg.ticker);
        QCOMPARE(tickers.size(), plan.legs.size());
    }

    void optimizer_counts_existing_live_exposure_across_cycles() {
        KalshiSurfacePoint point;
        point.ticker = QStringLiteral("NEW-YES");
        point.kind = QStringLiteral("above");
        point.floor = 64000.0;
        point.selected_side = QStringLiteral("yes");
        point.selected_ask = 0.20;
        point.selected_fair = 0.70;
        point.net_edge = 0.50;
        point.valid = true;

        KalshiPortfolioConstraints limits;
        limits.max_positions = 2;
        limits.max_same_side = 2;
        limits.unit_notional = 1.0;
        limits.max_total_cost = 2.0;
        limits.max_worst_case_loss = 2.0;
        limits.max_net_directional_cost = 1.0;
        limits.minimum_net_edge = 0.03;

        auto ctx = context(1'800'000'000'000LL);
        ctx.existing_open_positions = 1;
        ctx.existing_yes_positions = 1;
        ctx.existing_total_cost = 1.90;
        ctx.existing_net_directional_cost = 0.90;
        const auto cost_blocked = KalshiAutoEngine::optimize({point}, ctx, limits);
        QVERIFY(cost_blocked.legs.isEmpty());

        ctx.existing_total_cost = 0.20;
        ctx.existing_net_directional_cost = 0.20;
        ctx.existing_market_ids = {point.ticker};
        const auto duplicate_blocked = KalshiAutoEngine::optimize({point}, ctx, limits);
        QVERIFY(duplicate_blocked.legs.isEmpty());

        ctx.existing_market_ids.clear();
        ctx.existing_open_positions = limits.max_positions;
        const auto slots_blocked = KalshiAutoEngine::optimize({point}, ctx, limits);
        QVERIFY(slots_blocked.legs.isEmpty());

        ctx.existing_open_positions = 0;
        ctx.exposure_snapshot_ready = false;
        ctx.exposure_snapshot_reason = QStringLiteral("stale account exposure");
        const auto snapshot_blocked = KalshiAutoEngine::optimize({point}, ctx, limits);
        QVERIFY(snapshot_blocked.legs.isEmpty());
        QVERIFY(snapshot_blocked.blockers.join(' ').contains(QStringLiteral("stale")));
    }

    void market_metadata_owns_cadence_near_resolution() {
        const qint64 now = 1'800'000'000'000LL;
        auto hourly = above_market(QStringLiteral("HOUR-CLOSE"), QStringLiteral("EH"),
                                   64000, 0.40, 0.41, now + 4 * 60 * 60 * 1000LL);
        hourly.category = QStringLiteral("Crypto Hourly");
        auto daily = above_market(QStringLiteral("DAY-CLOSE"), QStringLiteral("ED"),
                                  64000, 0.40, 0.41, now + 10 * 60 * 1000LL);
        daily.category = QStringLiteral("Crypto Daily");
        const auto surface = KalshiAutoEngine::build_surface({hourly, daily}, {}, context(now));
        QCOMPARE(surface.size(), 2);
        QHash<QString, QString> cadence_by_ticker;
        for (const auto& point : surface)
            cadence_by_ticker.insert(point.ticker, point.cadence);
        QCOMPARE(cadence_by_ticker.value(QStringLiteral("HOUR-CLOSE")), QStringLiteral("1h"));
        QCOMPARE(cadence_by_ticker.value(QStringLiteral("DAY-CLOSE")), QStringLiteral("1d"));
    }

    void scheduled_event_block_prevents_every_order_without_losing_surface_evidence() {
        const qint64 now = 1'800'000'000'000LL;
        auto ctx = context(now);
        ctx.event_risk_active = true;
        ctx.event_trading_blocked = true;
        ctx.event_risk_reason = QStringLiteral("CPI release window");
        const auto market = above_market(QStringLiteral("EVENT"), QStringLiteral("E3"),
                                         64000, 0.20, 0.21, now + 3600000);
        const auto surface = KalshiAutoEngine::build_surface({market}, {}, ctx);
        QCOMPARE(surface.size(), 1);
        QVERIFY(surface.first().valid);
        const auto plan = KalshiAutoEngine::optimize(surface, ctx);
        QVERIFY(plan.legs.isEmpty());
        QVERIFY(plan.blockers.join(' ').contains(QStringLiteral("CPI")));
    }

    void optimizer_reserves_a_slot_for_qualifying_above_contract() {
        KalshiSurfacePoint range;
        range.ticker = QStringLiteral("RANGE");
        range.kind = QStringLiteral("range");
        range.floor = 63900.0;
        range.cap = 64100.0;
        range.selected_side = QStringLiteral("yes");
        range.selected_ask = 0.10;
        range.selected_fair = 0.60;
        range.net_edge = 0.50;
        range.valid = true;

        KalshiSurfacePoint above = range;
        above.ticker = QStringLiteral("ABOVE");
        above.kind = QStringLiteral("above");
        above.floor = 64000.0;
        above.cap = 0.0;
        above.selected_fair = 0.35;
        above.net_edge = 0.25;

        KalshiPortfolioConstraints limits;
        limits.max_positions = 1;
        limits.max_same_side = 1;
        limits.unit_notional = 1.0;
        limits.max_total_cost = 5.0;
        limits.max_worst_case_loss = 5.0;
        limits.minimum_net_edge = 0.03;
        limits.exit_cost_reserve = 0.01;
        const auto plan = KalshiAutoEngine::optimize({range, above}, context(1'800'000'000'000LL), limits);
        QCOMPARE(plan.legs.size(), 1);
        QCOMPARE(plan.legs.first().ticker, QStringLiteral("ABOVE"));
    }

    void one_sided_one_cent_tail_is_not_actionable() {
        const qint64 now = 1'800'000'000'000LL;
        const auto market = above_market(QStringLiteral("TAIL"), QStringLiteral("E4"), 65000,
                                         0.0, 0.01, now + 3600000);
        const auto surface = KalshiAutoEngine::build_surface({market}, {}, context(now),
                                                             QStringLiteral("E4"));
        QCOMPARE(surface.size(), 1);
        QVERIFY(!surface.first().valid);
        QCOMPARE(surface.first().rejection_reason,
                 QStringLiteral("one-sided selected-side book"));
    }

    void micro_evidence_gate_is_time_conditioned_and_shared() {
        KalshiMicroEvidenceInput input{QStringLiteral("yes"), 0.50, 4.0, 0.62,
                                       0.08, 250, 31 * 60};
        auto result = KalshiAutoEngine::evaluate_micro_evidence(input);
        QVERIFY(result.eligible);
        QCOMPARE(result.required_edge, 0.05);

        input.edge_after_cost = 0.049;
        result = KalshiAutoEngine::evaluate_micro_evidence(input);
        QVERIFY(!result.eligible);
        QVERIFY(result.blockers.join(QLatin1Char(' ')).contains(QStringLiteral("time-conditioned")));

        input.edge_after_cost = 0.08;
        input.quote_age_ms = 5'001;
        result = KalshiAutoEngine::evaluate_micro_evidence(input);
        QVERIFY(!result.eligible);
        QVERIFY(result.blockers.join(QLatin1Char(' ')).contains(QStringLiteral("quote older")));

        input.quote_age_ms = 100;
        input.confidence = 0.49;
        result = KalshiAutoEngine::evaluate_micro_evidence(input);
        QVERIFY(!result.eligible);
        QVERIFY(result.blockers.join(QLatin1Char(' ')).contains(QStringLiteral("confidence")));

        input.confidence = 0.75;
        input.depth = 0.5;
        result = KalshiAutoEngine::evaluate_micro_evidence(input);
        QVERIFY(!result.eligible);
        QVERIFY(result.blockers.join(QLatin1Char(' ')).contains(QStringLiteral("depth")));
    }

    void default_portfolio_is_bounded_to_five_two_dollar_legs() {
        KalshiPortfolioConstraints limits;
        QCOMPARE(limits.max_positions, 5);
        QCOMPARE(limits.unit_notional, 2.0);
        QCOMPARE(limits.max_total_cost, 10.0);
        QCOMPARE(limits.max_worst_case_loss, 10.0);

        QVector<KalshiSurfacePoint> surface;
        for (int i = 0; i < 7; ++i) {
            KalshiSurfacePoint point;
            point.ticker = QStringLiteral("MICRO-%1").arg(i);
            point.kind = QStringLiteral("above");
            point.floor = 63'500.0 + i * 100.0;
            point.selected_side = i % 2 == 0 ? QStringLiteral("yes") : QStringLiteral("no");
            point.selected_ask = 0.40 + i * 0.02;
            point.selected_bid = point.selected_ask - 0.01;
            point.selected_fair = std::min(0.95, point.selected_ask + 0.20);
            point.net_edge = point.selected_fair - point.selected_ask;
            point.fee_per_contract = 0.01;
            point.valid = true;
            surface.append(point);
        }
        const auto plan = KalshiAutoEngine::optimize(
            surface, context(1'800'000'000'000LL), limits);
        QVERIFY(plan.legs.size() <= 5);
        QVERIFY(plan.total_cost <= 10.0 + 1e-9);
        for (const auto& leg : plan.legs)
            QVERIFY(leg.contracts * leg.entry_price <= 2.0 + 1e-9);
    }

    void replay_never_uses_future_frame_context() {
        const qint64 now = 1'800'000'000'000LL;
        const auto market = above_market(QStringLiteral("R1"), QStringLiteral("ER"), 64000,
                                         0.30, 0.31, now + 3600000);
        KalshiReplayFrame first;
        first.ts_ms = now;
        first.context = context(now);
        first.markets = {market};
        KalshiReplayFrame future = first;
        future.ts_ms = now + 60000;
        future.context = context(future.ts_ms, 65000.0);
        KalshiPortfolioConstraints limits;
        limits.minimum_net_edge = 0.0;
        const auto one = KalshiAutoEngine::replay({first}, {{QStringLiteral("ER"), 65000.0}}, limits);
        const auto two = KalshiAutoEngine::replay({first, future}, {{QStringLiteral("ER"), 65000.0}}, limits);
        QCOMPARE(one.plans.first().legs.size(), two.plans.first().legs.size());
        if (!one.plans.first().legs.isEmpty())
            QCOMPARE(one.plans.first().legs.first().ticker, two.plans.first().legs.first().ticker);
    }

    void compatibility_audit_requires_new_api_fields() {
        const qint64 now = 1'800'000'000'000LL;
        auto complete = above_market(QStringLiteral("C1"), QStringLiteral("EC"), 64000,
                                     0.49, 0.50, now + 3600000);
        QVERIFY(KalshiAutoEngine::compatibility_issues(complete).isEmpty());
        complete.extras.remove(QStringLiteral("price_level_structure"));
        QVERIFY(KalshiAutoEngine::compatibility_issues(complete).contains(
            QStringLiteral("missing price_level_structure")));
    }

    void fifty_hourly_scenarios_preserve_risk_and_surface_invariants() {
        const qint64 base_now = 1'800'000'000'000LL;
        for (int scenario = 0; scenario < 50; ++scenario) {
            const qint64 now = base_now + scenario * 60'000;
            const double spot = 62'000.0 + scenario * 75.0;
            QVector<pr::PredictionMarket> markets;
            for (int strike_index = 0; strike_index < 9; ++strike_index) {
                const double strike = spot - 600.0 + strike_index * 150.0;
                const double ask = std::clamp(0.82 - strike_index * 0.08 + (scenario % 3) * 0.005,
                                              0.03, 0.97);
                markets.append(above_market(QStringLiteral("S%1-%2").arg(scenario).arg(strike_index),
                                            QStringLiteral("H%1").arg(scenario), strike,
                                            std::max(0.01, ask - 0.02), ask, now + 3'600'000));
            }
            auto ctx = context(now, spot);
            ctx.horizon_signals.append(KalshiHorizonSignal{
                QStringLiteral("15m"), 0.45 + (scenario % 11) * 0.01, 0.7,
                0.09, 300 + scenario, QStringLiteral("scenario-model"), now - 500});
            auto surface = KalshiAutoEngine::build_surface(markets, {}, ctx,
                                                           QStringLiteral("H%1").arg(scenario));
            QVector<KalshiSurfacePoint> ordered = surface;
            std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
                return left.floor < right.floor;
            });
            for (int i = 1; i < ordered.size(); ++i)
                QVERIFY2(ordered[i - 1].fair_yes + 1e-12 >= ordered[i].fair_yes,
                         "above-threshold surface must be monotone");

            KalshiPortfolioConstraints limits;
            limits.max_positions = 5;
            limits.max_same_side = 4;
            limits.max_total_cost = 100.0;
            limits.max_worst_case_loss = 60.0;
            limits.minimum_net_edge = 0.0;
            const auto plan = KalshiAutoEngine::optimize(surface, ctx, limits);
            QVERIFY(plan.legs.size() <= limits.max_positions);
            QVERIFY(plan.total_cost <= limits.max_total_cost + 1e-9);
            QVERIFY(plan.worst_case_pnl >= -limits.max_worst_case_loss - 1e-9);
            QSet<QString> unique;
            int yes = 0;
            int no = 0;
            for (const auto& leg : plan.legs) {
                unique.insert(leg.ticker);
                leg.side == QStringLiteral("yes") ? ++yes : ++no;
                QVERIFY(leg.entry_price > 0.0 && leg.entry_price < 1.0);
            }
            QCOMPARE(unique.size(), plan.legs.size());
            QVERIFY(yes <= limits.max_same_side);
            QVERIFY(no <= limits.max_same_side);
        }
    }
};

QTEST_MAIN(TstKalshiAutoEngine)
#include "tst_kalshi_auto_engine.moc"
