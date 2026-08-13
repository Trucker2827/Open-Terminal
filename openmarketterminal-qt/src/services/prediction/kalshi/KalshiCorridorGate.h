#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace openmarketterminal::services::prediction::kalshi_ns {

/// A sealed, strategy-specific evidence gate for the BTC threshold corridor.
///
/// This is deliberately not KalshiBotGate. KXBTCD directional predictions and
/// a two-leg lower-YES/higher-NO corridor share exchange contracts, but they do
/// not share a hypothesis, evidence record, or permission. This gate consumes
/// only certificate-backed `kalshi_btc_threshold_corridor_scan` rows.
///
/// A PASS arms only a bounded PAPER execution experiment. It does not require
/// historical opportunities: collecting paper evidence is the experiment's
/// purpose. Every proposed paper bid must still carry a fresh, certified scan
/// and fit the immutable risk envelope. Real micro-live execution has a second,
/// independent seal below; the paper verdict can never authorize it.
class KalshiCorridorGate {
  public:
    static constexpr auto kFamily = "btc_threshold_corridor";
    static constexpr auto kParamsFile = "kalshi-btc-corridor-gate-params.json";
    static constexpr auto kVerdictFile = "kalshi-btc-corridor-gate.json";
    static constexpr auto kEvidenceFile = "kalshi-btc-threshold-corridor.jsonl";
    static constexpr auto kPaperLedgerFile = "kalshi-btc-corridor-paper.jsonl";
    static constexpr auto kMicroLiveParamsFile = "kalshi-btc-corridor-micro-live-params.json";
    static constexpr auto kMicroLiveLedgerFile = "kalshi-btc-corridor-micro-live.jsonl";
    static constexpr auto kScanEvent = "kalshi_btc_threshold_corridor_scan";
    static constexpr auto kPaperBidEvent = "kalshi_btc_threshold_corridor_paper_bid";
    static constexpr auto kMicroLiveParamsEvent =
        "kalshi_btc_threshold_corridor_micro_live_params";
    static constexpr auto kMicroLiveExecutionEvent =
        "kalshi_btc_threshold_corridor_micro_live_execution";

    static constexpr auto kVerdictPass = "PASS";
    static constexpr auto kVerdictFail = "FAIL";
    static constexpr auto kVerdictTampered = "TAMPERED";
    static constexpr auto kVerdictNotPreregistered = "NOT_PREREGISTERED";

    // Broad parser ceilings prevent a supposedly paper-only seal from being
    // turned into an unbounded exposure authorization.
    static constexpr int kMaxBundlesCeiling = 100;
    static constexpr double kMaxOpportunityCostCeiling = 100.0;
    static constexpr int kMinScanAgeMs = 1'000;
    static constexpr int kMaxScanAgeMs = 300'000;
    static constexpr double kMicroLiveMaxAllInPerLegUsd = 2.0;
    static constexpr double kMicroLiveMaxPairAllInUsd = 4.0;
    static constexpr int kMicroLiveMaxExecutionsPerHour = 5;

    static QString seal(const QJsonObject& record);
    static bool seal_valid(const QJsonObject& record);
    static QJsonObject parse_params(const QJsonObject& raw, QString* error);
    static QJsonObject preregister(const QString& path, const QJsonObject& raw_params,
                                   qint64 now_ms, QString* error);
    static QJsonValue load_params_file(const QString& path);

    /// Activates a valid sealed paper-risk envelope. Evidence is summarized
    /// for the cockpit but never used as a prerequisite for paper collection.
    static QJsonObject evaluate(const QJsonValue& params_record,
                                const QJsonArray& evidence_rows,
                                qint64 now_ms);

    /// The only permission check the corridor paper executor may use.
    /// `pair_index` identifies the exact opportunity in `scan`; the scan's
    /// quoted quantity must equal `requested_bundles`. A directional
    /// KalshiBotGate verdict cannot satisfy this contract, and no corridor
    /// verdict can authorize live orders.
    static bool permits_paper_bid(const QJsonObject& verdict, const QJsonObject& scan,
                                  int pair_index, int requested_bundles,
                                  qint64 now_ms, QString* reason = nullptr);

    /// Separate authority for REAL micro-live corridor execution. It is never
    /// derived from the paper verdict. A valid seal authorizes at most $2
    /// all-in on EACH leg ($4 for the pair), and every call still requires the
    /// current fresh certificate-backed opportunity.
    static QJsonObject parse_micro_live_params(const QJsonObject& raw, QString* error);
    static QJsonObject preregister_micro_live(const QString& path,
                                              const QJsonObject& raw_params,
                                              qint64 now_ms, QString* error);
    static bool permits_micro_live(const QJsonValue& params_record,
                                   const QJsonObject& scan, int pair_index,
                                   int requested_bundles, qint64 now_ms,
                                   QString* reason = nullptr);
};

} // namespace openmarketterminal::services::prediction::kalshi_ns
