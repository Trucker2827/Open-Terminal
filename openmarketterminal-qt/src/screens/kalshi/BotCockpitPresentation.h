#pragma once

// Presentation for the BOT COCKPIT — the decision-rain scene the Predictions
// screen opens over the BOT tab while `kalshi bot` is trading (ladder rung 7).
//
// The scene is deliberately dramatic; the honesty rules that make it legible
// are the same ones the BOT panel already obeys, so they live here as a pure
// function of the evidence rather than inside a paint routine:
//
//  1. **Every animated element is a data element.** A falling column IS a
//     contract calibrator.json is currently predicting; the glyphs falling
//     down it ARE that contract's mid, calibrated probability, edge and
//     required-move sigma. An ignition IS one `action:"bid"` row in
//     kalshi-bot-decisions.jsonl, keyed by (ts_ms, ticker) so two bids on one
//     contract are two ignitions and re-presenting the same ledger is still
//     the same two. A dissolve IS one paper-settlement row. Nothing on this
//     scene is ambient: with no evidence there are no columns, no pulses, and
//     `motion` is false — an empty cockpit is still and says why.
//  2. **Frozen rain is stale rain.** calibrator.json carries ONE
//     `generated_at_ms` for the whole report, so the columns freeze together,
//     and they freeze at exactly the age the bot itself refuses the report at
//     (`KalshiBotDecision::Config::max_report_age_ms`). "Frozen" therefore
//     means "the bot would not trade off this report either" rather than a
//     threshold this scene invented for itself.
//  3. **The mood cannot overclaim.** LIVE is rendered only when the BOT
//     panel's own classifier says the newest readable tick was live. The panel
//     fails closed to UNKNOWN on a row it cannot read (issue #145) and this
//     scene inherits that: an all-paper ledger, an unreadable ledger, and an
//     absent ledger all render not-live moods. That is why `present_bot_cockpit`
//     takes the panel VIEW rather than re-reading the ledger — a second
//     mode classifier is a second thing to disagree with the CLI.
//  4. **Withheld numbers are missing, never zero.** A prediction with no mid
//     gets a glyph that reads "—", not 0.00. The KPI strip comes out of
//     kalshi-bot-gate.json's own `ledger` block verbatim — settled count, W/L,
//     net P&L, drawdown, and the sealed cap it is measured against — and reads
//     UNAVAILABLE, not zeroes, when the gate has published none.
//
// Nothing here reaches an exchange, and nothing here can arm anything: the
// scene is a renderer over four files the CLI writes.

#include "screens/kalshi/AutoCockpitPresentation.h"
#include "screens/kalshi/KalshiBotPanelPresentation.h"
#include "services/prediction/kalshi/KalshiBotDecision.h"
#include "services/prediction/kalshi/KalshiBotRuntime.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <utility>

namespace openmarketterminal::screens::kalshi {

// The report the rain falls from. Resolved through the one path module every
// consumer uses, exactly as the ledger and gate are.
inline constexpr auto kKalshiCalibratorFile = "calibrator.json";

inline QString kalshi_calibrator_path() {
    return cli::kalshi_evidence_path(QString::fromLatin1(kKalshiCalibratorFile));
}

/// The age at which a column freezes: the bot's own report-refusal age, not a
/// threshold this scene chose. A second constant here would let the rain keep
/// falling on a report the loop has already given up on.
inline qint64 bot_cockpit_freeze_bound_ms() {
    return services::prediction::kalshi_ns::KalshiBotDecision::Config{}.max_report_age_ms;
}

/// How many columns the scene draws at once. A cap is a rendering limit, so the
/// count it dropped is always stated (see `census`) — a silent top-N reads as
/// "these are all the contracts", which is the quiet kind of lie.
inline constexpr int kBotCockpitMaxColumns = 24;

/// How many pulses (ignitions, dissolves, gate/report events) the ledger stream
/// keeps. Also stated when it truncates.
inline constexpr int kBotCockpitMaxPulses = 12;

// ── Moods ──────────────────────────────────────────────────────────────────
// "paper" is the cool scene, "live" the hot one, "dormant" the grey one. There
// is no fourth: everything that is not a running paper loop or a live tick is
// a cockpit that should look dead.
inline constexpr auto kBotCockpitMoodPaper = "paper";
inline constexpr auto kBotCockpitMoodLive = "live";
inline constexpr auto kBotCockpitMoodDormant = "dormant";

/// Whether the BOT tab should be suggesting the cockpit. Defined once and
/// called from both sides — the scene model sets `suggest_cockpit` from it and
/// the BOT tab styles its button from it — so the button cannot start
/// suggesting on a state the scene would not agree with.
inline bool bot_cockpit_suggested(const KalshiBotPanelView& panel) {
    return panel.state == QStringLiteral("running");
}

/// One falling number. `known` false means the report did not carry it, and
/// `text` says so rather than printing a default.
struct BotCockpitGlyph {
    QString label;
    QString text;
    bool known = false;
    double value = 0.0;  ///< meaningless unless `known`
};

/// One rain column: a contract calibrator.json is predicting right now.
struct BotCockpitColumn {
    QString ticker;
    QList<BotCockpitGlyph> glyphs;
    /// The report this column's numbers came from is older than the bot's own
    /// refusal age (or dated in the future). A frozen column does not fall.
    bool frozen = false;
    QString frozen_reason;
    /// The bot bid this contract inside the ledger window. `ignitions` counts
    /// the journal rows, so a re-quoted contract ignites twice.
    int ignitions = 0;
    QString ignition_side;  ///< "YES" | "NO" — the side of the newest bid
    bool ignition_live = false;
    /// A paper settlement for this ticker landed in the window: the column
    /// dissolves rather than keeps falling.
    bool settled = false;
    bool settled_won = false;
    double settled_pnl_usd = 0.0;
    /// |calibrated p − market mid| — the ranking key when the cap bites.
    double abs_edge = 0.0;
    bool edge_known = false;
};

/// One animatable event, with the identity that makes it one. The widget
/// animates a pulse the first time it sees its `key` and never again: that is
/// what "one ignition per real journal row" means mechanically.
struct BotCockpitPulse {
    QString kind;  ///< "ignition" | "dissolve" | "tick" | "calibrator" | "gate"
    QString key;   ///< stable identity of the underlying data event
    QString text;
    QString role;  ///< colour role: green / red / cyan / amber / grey
    qint64 ts_ms = 0;
};

/// An orbit node: one named fact with its number, or a stated absence.
struct BotCockpitNode {
    QString id;
    QString label;
    QString value;
    QString role = QStringLiteral("grey");
    bool known = false;
};

struct BotCockpitScene {
    // ── mood ───────────────────────────────────────────────────────────────
    QString mood = QString::fromLatin1(kBotCockpitMoodDormant);
    bool live = false;     ///< real money went through the newest readable tick
    bool dormant = true;   ///< stopped / stale / off / unreadable — a dead cockpit
    QString banner;        ///< the unmissable line at the top of the scene
    QString mood_reason;   ///< why this mood and not another

    // ── rain ───────────────────────────────────────────────────────────────
    QList<BotCockpitColumn> columns;
    int columns_total = 0;   ///< predictions in the report, before the cap
    int columns_frozen = 0;
    QString census;          ///< "24 of 61 watched · ranked by |edge|", or the absence
    bool report_present = false;
    qint64 report_age_ms = -1;
    /// True only when something is actually moving: at least one unfrozen
    /// column on a cockpit that is not dormant. The scene's animation timer is
    /// driven off this, so a frozen or empty cockpit costs no repaints.
    bool motion = false;

    // ── centre ─────────────────────────────────────────────────────────────
    QString envelope;       ///< "BID YES $0.34 x3" / "PASS · EDGE_BELOW_THRESHOLD"
    QString envelope_key;   ///< changes exactly when a new decision row arrives
    QString envelope_role = QStringLiteral("grey");
    QString envelope_ticker;

    // ── orbit + strip ──────────────────────────────────────────────────────
    QList<BotCockpitNode> nodes;
    QList<BotCockpitPulse> pulses;  ///< newest first
    QStringList kpi;                ///< the strip, left to right
    /// One colour role per `kpi` entry, same index. Decided here beside the
    /// number rather than re-derived by the widget from the rendered text: a
    /// reworded entry must not be able to silently flip a colour.
    QStringList kpi_roles;
    bool kpi_available = false;
    QString kpi_unavailable_reason;

    /// The BOT tab suggests the cockpit exactly while the loop is running.
    bool suggest_cockpit = false;

    const BotCockpitNode* node(const QString& id) const {
        for (const auto& n : nodes)
            if (n.id == id) return &n;
        return nullptr;
    }
};

namespace bot_cockpit_detail {

using kalshi_bot_detail::criterion_line;
using kalshi_bot_detail::is_number;
using kalshi_bot_detail::money;

inline QString probability(double value) { return QString::number(value, 'f', 3); }

/// A DURATION, not an age. `auto_cockpit_age_text` says "17m ago", which is
/// wrong for the span a window covers — a span is not a point in the past.
inline QString span_text(qint64 ms) {
    if (ms < 120'000) return QStringLiteral("%1s").arg(ms / 1'000);
    if (ms < 7'200'000) return QStringLiteral("%1m").arg(ms / 60'000);
    return QStringLiteral("%1h").arg(ms / 3'600'000);
}

/// A glyph that the report did carry.
inline BotCockpitGlyph glyph(const QString& label, double value, const QString& text) {
    BotCockpitGlyph g;
    g.label = label;
    g.value = value;
    g.text = text;
    g.known = true;
    return g;
}

/// A glyph the report did NOT carry. Rule 4: it reads missing.
inline BotCockpitGlyph missing_glyph(const QString& label) {
    BotCockpitGlyph g;
    g.label = label;
    g.text = QStringLiteral("—");
    return g;
}

inline double number_or(const QJsonValue& value, double fallback, bool* known) {
    if (value.isDouble()) {
        if (known) *known = true;
        return value.toDouble();
    }
    if (known) *known = false;
    return fallback;
}

/// The four glyphs of one column, in fall order. `edge` is computed exactly as
/// KalshiBotDecision computes it (calibrated p − market mid) so the number
/// falling down the column is the number the ledger row carries.
inline void build_glyphs(const QJsonObject& prediction, BotCockpitColumn* column) {
    const QJsonObject features = prediction.value(QStringLiteral("features")).toObject();
    bool mid_known = false;
    const double mid =
        number_or(prediction.value(QStringLiteral("market_yes_mid")), 0.0, &mid_known);
    bool p_known = false;
    const double p = number_or(prediction.value(QStringLiteral("p_yes_full")), 0.0, &p_known);
    bool sigma_known = false;
    const double sigma =
        number_or(features.value(QStringLiteral("required_move_sigma")), 0.0, &sigma_known);

    column->glyphs << (mid_known ? glyph(QStringLiteral("mid"), mid, probability(mid))
                                 : missing_glyph(QStringLiteral("mid")));
    column->glyphs << (p_known ? glyph(QStringLiteral("p"), p, probability(p))
                               : missing_glyph(QStringLiteral("p")));
    if (mid_known && p_known) {
        const double edge = p - mid;
        column->edge_known = true;
        column->abs_edge = std::fabs(edge);
        column->glyphs << glyph(QStringLiteral("edge"), edge,
                                QStringLiteral("%1%2")
                                    .arg(edge >= 0 ? QStringLiteral("+") : QString())
                                    .arg(edge, 0, 'f', 3));
    } else {
        // An edge is a difference of two measurements. Missing either one makes
        // the edge unmeasured, not zero.
        column->glyphs << missing_glyph(QStringLiteral("edge"));
    }
    column->glyphs << (sigma_known ? glyph(QStringLiteral("sigma"), sigma,
                                           QString::number(sigma, 'f', 2))
                                   : missing_glyph(QStringLiteral("sigma")));
}

/// The compact decision text the envelope flashes. Deliberately shorter than
/// the panel's `decision_line` — same fields, one glance.
inline QString envelope_text(const QJsonObject& row, QString* role) {
    const QString action = row.value(QStringLiteral("action")).toString();
    const QString reason =
        row.value(QStringLiteral("reason_code")).toString(QStringLiteral("NO REASON CODE"));
    if (action == QStringLiteral("bid")) {
        const QString side = row.value(QStringLiteral("side")).toString(QStringLiteral("?"));
        const QJsonValue price = row.value(QStringLiteral("price"));
        const QJsonValue contracts = row.value(QStringLiteral("contracts"));
        // The tier is part of the act, not a footnote: a CROSS paid the spread
        // to be filled now, a REST is waiting at the mid (#158).
        const QString quote_style = row.value(QStringLiteral("quote_style")).toString();
        if (role) *role = side == QStringLiteral("NO") ? QStringLiteral("red")
                                                       : QStringLiteral("green");
        return QStringLiteral("BID %1 %2 x%3%4")
            .arg(side,
                 is_number(price) ? money(price.toDouble()) : QStringLiteral("price ?"),
                 is_number(contracts) ? QString::number(contracts.toInt()) : QStringLiteral("?"),
                 quote_style.isEmpty() ? QString()
                                       : QStringLiteral(" · ") + quote_style.toUpper());
    }
    if (action == QStringLiteral("fill")) {
        if (role) *role = QStringLiteral("green");
        return QStringLiteral("FILL · %1").arg(reason);
    }
    if (action == QStringLiteral("cancel")) {
        if (role) *role = QStringLiteral("amber");
        return QStringLiteral("CANCEL · %1").arg(reason);
    }
    if (role) *role = QStringLiteral("cyan");
    return QStringLiteral("PASS · %1").arg(reason);
}

/// The identity of a ledger row as an animatable event: its timestamp and its
/// contract. Two bids on one contract at two ticks are two keys; the same row
/// read twice is one.
inline QString row_key(const QString& kind, const QJsonObject& row) {
    return QStringLiteral("%1:%2:%3")
        .arg(kind)
        .arg(services::prediction::kalshi_ns::kalshi_bot_row_ts_ms(row))
        .arg(row.value(QStringLiteral("ticker")).toString());
}

} // namespace bot_cockpit_detail

/// The one classifier for the scene.
///
/// `panel` is the BOT panel's own view — the source of state, mode and the
/// kill switch, so the cockpit cannot disagree with the panel or the CLI about
/// what the bot is doing. `report` is calibrator.json, `gate`
/// kalshi-bot-gate.json, `ledger_rows` the ledger tail, `live_status` the
/// `kalshi auto live status` object the screen polls.
inline BotCockpitScene present_bot_cockpit(const KalshiBotPanelView& panel,
                                           const QJsonObject& report,
                                           const QJsonObject& gate,
                                           const QJsonArray& ledger_rows,
                                           const QJsonObject& live_status,
                                           qint64 now_ms,
                                           int max_columns = kBotCockpitMaxColumns,
                                           int max_pulses = kBotCockpitMaxPulses) {
    using namespace bot_cockpit_detail;
    BotCockpitScene scene;

    // ── mood ───────────────────────────────────────────────────────────────
    // Rule 3. `panel.mode_live` is true only when the newest READABLE tick said
    // live and no younger unreadable row outranks it, so an all-paper ledger
    // (and an unreadable one) can never reach the live branch from here.
    scene.suggest_cockpit = bot_cockpit_suggested(panel);
    if (panel.mode_live) {
        scene.mood = QString::fromLatin1(kBotCockpitMoodLive);
        scene.live = true;
        scene.dormant = false;
        // Real money outranks the loop state on the mood, exactly as it
        // outranks it on the panel's chip: a stopped loop with live orders is
        // not a dead cockpit.
        scene.banner = QStringLiteral("LIVE — REAL MONEY · %1").arg(panel.status);
        scene.mood_reason =
            QStringLiteral("the newest readable tick is journaled mode=live");
    } else if (panel.state == QStringLiteral("running") && !panel.mode_unknown) {
        scene.mood = QString::fromLatin1(kBotCockpitMoodPaper);
        scene.dormant = false;
        scene.banner = QStringLiteral("PAPER · %1").arg(panel.status);
        scene.mood_reason = QStringLiteral("the loop is ticking and the newest tick is paper");
    } else {
        scene.mood = QString::fromLatin1(kBotCockpitMoodDormant);
        scene.banner = panel.status.isEmpty()
            ? QStringLiteral("BOT COCKPIT DORMANT · no bot evidence to render")
            : panel.status;
        scene.mood_reason = panel.mode_unknown
            ? QStringLiteral("the newest ledger row carries no mode this build can read, so no "
                             "mode is claimed")
            : QStringLiteral("the loop is not running (%1)").arg(panel.state);
    }

    // ── rain: one column per prediction in the report ──────────────────────
    const QJsonObject predictions = report.value(QStringLiteral("predictions")).toObject();
    const qint64 generated_at_ms =
        static_cast<qint64>(report.value(QStringLiteral("generated_at_ms")).toDouble());
    scene.report_present = !report.isEmpty() && !predictions.isEmpty();
    if (generated_at_ms > 0) scene.report_age_ms = now_ms - generated_at_ms;

    // The whole report freezes together: it carries one generation stamp, not
    // one per contract, so there is no honest way to freeze a single column.
    const qint64 freeze_bound = bot_cockpit_freeze_bound_ms();
    const bool report_frozen =
        auto_cockpit_detail::timestamp_stale(generated_at_ms, now_ms, freeze_bound);
    const QString freeze_reason =
        generated_at_ms <= 0
            ? QStringLiteral("the report carries no generation time")
        : scene.report_age_ms < 0
            ? QStringLiteral("the report is dated in the future — clock skew, not freshness")
            : QStringLiteral("the report is %1, past the %2s age the bot itself refuses it at")
                  .arg(auto_cockpit_age_text(scene.report_age_ms))
                  .arg(freeze_bound / 1'000);

    // Newest bid and settlement per ticker, plus the pulse stream, in one pass
    // over the ledger tail.
    QHash<QString, QJsonObject> newest_bid;
    QHash<QString, QJsonObject> settlement;
    // Ignition COUNT is the number of journal rows, not the number of tickers:
    // a contract quoted three times ignited three times. Counted here, over the
    // whole window, so the pulse-stream cap cannot quietly reduce it.
    QHash<QString, int> ignition_counts;
    QList<BotCockpitPulse> pulses;
    QJsonObject newest_decision;
    qint64 newest_decision_ts = -1;
    int decisions_last_hour = 0;
    QSet<qint64> ticks_last_hour;
    qint64 oldest_row_ts = 0;
    qint64 newest_row_ts = 0;
    for (const auto& value : ledger_rows) {
        const QJsonObject row = value.toObject();
        const QString event = row.value(QStringLiteral("event")).toString();
        const qint64 ts = services::prediction::kalshi_ns::kalshi_bot_row_ts_ms(row);
        if (ts > 0) {
            oldest_row_ts = oldest_row_ts == 0 ? ts : qMin(oldest_row_ts, ts);
            newest_row_ts = qMax(newest_row_ts, ts);
        }
        const QString ticker = row.value(QStringLiteral("ticker")).toString();
        if (event == QLatin1String(kalshi_bot_detail::kDecisionEvent)) {
            if (ts > newest_decision_ts) {
                newest_decision_ts = ts;
                newest_decision = row;
            }
            if (ts > 0 && now_ms - ts <= 3'600'000 && now_ms >= ts) {
                ++decisions_last_hour;
                ticks_last_hour.insert(ts);
            }
            if (row.value(QStringLiteral("action")).toString() == QStringLiteral("bid")) {
                // One ignition per real journal row (rule 1).
                ignition_counts[ticker] += 1;
                const auto previous = newest_bid.constFind(ticker);
                if (previous == newest_bid.constEnd() ||
                    services::prediction::kalshi_ns::kalshi_bot_row_ts_ms(*previous) <= ts)
                    newest_bid.insert(ticker, row);
                const QString side =
                    row.value(QStringLiteral("side")).toString(QStringLiteral("?"));
                BotCockpitPulse pulse;
                pulse.kind = QStringLiteral("ignition");
                pulse.key = row_key(QStringLiteral("ignition"), row);
                pulse.ts_ms = ts;
                pulse.role = side == QStringLiteral("NO") ? QStringLiteral("red")
                                                          : QStringLiteral("green");
                QString ignored_role;
                pulse.text = QStringLiteral("%1 %2%3")
                                 .arg(ticker.isEmpty() ? QStringLiteral("(no contract)") : ticker,
                                      envelope_text(row, &ignored_role),
                                      kalshi_bot_detail::is_live(row) ? QStringLiteral(" · LIVE")
                                                                      : QString());
                pulses << pulse;
            }
        } else if (event == QLatin1String(kalshi_bot_detail::kSettlementEvent)) {
            settlement.insert(ticker, row);
            const bool won = row.value(QStringLiteral("won")).toBool();
            const QJsonValue pnl = row.value(QStringLiteral("realized_pnl"));
            BotCockpitPulse pulse;
            pulse.kind = QStringLiteral("dissolve");
            pulse.key = row_key(QStringLiteral("dissolve"), row);
            pulse.ts_ms = ts;
            pulse.role = won ? QStringLiteral("green") : QStringLiteral("red");
            pulse.text = QStringLiteral("%1 SETTLED %2 · %3")
                             .arg(ticker.isEmpty() ? QStringLiteral("(no contract)") : ticker,
                                  won ? QStringLiteral("WON") : QStringLiteral("LOST"),
                                  is_number(pnl) ? money(pnl.toDouble())
                                                 : QStringLiteral("P&L not stated"));
            pulses << pulse;
        }
    }

    // Two more data events the scene animates: a fresh report, and a gate
    // verdict. Both are keyed by their own timestamp, so a scene re-presented
    // against unchanged files produces the identical key and nothing re-fires.
    if (scene.report_present && generated_at_ms > 0) {
        BotCockpitPulse pulse;
        pulse.kind = QStringLiteral("calibrator");
        pulse.key = QStringLiteral("calibrator:%1").arg(generated_at_ms);
        pulse.ts_ms = generated_at_ms;
        pulse.role = report_frozen ? QStringLiteral("amber") : QStringLiteral("cyan");
        pulse.text = QStringLiteral("CALIBRATOR REFRESH · %1 contracts · %2")
                         .arg(predictions.size())
                         .arg(report_frozen ? freeze_reason
                                            : auto_cockpit_age_text(scene.report_age_ms));
        pulses << pulse;
    }
    if (!gate.isEmpty()) {
        const auto gate_ts = static_cast<qint64>(gate.value(QStringLiteral("ts_ms")).toDouble());
        const QString verdict =
            gate.value(QStringLiteral("verdict")).toString(QStringLiteral("VERDICT MISSING"));
        BotCockpitPulse pulse;
        pulse.kind = QStringLiteral("gate");
        pulse.key = QStringLiteral("gate:%1:%2").arg(gate_ts).arg(verdict);
        pulse.ts_ms = gate_ts;
        // The panel's role, not `gate_pass`: a stale PASS pulses amber like the
        // node it sits beside, so the scene has one answer about currency.
        pulse.role = panel.gate_role;
        pulse.text = QStringLiteral("SEALED GATE %1 · %2").arg(verdict, panel.gate_age);
        pulses << pulse;
    }

    std::stable_sort(pulses.begin(), pulses.end(),
                     [](const BotCockpitPulse& a, const BotCockpitPulse& b) {
                         return a.ts_ms > b.ts_ms;
                     });
    const int pulses_total = pulses.size();
    if (max_pulses >= 0 && pulses.size() > max_pulses) pulses = pulses.mid(0, max_pulses);
    scene.pulses = pulses;

    QList<BotCockpitColumn> columns;
    const QStringList tickers = predictions.keys();
    for (const QString& ticker : tickers) {
        BotCockpitColumn column;
        column.ticker = ticker;
        build_glyphs(predictions.value(ticker).toObject(), &column);
        column.frozen = report_frozen;
        if (report_frozen) column.frozen_reason = freeze_reason;
        const auto bid = newest_bid.constFind(ticker);
        if (bid != newest_bid.constEnd()) {
            column.ignition_side =
                bid->value(QStringLiteral("side")).toString(QStringLiteral("?"));
            column.ignition_live = kalshi_bot_detail::is_live(*bid);
        }
        const auto settled = settlement.constFind(ticker);
        if (settled != settlement.constEnd()) {
            column.settled = true;
            column.settled_won = settled->value(QStringLiteral("won")).toBool();
            column.settled_pnl_usd = settled->value(QStringLiteral("realized_pnl")).toDouble();
        }
        columns << column;
    }
    for (auto& column : columns) column.ignitions = ignition_counts.value(column.ticker, 0);

    scene.columns_total = columns.size();
    // Ranked by |edge| — the contracts closest to becoming a bid — so a cap
    // drops the quiet ones rather than an arbitrary slice. Unmeasured edges
    // sort last; ticker breaks ties so the order is stable between refreshes.
    std::stable_sort(columns.begin(), columns.end(),
                     [](const BotCockpitColumn& a, const BotCockpitColumn& b) {
                         if (a.edge_known != b.edge_known) return a.edge_known;
                         if (a.edge_known && a.abs_edge != b.abs_edge) return a.abs_edge > b.abs_edge;
                         return a.ticker < b.ticker;
                     });
    if (max_columns >= 0 && columns.size() > max_columns) columns = columns.mid(0, max_columns);
    scene.columns = columns;
    for (const auto& column : std::as_const(scene.columns))
        if (column.frozen) ++scene.columns_frozen;

    if (!scene.report_present) {
        scene.census = QStringLiteral("NO RAIN · no %1 predictions — the calibrator has published "
                                      "nothing for this cockpit to render")
                           .arg(QString::fromLatin1(kKalshiCalibratorFile));
    } else if (scene.columns_total > scene.columns.size()) {
        scene.census = QStringLiteral("%1 of %2 watched contracts · ranked by |edge| · %3 not drawn")
                           .arg(scene.columns.size())
                           .arg(scene.columns_total)
                           .arg(scene.columns_total - scene.columns.size());
    } else {
        scene.census = QStringLiteral("%1 watched contracts · all drawn").arg(scene.columns_total);
    }
    if (report_frozen && scene.report_present)
        scene.census += QStringLiteral(" · FROZEN: %1").arg(freeze_reason);

    // Rule 1, mechanically: motion needs something unfrozen to move, on a
    // cockpit that is not dead. The scene's animation timer is driven off this.
    scene.motion = !scene.dormant && scene.columns_frozen < scene.columns.size();

    // ── centre: the decision envelope ──────────────────────────────────────
    if (newest_decision.isEmpty()) {
        scene.envelope = QStringLiteral("NO DECISION JOURNALED · the envelope is empty until "
                                        "`kalshi bot once` writes a row");
        scene.envelope_role = QStringLiteral("grey");
    } else {
        QString role;
        scene.envelope = envelope_text(newest_decision, &role);
        scene.envelope_role = role;
        scene.envelope_ticker = newest_decision.value(QStringLiteral("ticker")).toString();
        scene.envelope_key = row_key(QStringLiteral("decision"), newest_decision);
        if (kalshi_bot_detail::is_live(newest_decision))
            scene.envelope = QStringLiteral("LIVE · %1").arg(scene.envelope);
    }

    // ── orbit nodes ────────────────────────────────────────────────────────
    const auto add_node = [&scene](const QString& id, const QString& label, const QString& value,
                                   const QString& role, bool known) {
        BotCockpitNode node;
        node.id = id;
        node.label = label;
        node.value = value;
        node.role = role;
        node.known = known;
        scene.nodes << node;
    };

    // CALIBRATOR — the report's OWN track record, which is a different measure
    // from the gate's brier_beats_market criterion (that one scores the bot's
    // settled bids). Both are on this scene; neither is relabelled as the
    // other, and the label says which is which — two Brier-vs-market numbers
    // three boxes apart, distinguishable only by a header comment, is the
    // thirty-second-legibility failure.
    const QJsonValue brier_full = report.value(QStringLiteral("brier_full"));
    const QJsonValue brier_baseline = report.value(QStringLiteral("brier_market_baseline"));
    if (is_number(brier_full) && is_number(brier_baseline)) {
        const bool adds_value = report.value(QStringLiteral("adds_value_over_market")).toBool();
        add_node(QStringLiteral("calibrator"),
                 QStringLiteral("CALIBRATOR — ITS OWN TRACK RECORD"),
                 QStringLiteral("Brier %1 vs market %2 on %3 resolved · %4")
                     .arg(brier_full.toDouble(), 0, 'f', 4)
                     .arg(brier_baseline.toDouble(), 0, 'f', 4)
                     .arg(report.value(QStringLiteral("resolved_contracts")).toInt())
                     .arg(adds_value ? QStringLiteral("ADDS VALUE") : QStringLiteral("NO EDGE YET")),
                 adds_value ? QStringLiteral("green") : QStringLiteral("amber"), true);
    } else {
        add_node(QStringLiteral("calibrator"), QStringLiteral("CALIBRATOR"),
                 QStringLiteral("NOT SCORED · the report carries no Brier track record"),
                 QStringLiteral("grey"), false);
    }

    // SETTLEMENTS / KPI — straight out of the gate's ledger block. Not
    // recomputed from the settlement rows above: the gate dedups by
    // position_id, and a second count would eventually disagree with the
    // verdict rendered beside it.
    const QJsonObject gate_ledger = gate.value(QStringLiteral("ledger")).toObject();
    const bool gate_evaluated = !gate.isEmpty() &&
                                gate.value(QStringLiteral("evaluated")).toBool() &&
                                !gate_ledger.isEmpty();
    if (gate_evaluated) {
        scene.kpi_available = true;
        const int settled = gate_ledger.value(QStringLiteral("settled_bids")).toInt();
        const int wins = gate_ledger.value(QStringLiteral("wins")).toInt();
        const int losses = gate_ledger.value(QStringLiteral("losses")).toInt();
        const double net = gate_ledger.value(QStringLiteral("net_pnl_usd")).toDouble();
        const double drawdown = gate_ledger.value(QStringLiteral("max_drawdown_usd")).toDouble();
        const QJsonValue drawdown_cap =
            gate.value(QStringLiteral("params")).toObject().value(QStringLiteral("max_drawdown_usd"));
        const bool drawdown_within_cap =
            is_number(drawdown_cap) && drawdown <= drawdown_cap.toDouble();
        scene.kpi << QStringLiteral("SETTLED %1").arg(settled)
                  << QStringLiteral("W/L %1-%2").arg(wins).arg(losses)
                  << QStringLiteral("NET %1").arg(money(net))
                  << QStringLiteral("DRAWDOWN %1 / %2")
                         .arg(money(drawdown),
                              is_number(drawdown_cap) ? money(drawdown_cap.toDouble())
                                                      : QStringLiteral("cap unknown"));
        // The one number the issue asks to be coloured by sign. A zero P&L is
        // neither a win nor a loss and is not painted as one.
        scene.kpi_roles << QStringLiteral("grey") << QStringLiteral("grey")
                        << (net > 0   ? QStringLiteral("green")
                            : net < 0 ? QStringLiteral("red")
                                      : QStringLiteral("grey"))
                        // A drawdown past its sealed cap is the loudest fact on
                        // the strip; an unstated cap is grey, never green.
                        << (!is_number(drawdown_cap) ? QStringLiteral("grey")
                            : drawdown_within_cap    ? QStringLiteral("grey")
                                                     : QStringLiteral("red"));
        add_node(QStringLiteral("settlements"), QStringLiteral("SETTLEMENTS"),
                 QStringLiteral("%1 settled · %2W / %3L · net %4 after %5 fees")
                     .arg(settled).arg(wins).arg(losses)
                     .arg(money(net),
                          money(gate_ledger.value(QStringLiteral("fees_usd")).toDouble())),
                 net > 0 ? QStringLiteral("green")
                         : net < 0 ? QStringLiteral("red") : QStringLiteral("grey"),
                 true);
    } else {
        scene.kpi_unavailable_reason =
            gate.isEmpty()
                ? QStringLiteral("no %1 — the promotion gate has not scored the paper record, so "
                                 "no settled count, W/L, P&L or drawdown is claimed")
                      .arg(QString::fromLatin1(kKalshiBotGateFile))
                : QStringLiteral("the gate published no ledger block (%1), so no settled count, "
                                 "W/L, P&L or drawdown is claimed")
                      .arg(gate.value(QStringLiteral("verdict"))
                               .toString(QStringLiteral("VERDICT MISSING")));
        scene.kpi << QStringLiteral("SCOREBOARD UNAVAILABLE · %1").arg(scene.kpi_unavailable_reason);
        scene.kpi_roles << QStringLiteral("grey");
        add_node(QStringLiteral("settlements"), QStringLiteral("SETTLEMENTS"),
                 QStringLiteral("UNAVAILABLE · %1").arg(scene.kpi_unavailable_reason),
                 QStringLiteral("grey"), false);
    }

    // Decision rate. The ledger tail is a byte window, not an hour, and every
    // tick writes one row PER CONTRACT — so this is counted over the last hour
    // explicitly and the span the window actually covers is printed beside it,
    // rather than dividing a count by a window and calling it a rate.
    const qint64 window_span_ms = newest_row_ts > oldest_row_ts ? newest_row_ts - oldest_row_ts : 0;
    const QString rate =
        newest_row_ts <= 0
            ? QStringLiteral("DECISIONS — no dated ledger row to count")
            : QStringLiteral("%1 decisions / %2 ticks in the last 60m (ledger window spans %3)")
                  .arg(decisions_last_hour)
                  .arg(ticks_last_hour.size())
                  .arg(span_text(window_span_ms));
    scene.kpi << rate;
    scene.kpi_roles << QStringLiteral("grey");

    // SEALED GATE — the verdict and its criteria, rendered by the BOT panel's
    // own criterion formatter so the two surfaces print identical lines.
    if (gate.isEmpty()) {
        add_node(QStringLiteral("gate"), QStringLiteral("SEALED GATE"),
                 QStringLiteral("NOT EVALUATED · no %1")
                     .arg(QString::fromLatin1(kKalshiBotGateFile)),
                 QStringLiteral("grey"), false);
    } else {
        const QJsonArray criteria = gate.value(QStringLiteral("criteria")).toArray();
        QStringList lines;
        int met = 0;
        for (const auto& value : criteria) {
            const QJsonObject criterion = value.toObject();
            if (criterion.value(QStringLiteral("met")).toBool()) ++met;
            lines << criterion_line(criterion);
        }
        const QString verdict =
            gate.value(QStringLiteral("verdict")).toString(QStringLiteral("VERDICT MISSING"));
        // The age comes second, right after the verdict word, and the node's
        // colour is the panel's own role (issue #167): a PASS the loop has not
        // re-evaluated within the staleness window is amber here exactly as it
        // is on the panel, never a green that reads as current.
        add_node(QStringLiteral("gate"), QStringLiteral("SEALED GATE"),
                 QStringLiteral("%1 · %2 · %3/%4 criteria met · %5")
                     .arg(verdict, panel.gate_age).arg(met).arg(criteria.size())
                     .arg(lines.isEmpty() ? QStringLiteral("the verdict carries no criteria")
                                          : lines.join(QStringLiteral(" · "))),
                 panel.gate_role, true);
    }

    // KILL SWITCH — the panel's own reading of the stop file.
    add_node(QStringLiteral("kill_switch"), QStringLiteral("KILL SWITCH"),
             panel.stopped ? QStringLiteral("ENGAGED · no bid is placed until it is cleared")
                           : QStringLiteral("CLEAR · the loop may bid"),
             panel.stopped ? QStringLiteral("red") : QStringLiteral("grey"), true);

    // EXPOSURE vs caps — the armed session's numbers, or the stated absence.
    // The panel's `armed` sentence is the authority on whether a session is
    // armed at all; the node adds the two numbers the scene needs.
    if (live_status.isEmpty()) {
        add_node(QStringLiteral("exposure"), QStringLiteral("EXPOSURE"),
                 QStringLiteral("UNKNOWN / FAIL CLOSED · `kalshi auto live status` unavailable"),
                 QStringLiteral("grey"), false);
    } else if (!panel.armed_live) {
        add_node(QStringLiteral("exposure"), QStringLiteral("EXPOSURE"),
                 QStringLiteral("DISARMED · no armed live session, so nothing is exposed through "
                                "this bot"),
                 QStringLiteral("grey"), true);
    } else {
        add_node(QStringLiteral("exposure"), QStringLiteral("EXPOSURE"),
                 QStringLiteral("%1 of %2 · stake <= %3 · all-in <= %4")
                     .arg(kalshi_bot_detail::cap(live_status,
                                                 QStringLiteral("worst_case_exposure_used")),
                          kalshi_bot_detail::cap(live_status, QStringLiteral("experiment_cap")),
                          kalshi_bot_detail::cap(live_status,
                                                 QStringLiteral("per_bet_contract_stake_cap")),
                          kalshi_bot_detail::cap(live_status,
                                                 QStringLiteral("per_bet_all_in_tolerance"))),
                 QStringLiteral("red"), true);
    }

    if (pulses_total > scene.pulses.size())
        scene.pulses.append([&] {
            BotCockpitPulse note;
            note.kind = QStringLiteral("truncated");
            note.key = QStringLiteral("truncated:%1").arg(pulses_total);
            note.role = QStringLiteral("grey");
            note.text = QStringLiteral("… %1 older events in this ledger window are not drawn")
                            .arg(pulses_total - scene.pulses.size());
            return note;
        }());

    return scene;
}

/// The scene as the screen renders it: the four evidence files, read through
/// the one path module, and the BOT panel view built from three of them.
inline BotCockpitScene load_bot_cockpit_scene(const QJsonObject& live_status, qint64 now_ms) {
    const QJsonArray ledger = read_kalshi_bot_ledger_tail();
    const QJsonObject gate = read_kalshi_bot_gate();
    const KalshiBotPanelView panel = present_kalshi_bot_panel(ledger, gate, live_status, now_ms, 8,
                                                              read_kalshi_bot_stop_file());
    QJsonObject report;
    QFile file(kalshi_calibrator_path());
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (document.isObject()) report = document.object();
    }
    return present_bot_cockpit(panel, report, gate, ledger, live_status, now_ms);
}

} // namespace openmarketterminal::screens::kalshi
