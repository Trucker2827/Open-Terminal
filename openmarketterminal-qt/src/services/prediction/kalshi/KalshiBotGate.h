#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace openmarketterminal::services::prediction::kalshi_ns {

/// The Kalshi bot's sealed promotion gate (ladder rung 2).
///
/// Rung 1 papers every decision to kalshi-bot-decisions.jsonl. This module
/// answers exactly one question about that ledger — has the paper record
/// earned promotion? — and it answers it the way an arena season is scored:
/// mechanically, against criteria that were preregistered and sealed BEFORE
/// the record was read.
///
/// Four structural properties, none of them stylistic:
///   1. **The criteria are frozen.** They live in a sealed params file
///      (SHA-256 over the whole record, written 0444). `preregister()`
///      refuses while a params file exists, so criteria cannot be edited to
///      fit the results they will judge.
///   2. **A broken seal is never read as data.** Params that fail their seal
///      check — edited, unsealed, or not even an object — produce `TAMPERED`
///      and NO criteria numbers at all. The gate refuses; it does not
///      evaluate. Absent params produce `NOT_PREREGISTERED` for the same
///      reason: there is nothing to evaluate against.
///   3. **The verdict is pure computation.** `evaluate()` is a function of
///      (sealed params, ledger rows, now). There is no override parameter, no
///      force flag, and no path by which a caller can turn a FAIL into a PASS
///      — an unknown params key is refused at seal time, and a key injected
///      after sealing breaks the seal.
///   4. **Missing numbers are absent, never zero.** A ledger with nothing
///      scoreable reports `brier_available:false` rather than a 0.0 Brier that
///      would read as a perfect forecast.
///
/// Non-goal (rung 5's job): acting on the verdict. Nothing here arms, sizes,
/// prices, prepares, or submits anything.
class KalshiBotGate {
  public:
    /// Verdicts. Stable strings — evidence consumers match on them.
    static constexpr auto kVerdictPass = "PASS";
    static constexpr auto kVerdictFail = "FAIL";
    /// Params exist but are not trustworthy (seal mismatch, no seal, or not a
    /// JSON object). No criteria are evaluated.
    static constexpr auto kVerdictTampered = "TAMPERED";
    /// No params file at all. Nothing was preregistered, so nothing is judged.
    static constexpr auto kVerdictNotPreregistered = "NOT_PREREGISTERED";

    /// Criterion ids, in report order.
    static constexpr auto kCriterionSettled = "min_settled_bids";
    static constexpr auto kCriterionNetPnl = "net_pnl_usd";
    static constexpr auto kCriterionBrier = "brier_beats_market";
    static constexpr auto kCriterionDrawdown = "max_drawdown_usd";

    static constexpr auto kParamsFile = "kalshi-bot-gate-params.json";
    static constexpr auto kVerdictFile = "kalshi-bot-gate.json";

    /// Ledger events this gate reads (written by rung 1).
    static constexpr auto kDecisionEvent = "kalshi_bot_decision";
    static constexpr auto kSettlementEvent = "kalshi_bot_paper_settlement";

    /// Preregistration floors. These are tightening-only: an operator may
    /// preregister a HARDER gate, never an easier one than the ladder's.
    static constexpr int kMinSettledFloor = 300;      ///< issue #127's floor
    static constexpr double kMinNetPnlFloor = 0.0;    ///< profit after fees
    /// A drawdown limit above this is vacuous under the $2 stake / $3 all-in
    /// caps (it is already ~17 total-loss bids), so it is refused rather than
    /// sealed into a criterion that can never bind.
    static constexpr double kMaxDrawdownCeiling = 50.0;
    /// Brier margins live on a 0..1 scale; a quarter-point margin is already
    /// far beyond any real calibrator/market gap, so larger is refused as a
    /// typo rather than sealed.
    static constexpr double kMaxBrierMargin = 0.25;

    /// SHA-256 over every field of `record` except the seal itself, computed
    /// over compact JSON (Qt emits object keys sorted, so this is canonical).
    static QString seal(const QJsonObject& record);

    /// Whether `record` carries a seal that matches its own contents.
    static bool seal_valid(const QJsonObject& record);

    /// Validates preregistration parameters against the floors above.
    /// Returns the normalized params, or an empty object with `*error` set.
    /// Unknown keys are refused: the params file has no room for an override.
    static QJsonObject parse_params(const QJsonObject& raw, QString* error);

    /// Writes the sealed params file at `path`, read-only (0444).
    ///
    /// Refuses — returning an empty object with `*error` set — when a params
    /// file already exists, whatever its state. Preregistered criteria are
    /// immutable while active; re-sealing over them is exactly the edit the
    /// seal exists to prevent.
    static QJsonObject preregister(const QString& path, const QJsonObject& raw_params,
                                   qint64 now_ms, QString* error);

    /// The params file as stored. Returns an undefined QJsonValue when the
    /// file does not exist (→ NOT_PREREGISTERED), the parsed value otherwise
    /// (a non-object, including unparseable JSON, → TAMPERED).
    static QJsonValue load_params_file(const QString& path);

    /// The gate verdict: pure computation over the sealed params and the rung
    /// 1 ledger. `decision_rows` and `settlement_rows` are raw ledger rows;
    /// this function does its own event filtering.
    static QJsonObject evaluate(const QJsonValue& params_record,
                                const QJsonArray& decision_rows,
                                const QJsonArray& settlement_rows,
                                qint64 now_ms);
};

} // namespace openmarketterminal::services::prediction::kalshi_ns
