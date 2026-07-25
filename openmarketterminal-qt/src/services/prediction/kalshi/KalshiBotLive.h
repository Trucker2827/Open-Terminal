#pragma once

#include "services/prediction/kalshi/KalshiBotRuntime.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace openmarketterminal::services::prediction::kalshi_ns {

/// Micro-live containment for the Kalshi bot (ladder rung 5).
///
/// This is the module that decides whether the bot is ALLOWED to bid live, and
/// what a live bid's intent must say. It adds NO authority: it cannot arm
/// anything, it cannot place anything, and every number it puts in an intent is
/// re-checked by `submit_order` — which remains the sole trading authority.
/// What it does is refuse, early and loudly, in every case where the charter's
/// carve-out conditions are not all satisfied at once.
///
/// The four conditions this module enforces, one refusal code each:
///
///   1. **A human armed the session.** The arm lives in the shared Kalshi live
///      session (`kalshi auto live session …`) plus the GUI's own live gate.
///      Nothing in this file — or anywhere on the bot's path — can set either.
///      The bot has no arm of its own and this rung gives it no way to get one.
///   2. **The run is bounded.** `session_active` is NOT the same as bounded: a
///      `24/7` session writes an empty `ends_at`, and the terminal's own
///      activity test (`!ends.isValid() || ends > now`) reads that as active.
///      Live bidding requires a session whose end is an actual timestamp still
///      in the future, so an unbounded arm is refused rather than ridden.
///   3. **The signal passed its preregistered gate.** The bot reads the gate's
///      PUBLISHED verdict (kalshi-bot-gate.json), exactly as the rung 3 panel
///      does, rather than re-deriving it — a second scorer would eventually
///      disagree with the verdict shown beside it. A missing file, a refusal
///      (TAMPERED / NOT_PREREGISTERED — those carry no criteria at all), a
///      FAIL, or a verdict older than `kMaxGateAgeMs` all refuse.
///   4. **The kill switch is clear.** Checked here too, so a stopped bot cannot
///      even be admitted to live mode — `KalshiBotDecision::decide()` would
///      refuse the tick anyway, but a refusal that only happens deeper is a
///      refusal that a future caller can route around.
///
/// Everything here is pure: functions of (live-status object, gate object,
/// stop file, now). No file is read, no order is built beyond a JSON intent,
/// and nothing is written.
class KalshiBotLive {
  public:
    /// Refusal codes. Stable strings — journaled to the ledger and asserted by
    /// tests, so they are part of the audit contract.
    static constexpr auto kRefusedStopped = "LIVE_REFUSED_BOT_STOPPED";
    static constexpr auto kRefusedStatusUnknown = "LIVE_REFUSED_SESSION_UNKNOWN";
    static constexpr auto kRefusedNotArmed = "LIVE_REFUSED_NOT_ARMED";
    static constexpr auto kRefusedUnbounded = "LIVE_REFUSED_SESSION_UNBOUNDED";
    static constexpr auto kRefusedGateMissing = "LIVE_REFUSED_GATE_MISSING";
    static constexpr auto kRefusedGateRefused = "LIVE_REFUSED_GATE_NOT_EVALUATED";
    static constexpr auto kRefusedGateFail = "LIVE_REFUSED_GATE_NOT_PASS";
    static constexpr auto kRefusedGateStale = "LIVE_REFUSED_GATE_STALE";

    /// Reason codes for what happened to a live bid at the submit path.
    static constexpr auto kLiveSubmitted = "LIVE_ORDER_SUBMITTED";
    static constexpr auto kLiveRejected = "LIVE_ORDER_REJECTED_BY_SUBMIT";

    /// The mode tag every live ledger row carries. Paper rows keep rung 1's
    /// `mode:"paper"`; rows written before modes existed carry none, and every
    /// reader below treats an absent mode as paper.
    static constexpr auto kModeLive = "live";
    static constexpr auto kModePaper = "paper";

    /// The experiment every bot live order belongs to. Deliberately the SAME id
    /// PR #44's micro-live pilot uses, because the caps are the same caps and
    /// the exposure must be counted in one place: `submit_order` sums the
    /// experiment's outstanding orders, so a bot order and a hand-approved
    /// order compete for one $120 ceiling instead of two.
    static constexpr auto kExperimentId = "kalshi-micro-live-v1";

    /// How old the gate verdict may be. The verdict is an opinion about a
    /// settled paper record that keeps growing — with the pilot's ten orders an
    /// hour against a 300-settled-bid denominator, a verdict a working day old
    /// is scoring a materially different record than the one that exists. An
    /// hour is roughly one 15-minute-contract cycle's worth of drift, and
    /// re-running `kalshi bot gate` costs one command, so the bound is set
    /// where it actually binds rather than where it is convenient.
    static constexpr qint64 kMaxGateAgeMs = 60LL * 60 * 1000;

    /// Whether live bidding is permitted, and if not, exactly why.
    struct Permission {
        bool permitted = false;
        QString reason_code;   ///< one of the kRefused* codes when refused
        QString detail;        ///< a sentence a human can act on
        /// The armed session's caps, when a session was readable. Absent (<= 0)
        /// otherwise — a cap the session did not state is never defaulted into
        /// existence here, because a defaulted cap would be authority nobody
        /// granted.
        double max_stake_usd = -1.0;
        double max_all_in_usd = -1.0;
        double experiment_cap_usd = -1.0;
        int max_orders_per_hour = -1;
        QString session_id;
        QString session_ends_at;
        /// The gate verdict's own timestamp, so the caller can report what it
        /// trusted rather than re-reading the file.
        qint64 gate_ts_ms = -1;
    };

    /// The whole admission decision.
    ///
    /// `live_status` is the `kalshi auto live status` object (the same one the
    /// GUI BOT panel renders its caps from); an EMPTY object means the status
    /// could not be read and refuses with `kRefusedStatusUnknown` — an
    /// unreadable arm state is never read as armed, and never as disarmed
    /// either. `gate` is kalshi-bot-gate.json as published, `stop` the kill
    /// switch as read from disk this tick.
    static Permission permit(const QJsonObject& live_status,
                             const QJsonObject& gate,
                             const KalshiBotStopFile& stop,
                             qint64 now_ms,
                             qint64 max_gate_age_ms = kMaxGateAgeMs);

    /// The `prepare_order` intent for one bid row from
    /// `KalshiBotDecision::decide()`.
    ///
    /// Every limit in it comes from `permission` — that is, from the session a
    /// human armed — never from the bot's own paper config, so the bot cannot
    /// hand itself a wider cap than the arm granted. The intent is tagged
    /// `experiment_id` and `automation_session_id`, which is what makes
    /// `submit_order` run the full micro-live gate stack (session attribution,
    /// stake cap, all-in ceiling, rolling-hour cap, cumulative experiment cap,
    /// per-contract duplicate guard) against it. Returns an empty object when
    /// the row is not a bid or carries no usable price/size — a malformed row
    /// produces no order at all.
    static QJsonObject live_intent(const QJsonObject& bid_row, const Permission& permission);

    /// The bid row as journaled after the submit path answered.
    ///
    /// `submission` is `submit_order`'s data object verbatim. The row keeps
    /// every paper-math field the decision carried, retags it `mode:"live"`,
    /// and adds what the venue actually said — status, reason, filled and
    /// remaining counts. Nothing is inferred: if the submit path returned no
    /// status, the row says so rather than assuming the order lives.
    static QJsonObject live_row(const QJsonObject& decision_row,
                                const QJsonObject& submission,
                                const Permission& permission);

    /// A decision row retagged live without a submission — passes, refusals,
    /// and the kill-switch row on a live tick. A live tick's passes are live
    /// decisions and belong in the ledger as such.
    static QJsonObject live_row(const QJsonObject& decision_row, const Permission& permission);

    /// The refusal row journaled when live mode is not permitted. It carries
    /// the refusal code and NO trading numbers — there was no decision to
    /// describe, and a refusal that printed sizes would imply one.
    static QJsonObject refusal_row(const Permission& permission, qint64 now_ms);

    /// Whether a ledger row was written by a LIVE tick. An absent `mode` is
    /// paper: rung 1 wrote rows before modes existed, and the paper machinery
    /// (fill model, paper settlement, the promotion gate) must keep reading
    /// them. This is the one place that judgement is made.
    static bool is_live_row(const QJsonObject& row);

    /// The contracts the venue has already taken a bot order on, as `{ticker}`
    /// rows in the shape `KalshiBotDecision::decide()` reads for ALREADY_HELD.
    ///
    /// This is NOT a re-implementation of `submit_order`'s per-contract
    /// duplicate guard — it exists because that guard demonstrably does not
    /// cover this case. The guard counts drafts whose status is in
    /// (`submitting`, `submission_unknown`, `submitted`), but a LIVE submit
    /// writes the VENUE's state onto the draft (`filled`, `partially_filled`,
    /// `resting`, …; only the paper branch ever writes `submitted`). A filled
    /// bot order therefore leaves a `filled` draft that the guard's IN-list
    /// misses, and without this the bot would re-bid the contract it just
    /// bought on every tick until the hourly or experiment cap stopped it.
    ///
    /// Only an ACCEPTED order blocks. A bid the submit path refused — a rate
    /// limit, a cap, a killed fill-and-kill — leaves nothing at the venue and
    /// is retried next tick, which is the correct behaviour for a quote that
    /// never became an order.
    static QJsonArray live_working(const QJsonArray& ledger_rows);
};

} // namespace openmarketterminal::services::prediction::kalshi_ns
