#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

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
/// Non-goal: acting on the verdict. Nothing here arms, sizes, prices,
/// prepares, or submits anything. Rung 5 READS the published verdict as one of
/// its live-admission conditions; it never asks this class to re-score, and it
/// cannot influence the score — `evaluate()` skips rows a live tick wrote, so a
/// live outcome can never be counted as the paper evidence that authorised it.
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
    /// The paper record the gate was handed is demonstrably not the whole
    /// record: a generation of the ledger is missing, or its oldest row
    /// postdates a settlement this gate has already published a score for. A
    /// truncated record is refused for the same reason tampered params are —
    /// scoring the remainder and publishing the numbers would be a verdict
    /// about a record that no longer exists (issue #152).
    static constexpr auto kVerdictRecordIncomplete = "RECORD_INCOMPLETE";

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

    /// What the caller could see about the RECORD the rows came from — the one
    /// thing `evaluate()` cannot derive from the rows themselves, because a
    /// truncated record looks exactly like a shorter one (issue #152).
    ///
    /// It is a required argument, never defaulted: a caller that forgot it
    /// would silently claim "the record is whole", which is the failure this
    /// struct exists to make impossible.
    struct RecordIntegrity {
        /// Ledger generations missing from the sequence, as paths. Non-empty
        /// means something deleted part of the record.
        QStringList missing_generations;
        /// The oldest dated row the reader could see; 0 when the record has no
        /// dated row at all.
        qint64 oldest_row_ts_ms = 0;
        /// `ledger.first_settled_ts_ms` from the verdict this gate published
        /// last — an anchor already on disk. 0 when nothing was published, or
        /// when what was published scored no settlement.
        qint64 published_first_settled_ts_ms = 0;

        /// For records read whole from a single source, e.g. a fixture built in
        /// memory. Says nothing has been published to compare against.
        static RecordIntegrity whole(qint64 oldest_row_ts_ms = 0) {
            RecordIntegrity record;
            record.oldest_row_ts_ms = oldest_row_ts_ms;
            return record;
        }
    };

    /// The anchor carried by a previously published verdict: its scored
    /// `ledger.first_settled_ts_ms`, or — when that verdict was itself a
    /// refusal, which carries no ledger block — the anchor it carried forward.
    /// 0 when neither is present. Without the carry-forward, one refusal would
    /// erase the anchor and the next run would score the truncated remainder.
    static qint64 published_anchor_ms(const QJsonObject& published_verdict);

    /// The gate verdict: pure computation over the sealed params, the rung 1
    /// ledger, and what the caller could see of that ledger's completeness.
    /// `decision_rows` and `settlement_rows` are raw ledger rows; this function
    /// does its own event filtering.
    static QJsonObject evaluate(const QJsonValue& params_record,
                                const QJsonArray& decision_rows,
                                const QJsonArray& settlement_rows,
                                qint64 now_ms,
                                const RecordIntegrity& record);
};

} // namespace openmarketterminal::services::prediction::kalshi_ns
