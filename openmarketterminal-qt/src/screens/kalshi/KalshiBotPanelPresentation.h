#pragma once

// Presentation for the Kalshi screen's BOT panel (ladder rung 3). Pure
// function of (decision-ledger rows, kalshi-bot-gate.json, the live session
// status object, now) so every honesty rule is regression-testable without a
// widget.
//
// The panel computes NO trading numbers of its own. The settled count, net
// P&L after fees, and Brier-vs-market come out of kalshi-bot-gate.json exactly
// as the gate wrote them, because the gate is where that arithmetic lives (it
// dedups settlements by position_id and joins the calibrated probability back
// to the decision that opened the position). A panel that re-derived those
// numbers would be a second implementation that silently disagrees with the
// verdict it is displayed next to. No gate file means no scoreboard — an
// absent evaluation reads absent, not zero.
//
// The ledger is read only for what the ledger alone knows: how recently the
// bot ticked, whether it trusted its signal on that tick, and what it actually
// decided, passes included.
//
// Rung 4: the status chip's classification is NOT computed here either. State,
// staleness threshold, colour role and headline all come from
// KalshiBotRuntime.h, which `kalshi bot status` renders from as well — that is
// what makes "the CLI mirrors the chip" a property rather than a claim.

#include "cli/ServeCommand.h"
#include "services/prediction/kalshi/KalshiBotRuntime.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QTimeZone>

namespace openmarketterminal::screens::kalshi {

// The state names, staleness threshold, colour role and stop-file format are
// the CLI's, not the panel's — one definition, two renderers.
using services::prediction::kalshi_ns::kalshi_bot_state_color_role;
using services::prediction::kalshi_ns::KalshiBotLoopStatus;
using services::prediction::kalshi_ns::KalshiBotStopFile;
using services::prediction::kalshi_ns::kKalshiBotStaleMs;

// The evidence files `kalshi bot` (rungs 1 and 4) and the promotion gate
// (rung 2) write. All three resolve through the one path module every consumer
// uses, so the panel can never read a different directory than the CLI writes.
inline constexpr auto kKalshiBotLedgerFile =
    services::prediction::kalshi_ns::kKalshiBotDecisionLedgerFile;
inline constexpr auto kKalshiBotStopFile =
    services::prediction::kalshi_ns::kKalshiBotStopFileName;
inline constexpr auto kKalshiBotGateFile = "kalshi-bot-gate.json";

inline QString kalshi_bot_ledger_path() {
    return cli::kalshi_evidence_path(QString::fromLatin1(kKalshiBotLedgerFile));
}
inline QString kalshi_bot_gate_path() {
    return cli::kalshi_evidence_path(QString::fromLatin1(kKalshiBotGateFile));
}
inline QString kalshi_bot_stop_path() {
    return cli::kalshi_evidence_path(QString::fromLatin1(kKalshiBotStopFile));
}

struct KalshiBotPanelView {
    QString status;             // headline: stopped/running/stale/off plus last-tick age
    QString state;              // "stopped" | "running" | "stale" | "off"
    bool stopped = false;       // the kill switch file exists right now
    QString armed;              // armed state + the caps in force, or DISARMED
    bool armed_live = false;    // an armed live session exists right now
    QString signal;             // signal-trust state of the most recent tick
    QString scoreboard;         // paper scoreboard, straight from the gate file
    QString gate;               // PASS / FAIL / refusal, with its numbers
    bool gate_pass = false;     // the gate evaluated and every criterion is met
    QStringList decisions;      // most recent decisions first, passes included
};

namespace kalshi_bot_detail {

inline bool is_number(const QJsonValue& value) { return value.isDouble(); }

inline QString money(double dollars) { return QStringLiteral("$%1").arg(dollars, 0, 'f', 2); }

/// A cap that was never written is unknown, not a default. The panel refuses to
/// print a ceiling the session did not actually state.
inline QString cap(const QJsonObject& status, const QString& key) {
    const QJsonValue value = status.value(key);
    return is_number(value) ? money(value.toDouble()) : QStringLiteral("unknown");
}

inline QString clock(qint64 ts_ms) {
    return QDateTime::fromMSecsSinceEpoch(ts_ms, QTimeZone::UTC).toString(QStringLiteral("hh:mm:ss")) +
           QStringLiteral("Z");
}

/// The comparison operator out of a criterion's `comparison` sentence
/// ("observed >= required"), so the rendered line reads as the gate stated it.
inline QString comparison_operator(const QJsonObject& criterion) {
    const QStringList parts =
        criterion.value(QStringLiteral("comparison")).toString().split(QLatin1Char(' '),
                                                                      Qt::SkipEmptyParts);
    return parts.size() >= 2 ? parts.at(1) : QStringLiteral("vs");
}

inline QString criterion_line(const QJsonObject& criterion) {
    const QString id = criterion.value(QStringLiteral("id")).toString(QStringLiteral("criterion"));
    const bool met = criterion.value(QStringLiteral("met")).toBool();
    const QJsonValue observed = criterion.value(QStringLiteral("observed"));
    const QJsonValue required = criterion.value(QStringLiteral("required"));
    if (!is_number(observed) || !is_number(required)) {
        // The gate writes no numbers for a criterion it could not score (an
        // unscoreable Brier, for one). Say that; do not print a 0.
        return QStringLiteral("%1 NOT SCORED (%2)")
            .arg(id, criterion.value(QStringLiteral("note"))
                         .toString(QStringLiteral("the gate reported no numbers")));
    }
    const int digits = id == QStringLiteral("min_settled_bids") ? 0
                     : id == QStringLiteral("brier_beats_market") ? 4
                                                                  : 2;
    return QStringLiteral("%1 %2 %3 %4 %5")
        .arg(id, QString::number(observed.toDouble(), 'f', digits), comparison_operator(criterion),
             QString::number(required.toDouble(), 'f', digits),
             met ? QStringLiteral("MET") : QStringLiteral("NOT MET"));
}

inline QString decision_line(const QJsonObject& row) {
    const auto ts_ms = static_cast<qint64>(row.value(QStringLiteral("ts_ms")).toDouble());
    const QString ticker = row.value(QStringLiteral("ticker")).toString();
    const bool bid = row.value(QStringLiteral("action")).toString() == QStringLiteral("bid");

    QStringList parts;
    parts << (ts_ms > 0 ? clock(ts_ms) : QStringLiteral("--:--:--"));
    parts << (bid ? QStringLiteral("BID") : QStringLiteral("PASS"));
    parts << (ticker.isEmpty() ? QStringLiteral("(no contract)") : ticker);
    if (bid) {
        const QJsonValue price = row.value(QStringLiteral("price"));
        const QJsonValue contracts = row.value(QStringLiteral("contracts"));
        parts << QStringLiteral("%1 %2 x%3")
                     .arg(row.value(QStringLiteral("side")).toString(QStringLiteral("?")),
                          is_number(price) ? money(price.toDouble()) : QStringLiteral("price ?"),
                          is_number(contracts) ? QString::number(contracts.toInt())
                                               : QStringLiteral("?"));
        const QJsonValue all_in = row.value(QStringLiteral("all_in_usd"));
        if (is_number(all_in)) parts << QStringLiteral("all-in %1").arg(money(all_in.toDouble()));
    }
    const QJsonValue edge = row.value(QStringLiteral("edge"));
    if (is_number(edge))
        parts << QStringLiteral("edge %1%2")
                     .arg(edge.toDouble() >= 0 ? QStringLiteral("+") : QString())
                     .arg(edge.toDouble(), 0, 'f', 3);
    parts << row.value(QStringLiteral("reason_code")).toString(QStringLiteral("NO REASON CODE"));
    return parts.join(QStringLiteral(" · "));
}

} // namespace kalshi_bot_detail

/// `ledger_rows` are raw rows from kalshi-bot-decisions.jsonl (both decision
/// and paper-settlement events); this function does its own event filtering.
/// `gate` is kalshi-bot-gate.json, `live_status` the `kalshi auto live status`
/// object the screen already polls, `stop` the kill switch as read from disk.
/// `max_decisions` bounds the rendered list.
inline KalshiBotPanelView present_kalshi_bot_panel(const QJsonArray& ledger_rows,
                                                   const QJsonObject& gate,
                                                   const QJsonObject& live_status,
                                                   qint64 now_ms,
                                                   int max_decisions = 8,
                                                   const KalshiBotStopFile& stop = {}) {
    using namespace kalshi_bot_detail;
    KalshiBotPanelView view;

    // --- what the bot did, newest first ------------------------------------
    QList<QJsonObject> decisions;
    for (const auto& value : ledger_rows) {
        const QJsonObject row = value.toObject();
        if (row.value(QStringLiteral("event")).toString() != QStringLiteral("kalshi_bot_decision"))
            continue;
        decisions.prepend(row);
    }

    // --- status chip: classified by the CLI's own classifier ----------------
    const KalshiBotLoopStatus status = services::prediction::kalshi_ns::kalshi_bot_loop_status(
        services::prediction::kalshi_ns::kalshi_bot_newest_ts_ms(ledger_rows), stop, now_ms);
    view.state = status.state;
    view.stopped = status.stop.engaged;
    view.status = status.state == QStringLiteral("running")
        ? QStringLiteral("%1 · %2 decisions in view").arg(status.headline).arg(decisions.size())
        : status.headline;

    // --- armed state and the caps in force ----------------------------------
    if (live_status.isEmpty()) {
        view.armed = QStringLiteral(
            "SESSION UNKNOWN / FAIL CLOSED · `kalshi auto live status` unavailable — armed state "
            "cannot be read, so it is not claimed");
    } else if (!live_status.value(QStringLiteral("session_active")).toBool()) {
        view.armed = QStringLiteral(
            "DISARMED · no armed live session · the bot papers only (every ledger row carries "
            "mode=paper, live_eligible=false)");
    } else {
        view.armed_live = true;
        const QJsonValue orders = live_status.value(QStringLiteral("max_orders_per_hour"));
        // Whose arm this is, stated on the line itself: the shared Kalshi live
        // session (the deterministic executor's), not a bot arm — the bot has
        // no arm of its own, and this rung gives it no way to get one.
        view.armed = QStringLiteral("ARMED · shared Kalshi live session (the bot has no arm of its "
                                    "own) · caps in force: stake <= %1 · all-in <= %2 · "
                                    "<= %3 orders/hour · exposure %4 of %5%6")
            .arg(cap(live_status, QStringLiteral("per_bet_contract_stake_cap")),
                 cap(live_status, QStringLiteral("per_bet_all_in_tolerance")),
                 is_number(orders) ? QString::number(orders.toInt()) : QStringLiteral("unknown"),
                 cap(live_status, QStringLiteral("worst_case_exposure_used")),
                 cap(live_status, QStringLiteral("experiment_cap")),
                 live_status.value(QStringLiteral("kill_switch")).toBool()
                     ? QStringLiteral(" · KILL SWITCH ON")
                     : QString());
    }

    // --- signal trust, as of the most recent decision -----------------------
    if (decisions.isEmpty()) {
        view.signal = QStringLiteral("SIGNAL UNKNOWN · no decision row to read it from");
    } else {
        const QJsonObject& latest = decisions.first();
        const QJsonValue trusted = latest.value(QStringLiteral("signal_trusted"));
        const QString reason = latest.value(QStringLiteral("reason_code")).toString();
        if (reason == QStringLiteral("BOT_STOPPED"))
            // The stopped tick never reached the report, so it has no opinion
            // on the signal — saying "unknown because it refused its report"
            // would misdescribe why.
            view.signal = QStringLiteral(
                "SIGNAL NOT READ · the kill switch stopped the last tick before the calibrator "
                "report was opened");
        else if (!trusted.isBool())
            view.signal = QStringLiteral("SIGNAL UNKNOWN · the last tick refused its report (%1)")
                              .arg(latest.value(QStringLiteral("reason_code"))
                                       .toString(QStringLiteral("no reason code")));
        else if (trusted.toBool())
            view.signal = QStringLiteral(
                "SIGNAL TRUSTED · the calibrator's measured Brier beats its market baseline");
        else
            view.signal = QStringLiteral(
                "SIGNAL UNTRUSTED · the calibrator has not beaten the market baseline; every bid "
                "is journaled SIGNAL_UNTRUSTED");
    }

    // --- scoreboard and gate verdict, both from the gate file ---------------
    if (gate.isEmpty()) {
        view.scoreboard = QStringLiteral(
            "PAPER SCOREBOARD UNAVAILABLE · no %1 — the promotion gate has not scored the paper "
            "record yet, so no settled count, P&L, or Brier is claimed")
                              .arg(QString::fromLatin1(kKalshiBotGateFile));
        view.gate = QStringLiteral("GATE NOT EVALUATED · no %1")
                        .arg(QString::fromLatin1(kKalshiBotGateFile));
    } else if (!gate.value(QStringLiteral("evaluated")).toBool()) {
        const QString verdict =
            gate.value(QStringLiteral("verdict")).toString(QStringLiteral("VERDICT MISSING"));
        view.scoreboard = QStringLiteral(
            "PAPER SCOREBOARD UNAVAILABLE · the gate refused to evaluate (%1), so it published no "
            "scoreboard numbers").arg(verdict);
        view.gate = QStringLiteral("GATE %1 · %2")
                        .arg(verdict, gate.value(QStringLiteral("reason"))
                                          .toString(QStringLiteral("no reason given")));
    } else {
        const QJsonObject ledger = gate.value(QStringLiteral("ledger")).toObject();
        const QJsonArray criteria = gate.value(QStringLiteral("criteria")).toArray();

        QString brier;
        for (const auto& value : criteria) {
            const QJsonObject criterion = value.toObject();
            if (criterion.value(QStringLiteral("id")).toString() !=
                QStringLiteral("brier_beats_market"))
                continue;
            brier = criterion.value(QStringLiteral("brier_available")).toBool()
                ? QStringLiteral("Brier %1 vs market %2 on %3 scored")
                      .arg(criterion.value(QStringLiteral("brier_bot")).toDouble(), 0, 'f', 4)
                      .arg(criterion.value(QStringLiteral("brier_market_baseline")).toDouble(), 0, 'f', 4)
                      .arg(criterion.value(QStringLiteral("scored_contracts")).toInt())
                : QStringLiteral("Brier NOT SCORED (no settled bid carried both a calibrated "
                                 "probability and its market mid)");
        }
        view.scoreboard = ledger.isEmpty()
            ? QStringLiteral("PAPER SCOREBOARD UNAVAILABLE · the verdict carries no ledger block, "
                             "so it published no settled count, P&L, or Brier")
            : QStringLiteral("PAPER SCOREBOARD · %1 settled · net %2 after %3 fees · %4")
                  .arg(QString::number(ledger.value(QStringLiteral("settled_bids")).toInt()),
                       money(ledger.value(QStringLiteral("net_pnl_usd")).toDouble()),
                       money(ledger.value(QStringLiteral("fees_usd")).toDouble()),
                       brier.isEmpty() ? QStringLiteral("Brier criterion missing from the verdict")
                                       : brier);

        const QString verdict =
            gate.value(QStringLiteral("verdict")).toString(QStringLiteral("VERDICT MISSING"));
        view.gate_pass = verdict == QStringLiteral("PASS");
        QStringList lines;
        for (const auto& value : criteria) lines << criterion_line(value.toObject());
        view.gate = QStringLiteral("GATE %1 · %2")
                        .arg(verdict, lines.isEmpty()
                                          ? QStringLiteral("the verdict carries no criteria")
                                          : lines.join(QStringLiteral(" · ")));
    }

    for (const QJsonObject& row : decisions) {
        if (view.decisions.size() >= max_decisions) break;
        view.decisions << decision_line(row);
    }
    return view;
}

/// The tail of the decision ledger — the CLI's own reader, so `kalshi bot
/// status` and the chip age the same rows.
inline QJsonArray read_kalshi_bot_ledger_tail(qint64 window_bytes = 512LL * 1024) {
    return services::prediction::kalshi_ns::kalshi_bot_read_ledger_tail(kalshi_bot_ledger_path(),
                                                                        window_bytes);
}

inline QJsonObject read_kalshi_bot_gate() {
    QFile file(kalshi_bot_gate_path());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject();
}

inline KalshiBotStopFile read_kalshi_bot_stop_file() {
    return services::prediction::kalshi_ns::kalshi_bot_read_stop_file(kalshi_bot_stop_path());
}

/// The panel as the screen renders it: all three evidence files read through
/// the unified path module, nothing else consulted.
inline KalshiBotPanelView load_kalshi_bot_panel(const QJsonObject& live_status, qint64 now_ms) {
    return present_kalshi_bot_panel(read_kalshi_bot_ledger_tail(), read_kalshi_bot_gate(),
                                    live_status, now_ms, 8, read_kalshi_bot_stop_file());
}

} // namespace openmarketterminal::screens::kalshi
