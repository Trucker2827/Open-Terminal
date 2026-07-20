#pragma once

#include "services/prediction/PredictionTypes.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace openmarketterminal::services::edge_radar {

inline constexpr char kKalshiSettlementModelVersion[] = "kalshi-settlement-average-v3";

struct KalshiHorizonSignal {
    QString horizon;
    double up_probability = 0.5;
    double confidence = 0.0;
    double calibration_score = 0.0;
    int sample_count = 0;
    QString source;
    qint64 observed_at_ms = 0;
};

struct KalshiTimedPrice {
    qint64 observed_at_ms = 0;
    double price = 0.0;
};

struct KalshiVolatilityEstimate {
    bool ready = false;
    double annual_volatility = 0.0;
    double time_of_day_multiplier = 1.0;
    int sample_count = 0;
    qint64 observed_at_ms = 0;
    QString source = QStringLiteral("local_1m_ewma");
    QString reason;
};

struct KalshiDistributionEstimate {
    bool ready = false;
    double diffusion_annual_volatility = 0.0;
    double jump_intensity_per_hour = 0.0;
    double mean_absolute_jump_return = 0.0;
    int diffusion_sample_count = 0;
    int jump_sample_count = 0;
    qint64 observed_at_ms = 0;
    QString reason;
};

struct KalshiCalibrationSample {
    double predicted_probability = 0.5;
    double market_probability = 0.5;
    double outcome = 0.0;
    qint64 observed_at_ms = 0;
    QString independent_event_id;
};

struct KalshiCalibrationPoint {
    double predicted_probability = 0.5;
    double calibrated_probability = 0.5;
    int sample_count = 0;
};

struct KalshiCalibrationModel {
    bool ready = false;
    int sample_count = 0;
    double model_brier = 0.0;
    double market_brier = 0.0;
    double clustered_standard_error = 0.0;
    double conservative_advantage = 0.0;
    double learned_model_weight = 0.0;
    QVector<KalshiCalibrationPoint> points;
    QString reason;
};

struct KalshiCohortModel {
    QString cohort;
    KalshiCalibrationModel model;
};

struct KalshiSettlementAverageState {
    bool available = false;
    double latest_index = 0.0;
    qint64 latest_index_observed_at_ms = 0;
    int window_seconds = 60;
    int observed_samples = 0;
    double observed_sum = 0.0;
    QVector<KalshiTimedPrice> recent_observations;
    QString source = QStringLiteral("cfbenchmarks");
};

struct KalshiAutoContext {
    double spot = 0.0;
    double annual_volatility = 0.0;
    bool volatility_ready = false;
    double volatility_time_of_day_multiplier = 1.0;
    int volatility_sample_count = 0;
    qint64 volatility_observed_at_ms = 0;
    QString volatility_source;
    QString volatility_reason;
    qint64 decision_ts_ms = 0;
    qint64 spot_observed_at_ms = 0;
    qint64 max_spot_age_ms = 5'000;
    QVector<KalshiHorizonSignal> horizon_signals;
    KalshiSettlementAverageState settlement_average;
    KalshiDistributionEstimate distribution;
    QHash<QString, QVector<double>> conditioned_returns_by_horizon;
    QVector<KalshiCalibrationPoint> calibration_curve;
    QHash<QString, KalshiCalibrationModel> calibration_models;
    int calibration_sample_count = 0;
    double learned_model_weight = 0.0;
    bool calibration_gate_enabled = false;
    // Evidence collection may continue while this remains false. A calibrated
    // bucket only affects executable fair value after an explicit promotion.
    bool model_authority_enabled = false;
    int minimum_calibration_samples = 30;
    double spot_index_basis = 0.0;
    int basis_sample_count = 0;
    bool causal_gate_enabled = false;
    qint64 minimum_causal_lag_ms = 250;
    int existing_open_positions = 0;
    int existing_yes_positions = 0;
    int existing_no_positions = 0;
    double existing_total_cost = 0.0;
    double existing_net_directional_cost = 0.0;
    QStringList existing_market_ids;
    bool exposure_snapshot_ready = true;
    QString exposure_snapshot_reason;
    bool event_risk_active = false;
    bool event_trading_blocked = false;
    double event_volatility_multiplier = 1.0;
    QString event_risk_reason;
};

struct KalshiSurfacePoint {
    QString ticker;
    QString event_ticker;
    QString question;
    QString kind;
    QString cadence;
    qint64 settlement_ts_ms = 0;
    double floor = 0.0;
    double cap = 0.0;
    double fair_yes = 0.0;
    double fair_no = 0.0;
    double model_probability_raw = 0.0;
    double calibrated_probability = 0.0;
    double market_implied_probability = 0.0;
    double market_curve_probability = 0.0;
    double relative_value_residual = 0.0;
    double model_weight = 0.0;
    double yes_bid = 0.0;
    double yes_ask = 0.0;
    double no_bid = 0.0;
    double no_ask = 0.0;
    double yes_depth = 0.0;
    double no_depth = 0.0;
    QString selected_side;
    double selected_fair = 0.0;
    double selected_ask = 0.0;
    double selected_bid = 0.0;
    qint64 quote_observed_at_ms = 0;
    double fee_per_contract = 0.0;
    double net_edge = 0.0;
    double time_risk_score = 0.0;
    double directional_delta = 0.0;
    double confidence = 0.0;
    bool causal_eligible = false;
    qint64 causal_lag_ms = -1;
    QString causal_reason;
    bool cross_horizon_consistent = true;
    QString cross_horizon_reason;
    QString probability_source;
    QString calibration_bucket;
    int calibration_samples = 0;
    double settlement_mean = 0.0;
    double settlement_stddev = 0.0;
    qint64 context_freshness_ms = -1;
    int context_sample_count = 0;
    double context_calibration_score = 0.0;
    double context_up_probability = 0.5;
    QString context_sources;
    int seconds_left = -1;
    bool valid = false;
    QString rejection_reason;
};

struct KalshiPayoffPoint {
    double settlement_price = 0.0;
    double pnl = 0.0;
};

struct KalshiPortfolioLeg {
    QString ticker;
    QString side;
    QString kind;
    double floor = 0.0;
    double cap = 0.0;
    int contracts = 0;
    double entry_price = 0.0;
    double fair_probability = 0.0;
    double entry_fee = 0.0;
    double expected_profit = 0.0;
};

struct KalshiPortfolioConstraints {
    int max_positions = 5;
    int max_same_side = 4;
    double unit_notional = 2.0;
    double max_total_cost = 10.0;
    double max_worst_case_loss = 10.0;
    double minimum_net_edge = 0.03;
    double maximum_entry_price = 0.97;
    double exit_cost_reserve = 0.01;
    double max_net_directional_cost = 4.0;
};

struct KalshiMicroEvidenceInput {
    QString side;
    double ask = 0.0;
    double depth = 0.0;
    double confidence = 0.0;
    double edge_after_cost = 0.0;
    qint64 quote_age_ms = -1;
    int seconds_left = -1;
};

struct KalshiMicroEvidenceResult {
    bool eligible = false;
    double required_edge = 0.0;
    QStringList blockers;
};

struct KalshiPortfolioPlan {
    QVector<KalshiPortfolioLeg> legs;
    QVector<KalshiPayoffPoint> payoff_curve;
    double total_cost = 0.0;
    double expected_profit = 0.0;
    double worst_case_pnl = 0.0;
    double best_case_pnl = 0.0;
    double expected_pnl = 0.0;
    QString verdict;
    QStringList blockers;
};

struct KalshiReplayFrame {
    qint64 ts_ms = 0;
    KalshiAutoContext context;
    QVector<openmarketterminal::services::prediction::PredictionMarket> markets;
    QHash<QString, openmarketterminal::services::prediction::PredictionOrderBook> books;
};

struct KalshiReplayResult {
    int frames = 0;
    int decisions = 0;
    int trades = 0;
    int wins = 0;
    int losses = 0;
    double net_pnl = 0.0;
    double max_drawdown = 0.0;
    QVector<KalshiPortfolioPlan> plans;
};

class KalshiAutoEngine {
  public:
    static KalshiVolatilityEstimate estimate_realized_volatility(
        const QVector<KalshiTimedPrice>& prices,
        qint64 decision_ts_ms);

    static KalshiDistributionEstimate estimate_distribution(
        const QVector<KalshiTimedPrice>& prices,
        qint64 decision_ts_ms);

    static KalshiCalibrationModel fit_isotonic_calibration(
        const QVector<KalshiCalibrationSample>& samples,
        int minimum_samples = 30);

    // Groups tagged samples by cohort key and fits the same out-of-fold
    // calibration to each cohort independently. Reuses fit_isotonic_calibration
    // as the single source of truth; result is sorted by cohort key.
    static QVector<KalshiCohortModel> fit_cohort_calibration(
        const QVector<QPair<QString, KalshiCalibrationSample>>& tagged_samples,
        int minimum_events = 30);

    static double calibrated_probability(
        double probability,
        const QVector<KalshiCalibrationPoint>& curve);

    static double settlement_average_probability_above(
        const KalshiAutoContext& context,
        double strike,
        int seconds_left,
        double* projected_mean = nullptr,
        double* projected_stddev = nullptr,
        const QString& contract_horizon = {});

    static QVector<KalshiSurfacePoint> build_surface(
        const QVector<openmarketterminal::services::prediction::PredictionMarket>& markets,
        const QHash<QString, openmarketterminal::services::prediction::PredictionOrderBook>& books,
        const KalshiAutoContext& context,
        const QString& event_ticker = {});

    static KalshiPortfolioPlan optimize(const QVector<KalshiSurfacePoint>& surface,
                                        const KalshiAutoContext& context,
                                        const KalshiPortfolioConstraints& constraints = {});

    static KalshiMicroEvidenceResult evaluate_micro_evidence(
        const KalshiMicroEvidenceInput& input,
        double minimum_edge = 0.03,
        qint64 maximum_quote_age_ms = 5'000);

    static KalshiReplayResult replay(const QVector<KalshiReplayFrame>& frames,
                                     const QHash<QString, double>& final_settlement_by_event,
                                     const KalshiPortfolioConstraints& constraints = {});

    static QStringList compatibility_issues(
        const openmarketterminal::services::prediction::PredictionMarket& market);

    static QJsonObject surface_to_json(const QVector<KalshiSurfacePoint>& surface);
    static QJsonObject plan_to_json(const KalshiPortfolioPlan& plan);
    static QJsonObject replay_to_json(const KalshiReplayResult& replay);
};

} // namespace openmarketterminal::services::edge_radar
