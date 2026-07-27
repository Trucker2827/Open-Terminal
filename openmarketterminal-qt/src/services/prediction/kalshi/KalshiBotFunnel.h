#pragma once

// The Kalshi bot's CONVERSION FUNNEL and its pace to the sealed gate (issue
// #153).
//
// Why this module exists: every surface the operator reads shows the promotion
// gate's NUMERATOR — `settled 4 (3 won / 1 lost)`, `min_settled_bids 4 >= 300
// NOT MET` — and nothing else. The numerator is the smallest and most selected
// part of the record. On the live ledger, 304 quotes produced 5 fills and 4
// settlements: the scoreboard reads like a 4-sample record of the bot's
// decisions when it is a 4-sample record of the 1.6% of its quotes the market
// came to. And at 2.05 settlements/day the sealed 300-settlement gate is ~144
// days out, which no surface states either, so a gate that actually reads "not
// this year at this rate" reads as a plain "not yet".
//
// Three honesty rules are structural here, not stylistic:
//
//   1. **Every count is a count of rows actually seen.** A record with no bids
//      reports `fill_rate` as ABSENT, never 0.0 — a zero fill rate is a claim
//      about a denominator that does not exist. Same for every derived number
//      below: absence is a key that is not in the JSON, never a zero.
//   2. **A pace is a measurement, not a promise.** `days_to_gate_at_observed
//      _rate` is absent when the span is under an hour, when the observed rate
//      is zero, or when the gate's sealed minimum cannot be read and TRUSTED.
//      No `inf`, no `0 days`, no extrapolation from a span the record cannot
//      support.
//   3. **The paper record only.** Live rows are skipped INSIDE `measure()`,
//      exactly as `KalshiBotOrders::replay()` and `KalshiBotGate::evaluate()`
//      skip them, and for the gate's own stated reason: a caller that forgot
//      the filter would silently mix a live outcome into the paper evidence.
//      `resting_now` comes from the book replay, so counting a live bid here
//      would put the numerator and the denominator in different universes.
//
// Split deliberately across a header and a .cpp:
//   * the struct, the JSON shape, the file reader and the ONE FORMATTER both
//     renderers use are header-inline, for the same reason KalshiBotRuntime.h
//     is — the GUI does not link the CLI's translation units, and the BOT panel
//     and `kalshi bot status` must render the same sentence from the same file;
//   * `measure()` is in openterminal_core beside the other bot modules, because
//     it replays the book through KalshiBotOrders and checks the gate params'
//     seal through KalshiBotGate.
//
// Non-goal: changing anything the funnel measures. This module reports the
// pace; it does not improve it. The sealed params file is read-only input.

#include "services/prediction/kalshi/KalshiBotRuntime.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace openmarketterminal::services::prediction::kalshi_ns {

/// Where the tick publishes the funnel. Named here beside the ledger it is
/// measured from, so a rename cannot leave one reader on the old path.
inline constexpr auto kKalshiBotFunnelFile = "kalshi-bot-funnel.json";

/// A record shorter than this cannot support a rate projected out to days, so
/// the pace is absent rather than extrapolated (honesty rule 2).
inline constexpr qint64 kKalshiBotFunnelMinPaceSpanMs = 60LL * 60 * 1000;

/// The bucket a fill row that journals no `quote_style` is counted under. It is
/// NOT `rest`: every fill written before #158 was in fact passive, but the row
/// does not say so, and a funnel that filled that in would be asserting a tier
/// the ledger never stated (honesty rule 1). The sentence such a row carries is
/// still printed — what is unstated is the tier, not the rule.
inline constexpr auto kKalshiBotFunnelUnstatedTier = "unstated";

/// The conversion funnel over one set of ledger rows, plus the provenance of
/// what was measured. Every `*_available` flag is the difference between "the
/// record does not support this number" and "this number is zero".
struct KalshiBotFunnel {
    // --- what the record contains ------------------------------------------
    int bids = 0;                       ///< paper decision rows with action `bid`
    int resting_now = 0;                ///< orders the replayed book still has working
    int fills = 0;                      ///< paper lifecycle rows with action `fill`
    int canceled_ttl = 0;
    int canceled_edge_gone = 0;
    int canceled_market_settled = 0;
    int unconfirmed_cancels = 0;
    /// Settlements counted the way the gate counts them: deduped by
    /// `position_id`, live rows excluded, and a row that carries no
    /// `position_id` or no numeric `realized_pnl` not counted at all. If this
    /// were a raw row count, `settled_remaining` would be subtracting from a
    /// different denominator than the gate scores.
    int settlements = 0;
    /// Settlement rows the gate would refuse to identify. Reported so the drop
    /// is visible rather than silent.
    int settlements_unidentifiable = 0;
    /// Bids that predate the order lifecycle (rung 1: no `order_state`), which
    /// `KalshiBotOrders::replay()` books as ASSUMED fills at the quoted price.
    /// They are not counted in `fills` — nothing observed them fill — but they
    /// are stated, because a record holding assumed fills that carry no
    /// `fill_model` of their own would otherwise read as purely rung-6.
    int legacy_assumed_fill_bids = 0;

    // --- derived, each absent when the record cannot support it -------------
    bool fill_rate_available = false;
    double fill_rate = 0.0;             ///< fills / bids; absent when bids == 0
    bool settled_per_day_available = false;
    double settled_per_day = 0.0;       ///< absent when the span is zero

    // --- the pace to the SEALED gate ----------------------------------------
    bool gate_required_available = false;
    int gate_required = 0;              ///< `min_settled_bids` from the sealed params
    bool pace_available = false;
    int settled_remaining = 0;
    double days_to_gate_at_observed_rate = 0.0;
    /// Why the pace is absent, in the file and on the screen. Empty when it is
    /// present.
    QString pace_unavailable_reason;

    // --- provenance of what was measured ------------------------------------
    int rows_read = 0;                  ///< rows handed to measure(), before any filter
    int live_rows_skipped = 0;          ///< of those, rows a live tick wrote
    qint64 first_ts_ms = 0;             ///< oldest dated paper row; 0 when none
    qint64 last_ts_ms = 0;              ///< newest dated paper row; 0 when none
    qint64 span_ms = 0;                 ///< last - first; 0 when the record is undated
    /// The fill model(s) the record's own rows stamped on themselves, sorted
    /// and deduped. Printed verbatim: the selection the paper record was made
    /// by is the ledger's claim, not this module's.
    QStringList fill_models;

    /// One quoting tier's disclosure, over the fills that tier priced.
    ///
    /// Why this is keyed rather than a single string (#158): the crossing tier
    /// gave the ledger a SECOND fill rule, and the two say opposite things
    /// about how a fill was selected. A scalar could only hold one of them, so
    /// a record containing both published whichever row happened to come last
    /// — the panel then stated, over crossing fills, the passive sentence that
    /// is false for every one of them. Which tier is not a matter of opinion:
    /// it is the `quote_style` the deciding tick journaled on the order, and it
    /// is read here, never re-derived from the rule's prose.
    struct FillRule {
        /// The row's own `quote_style`, or `kKalshiBotFunnelUnstatedTier` for
        /// rows that state none (every fill written before #158). Never
        /// inferred: a row that named no tier is not relabelled as one.
        QString quote_style;
        /// The one sentence the rows under this tier carried. EMPTY when they
        /// carried none, and empty when they disagreed — see below.
        QString rule;
        /// Set when rows under one tier carried DIFFERENT rule texts. Nothing
        /// this codebase writes can do that (the sentence is a function of the
        /// tier), so it means a hand-edited or a future record; picking one of
        /// them would be the same last-writer bug this struct exists to kill.
        bool rules_disagree = false;
        /// Fills counted under this tier. Every `action: fill` row lands in
        /// exactly one bucket, so these sum to `fills` whether or not the rows
        /// carried a rule at all.
        int fills = 0;
    };
    /// Every tier the record's fills were priced by, sorted by `quote_style`
    /// so the published file and the rendered line cannot depend on row order.
    QList<FillRule> fill_rules;

    /// The whole funnel over `rows`, scored against the SEALED gate params.
    ///
    /// `rows` are raw ledger rows (decision and paper-settlement events alike);
    /// this function does its own event, live-row and identity filtering.
    /// `params_record` is `KalshiBotGate::load_params_file()`'s value — an
    /// undefined, unsealed or tampered record yields NO gate number and no
    /// pace, because a pace quoted against a target the gate itself would
    /// refuse to honour is not a measurement.
    static KalshiBotFunnel measure(const QJsonArray& rows, const QJsonValue& params_record);

    /// The published file's payload. Absent numbers are ABSENT KEYS: a reader
    /// that finds no `fill_rate` must have nothing to print, not a zero.
    QJsonObject to_json(qint64 now_ms) const;
};

// --- reading the published file ---------------------------------------------

inline QString kalshi_bot_funnel_absent_reason() {
    return QStringLiteral("no %1 in the evidence directory — no paper tick has published a funnel "
                          "here yet (a LIVE tick replays no paper book and publishes none)")
        .arg(QString::fromLatin1(kKalshiBotFunnelFile));
}

/// The funnel file as a reader found it. `available` is false for every reason
/// a reader could fail, and `why` says which — the renderers print that reason
/// and NO numbers.
struct KalshiBotFunnelFile {
    bool available = false;
    QString why = kalshi_bot_funnel_absent_reason();
    QJsonObject object;
};

inline KalshiBotFunnelFile kalshi_bot_read_funnel_file(const QString& path) {
    KalshiBotFunnelFile file;
    QFile handle(path);
    if (!handle.exists()) return file;
    if (!handle.open(QIODevice::ReadOnly | QIODevice::Text)) {
        file.why = QStringLiteral("%1 exists but cannot be opened").arg(path);
        return file;
    }
    const QJsonDocument document = QJsonDocument::fromJson(handle.readAll());
    if (!document.isObject()) {
        file.why = QStringLiteral("%1 is not parseable JSON — no numbers are read from it")
                       .arg(path);
        return file;
    }
    file.available = true;
    file.object = document.object();
    return file;
}

// --- the ONE formatter both surfaces render ---------------------------------

namespace kalshi_bot_funnel_detail {

inline bool is_number(const QJsonValue& value) { return value.isDouble(); }

inline int count(const QJsonObject& funnel, const char* key) {
    return funnel.value(QLatin1String(key)).toInt();
}

/// A duration a human reads. Hours below two days, days above — the span this
/// record was measured over is the sentence's whole point, so it is never
/// rounded away to "a while".
inline QString span_text(qint64 span_ms) {
    if (span_ms <= 0) return QStringLiteral("no span");
    const double hours = static_cast<double>(span_ms) / 3'600'000.0;
    return hours < 48.0 ? QStringLiteral("%1h").arg(hours, 0, 'f', 1)
                        : QStringLiteral("%1d").arg(hours / 24.0, 0, 'f', 1);
}

/// Every tier's disclosure, each next to the tier it describes and the fills it
/// was stated over (#158). A mixed record states BOTH sentences, because both
/// are true of the fills they are keyed to and neither is true of the others.
/// `QJsonObject::keys()` is sorted, so which row came last cannot change a
/// character of this.
inline QString fill_rules_text(const QJsonObject& funnel) {
    const QJsonObject rules = funnel.value(QStringLiteral("fill_rules")).toObject();
    QStringList parts;
    for (const QString& tier : rules.keys()) {
        const QJsonObject entry = rules.value(tier).toObject();
        const int fills = entry.value(QStringLiteral("fills")).toInt();
        const QString rule = entry.value(QStringLiteral("rule")).toString();
        QString said;
        if (!rule.isEmpty())
            said = rule;
        else if (entry.value(QStringLiteral("rules_disagree")).toBool())
            said = QStringLiteral("the rows under this tier state DIFFERENT fill rules, so none is "
                                  "quoted for them");
        else
            said = QStringLiteral("these fills state no fill_rule of their own");
        parts << QStringLiteral("%1 (%2 %3): %4")
                     .arg(tier)
                     .arg(fills)
                     .arg(fills == 1 ? QStringLiteral("fill") : QStringLiteral("fills"), said);
    }
    return parts.isEmpty() ? QString()
                           : QStringLiteral(" — %1").arg(parts.join(QStringLiteral(" · ")));
}

} // namespace kalshi_bot_funnel_detail

/// The funnel as BOTH the BOT panel and `kalshi bot status` render it: one
/// formatter, one rounding, one set of sentences. An unavailable file yields
/// exactly one line — the refusal and its reason, with no numbers anywhere.
///
/// A file older than two tick intervals (`kKalshiBotStaleMs`, the same constant
/// the status chip goes stale on) carries its age on EVERY line: a funnel is a
/// snapshot of a record that keeps growing, and a stale snapshot presented as
/// current is the failure this whole module is about.
inline QStringList kalshi_bot_funnel_lines(const KalshiBotFunnelFile& file, qint64 now_ms) {
    using namespace kalshi_bot_funnel_detail;
    if (!file.available)
        return QStringList{QStringLiteral("FUNNEL UNAVAILABLE · %1").arg(file.why)};

    const QJsonObject funnel = file.object;
    const auto published_ms = static_cast<qint64>(funnel.value(QStringLiteral("ts_ms")).toDouble());
    QString age;
    if (published_ms > 0 && now_ms > published_ms && now_ms - published_ms > kKalshiBotStaleMs)
        age = QStringLiteral(" · as of %1").arg(kalshi_bot_age_text(now_ms - published_ms));

    const int bids = count(funnel, "bids");
    const int fills = count(funnel, "fills");
    const int settlements = count(funnel, "settlements");
    const qint64 span_ms = static_cast<qint64>(funnel.value(QStringLiteral("span_ms")).toDouble());

    QStringList lines;
    lines << QStringLiteral("FUNNEL · %1 bids → %2 fills → %3 settled · %4 resting now · canceled "
                            "%5 TTL / %6 edge gone / %7 market settled · %8 unconfirmed")
                 .arg(bids)
                 .arg(fills)
                 .arg(settlements)
                 .arg(count(funnel, "resting_now"))
                 .arg(count(funnel, "canceled_ttl"))
                 .arg(count(funnel, "canceled_edge_gone"))
                 .arg(count(funnel, "canceled_market_settled"))
                 .arg(count(funnel, "unconfirmed_cancels")) + age;

    // The denominator sentence — the one this issue exists for. The rate is
    // never printed without the count it came from.
    const QJsonValue fill_rate = funnel.value(QStringLiteral("fill_rate"));
    lines << (is_number(fill_rate)
                  // "never observed to fill", not "the market never came to
                  // them": rung-1 bids are booked as ASSUMED fills by the
                  // replay, and this line must not read as contradicting the
                  // FILL MODEL line below that discloses them.
                  ? QStringLiteral("FILL RATE · %1% — %2 of %3 bids filled; the other %4 were never "
                                   "observed to fill")
                        .arg(fill_rate.toDouble() * 100.0, 0, 'f', 1)
                        .arg(fills)
                        .arg(bids)
                        .arg(bids - fills)
                  : QStringLiteral("FILL RATE UNAVAILABLE · this record contains no bid, so there "
                                   "is no denominator to divide by")) + age;

    const QJsonValue per_day = funnel.value(QStringLiteral("settled_per_day"));
    lines << (is_number(per_day)
                  ? QStringLiteral("SETTLEMENT RATE · %1 settled per day — %2 settlements measured "
                                   "over %3 of record")
                        .arg(per_day.toDouble(), 0, 'f', 2)
                        .arg(settlements)
                        .arg(span_text(span_ms))
                  : QStringLiteral("SETTLEMENT RATE UNAVAILABLE · the record spans no measurable "
                                   "time (%1 rows read)")
                        .arg(count(funnel, "rows_read"))) + age;

    const QJsonValue days = funnel.value(QStringLiteral("days_to_gate_at_observed_rate"));
    const QJsonValue remaining = funnel.value(QStringLiteral("settled_remaining"));
    lines << (is_number(days) && is_number(remaining)
                  ? QStringLiteral("PACE TO THE SEALED GATE · %1 more settled bids needed of %2 · "
                                   "~%3 days at the observed rate")
                        .arg(remaining.toInt())
                        .arg(count(funnel, "gate_required"))
                        .arg(days.toDouble(), 0, 'f', 0)
                  : QStringLiteral("PACE UNAVAILABLE · %1")
                        .arg(funnel.value(QStringLiteral("pace_unavailable_reason"))
                                 .toString(QStringLiteral("the record does not support a pace")))) +
                 age;

    // What the record was SELECTED by, in the ledger's own words. Printed from
    // the rows' `fill_model` / `fill_rule` values, never asserted here.
    QStringList models;
    for (const auto& value : funnel.value(QStringLiteral("fill_models")).toArray())
        models << value.toString();
    const QString rules = fill_rules_text(funnel);
    // Rung-1 bids state no model at all, so a record holding them is described
    // by counting them, never by inventing a name for what they were selected
    // by. `replay()` books them as assumed fills whatever this line says.
    const int legacy = count(funnel, "legacy_assumed_fill_bids");
    const QString assumed =
        legacy > 0 ? QStringLiteral(" · %1 bids predate the order lifecycle and are replayed as "
                                    "ASSUMED fills, stating no fill_model of their own").arg(legacy)
                   : QString();
    lines << (models.isEmpty()
                  ? QStringLiteral("FILL MODEL · not stated — no row in this record carries a "
                                   "fill_model, so the selection behind these fills is unstated%1%2")
                        .arg(rules, assumed)
                  : QStringLiteral("FILL MODEL · %1%2%3")
                        .arg(models.join(QStringLiteral(" + ")), rules, assumed)) + age;
    return lines;
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
