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
/// A PASS authorises only a bounded PAPER execution experiment. There is no
/// live corridor order path, and the verdict states that fact explicitly.
class KalshiCorridorGate {
  public:
    static constexpr auto kFamily = "btc_threshold_corridor";
    static constexpr auto kParamsFile = "kalshi-btc-corridor-gate-params.json";
    static constexpr auto kVerdictFile = "kalshi-btc-corridor-gate.json";
    static constexpr auto kEvidenceFile = "kalshi-btc-threshold-corridor.jsonl";
    static constexpr auto kScanEvent = "kalshi_btc_threshold_corridor_scan";

    static constexpr auto kVerdictPass = "PASS";
    static constexpr auto kVerdictFail = "FAIL";
    static constexpr auto kVerdictTampered = "TAMPERED";
    static constexpr auto kVerdictNotPreregistered = "NOT_PREREGISTERED";

    // Tightening-only floors. They prevent sealing a one-lucky-quote gate.
    static constexpr int kMinScansFloor = 300;
    static constexpr int kMinDistinctEventsFloor = 3;
    static constexpr int kMinOpportunityScansFloor = 10;
    static constexpr int kMinOpportunityEventsFloor = 3;
    static constexpr double kMaxUnavailableRateCeiling = 0.10;

    static QString seal(const QJsonObject& record);
    static bool seal_valid(const QJsonObject& record);
    static QJsonObject parse_params(const QJsonObject& raw, QString* error);
    static QJsonObject preregister(const QString& path, const QJsonObject& raw_params,
                                   qint64 now_ms, QString* error);
    static QJsonValue load_params_file(const QString& path);

    /// Pure evaluation over the entire scanner record. Unknown or malformed
    /// rows do not become zero-edge observations; they are counted and make
    /// the record fail the availability criterion.
    static QJsonObject evaluate(const QJsonValue& params_record,
                                const QJsonArray& evidence_rows,
                                qint64 now_ms);

    /// The only permission check a future corridor paper executor may use.
    /// A directional KalshiBotGate verdict cannot satisfy this contract, and
    /// no corridor verdict can authorize live orders.
    static bool permits_paper_bid(const QJsonObject& verdict, QString* reason = nullptr);
};

} // namespace openmarketterminal::services::prediction::kalshi_ns
