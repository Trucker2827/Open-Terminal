#pragma once

// Presentation for the BOT COCKPIT — the decision-rain scene the Predictions
// screen opens over the BOT tab while `kalshi bot` is trading (ladder rung 7).
//
// The scene is deliberately dramatic; the honesty rules that make it legible
// are the same ones the BOT panel already obeys, so they live here as a pure
// function of the evidence rather than inside a paint routine:
//
//  1. **Every animated element is a data element.** A falling column IS a
//     contract a calibrator is currently predicting; the glyphs falling down
//     it ARE that contract's mid, calibrated probability, edge and
//     required-move sigma. Threshold books come from calibrator.json;
//     KXBTC15M columns from kxbtc15m-calibrator.json; GOLD/SILVER/WTI from
//     commodities-15m-calibrator.json (the bot's own family split). An
//     ignition IS one `action:"bid"` row in
//     kalshi-bot-decisions.jsonl, keyed by (ts_ms, ticker) so two bids on one
//     contract are two ignitions and re-presenting the same ledger is still
//     the same two. A dissolve IS one paper-settlement row. Nothing on this
//     scene is ambient: with no evidence there are no columns, no pulses, and
//     `motion` is false — an empty cockpit is still and says why.
//  2. **Frozen rain is stale rain.** Each source report carries its own
//     `generated_at_ms`, so columns freeze per source at exactly the age the
//     bot itself refuses that report
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
#include "screens/kalshi/BotCockpitRainLabels.h"
#include "screens/kalshi/KalshiBotPanelPresentation.h"
#include "services/prediction/kalshi/KalshiBotDecision.h"
#include "services/prediction/kalshi/KalshiBotRuntime.h"
#include "services/prediction/kalshi/KalshiStrategyGridView.h"

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

using openmarketterminal::services::prediction::kalshi_ns::KalshiBotDecision;

// The reports the rain falls from. Resolved through the one path module every
// consumer uses, exactly as the ledger and gate are.
inline constexpr auto kKalshiCalibratorFile = "calibrator.json";
inline constexpr auto kKalshiKxbtc15mCalibratorFile = "kxbtc15m-calibrator.json";
inline constexpr auto kKalshiCommodities15mCalibratorFile = "commodities-15m-calibrator.json";

inline QString kalshi_calibrator_path() {
    return cli::kalshi_evidence_path(QString::fromLatin1(kKalshiCalibratorFile));
}

inline QString kalshi_kxbtc15m_calibrator_path() {
    return cli::kalshi_evidence_path(QString::fromLatin1(kKalshiKxbtc15mCalibratorFile));
}
inline QString kalshi_commodities_15m_calibrator_path() {
    return cli::kalshi_evidence_path(QString::fromLatin1(kKalshiCommodities15mCalibratorFile));
}

/// Pick the open KXBTC15M ticker the calibrator readout should follow.
/// "Open" means still present in the directional report's `predictions`.
/// Keeps `current_ticker` when it is still open; otherwise the widest |edge|,
/// then ticker for stability. Empty when the report has no 15m predictions.
inline QString prefer_open_kxbtc15m_ticker(const QJsonObject& kxbtc15m_report,
                                          const QString& current_ticker = {}) {
    using services::prediction::kalshi_ns::KalshiBotDecision;
    const QJsonObject predictions =
        kxbtc15m_report.value(QStringLiteral("predictions")).toObject();
    if (predictions.isEmpty()) return {};
    if (!current_ticker.isEmpty() && KalshiBotDecision::is_kxbtc15m_ticker(current_ticker) &&
        predictions.contains(current_ticker)) {
        return current_ticker;
    }
    QString best;
    double best_abs_edge = -1.0;
    bool best_known = false;
    for (auto it = predictions.constBegin(); it != predictions.constEnd(); ++it) {
        if (!KalshiBotDecision::is_kxbtc15m_ticker(it.key())) continue;
        const QJsonObject pred = it.value().toObject();
        const QJsonValue p = pred.value(QStringLiteral("p_yes_full"));
        const QJsonValue mid = pred.value(QStringLiteral("market_yes_mid"));
        const bool known = p.isDouble() && mid.isDouble();
        const double abs_edge = known ? std::fabs(p.toDouble() - mid.toDouble()) : -1.0;
        const bool better =
            best.isEmpty() || (known && !best_known) ||
            (known == best_known && known && abs_edge > best_abs_edge) ||
            (known == best_known && abs_edge == best_abs_edge && it.key() < best);
        if (!better) continue;
        best = it.key();
        best_abs_edge = abs_edge;
        best_known = known;
    }
    return best;
}

/// Compact outside-info ablation readout for cockpit/CLI-shaped report JSON.
/// Lists each variant's n + whether it currently beats mid; names the trusted
/// winner when present. Pure — no I/O.
inline QString outside_info_ablation_line(const QJsonObject& report) {
    const QJsonObject ablations = report.value(QStringLiteral("ablations")).toObject();
    if (ablations.isEmpty()) return {};
    QStringList parts;
    for (auto it = ablations.constBegin(); it != ablations.constEnd(); ++it) {
        const QJsonObject row = it.value().toObject();
        const int n = row.value(QStringLiteral("scored_contracts")).toInt();
        const bool beats = row.value(QStringLiteral("beats_mid")).toBool();
        // Shorten common prefixes so the orbit line stays scannable.
        QString name = it.key();
        name.replace(QStringLiteral("physics_"), QString());
        parts << QStringLiteral("%1 n=%2%3")
                     .arg(name)
                     .arg(n)
                     .arg(beats ? QStringLiteral("*") : QString());
    }
    const QString variant = report.value(QStringLiteral("trusted_variant")).toString();
    QString line = QStringLiteral("ablate %1").arg(parts.join(QStringLiteral(" · ")));
    if (!variant.isEmpty())
        line += QStringLiteral(" · trusted=%1").arg(variant);
    return line;
}

/// Settlement-parity / Pyth honesty line for commodities reports.
inline QString settlement_parity_line(const QJsonObject& report) {
    const QJsonObject parity = report.value(QStringLiteral("settlement_parity")).toObject();
    if (parity.isEmpty()) return {};
    const int checked = parity.value(QStringLiteral("checked")).toInt();
    const int matched = parity.value(QStringLiteral("matched")).toInt();
    const QJsonValue rate = parity.value(QStringLiteral("match_rate"));
    if (checked <= 0)
        return QStringLiteral("parity Pyth↔Kalshi unchecked");
    return QStringLiteral("parity Pyth↔Kalshi %1/%2 (%3)")
        .arg(matched)
        .arg(checked)
        .arg(rate.isDouble() ? QString::number(rate.toDouble() * 100.0, 'f', 0) + QStringLiteral("%")
                             : QStringLiteral("—"));
}

/// One line for the KXBTC15M scoreboard node / KPI: scored/floor, ΔBrier vs
/// mid, trust, trusted ablation variant. Pure over the report object — no I/O.
inline QString kxbtc15m_scoreboard_line(const QJsonObject& report) {
    if (report.isEmpty()) {
        return QStringLiteral("NO %1 — the directional calibrator has not published here yet")
            .arg(QString::fromLatin1(kKalshiKxbtc15mCalibratorFile));
    }
    const int scored = report.value(QStringLiteral("scored_contracts")).toInt();
    const int floor = report.value(QStringLiteral("min_scored_contracts")).toInt(100);
    const QJsonValue brier_full = report.value(QStringLiteral("brier_full"));
    const QJsonValue brier_mid = report.value(QStringLiteral("brier_market_mid_raw"));
    const bool trusted = KalshiBotDecision::signal_trusted(report);
    const QString variant = report.value(QStringLiteral("trusted_variant")).toString();
    if (!brier_full.isDouble() || !brier_mid.isDouble()) {
        QString line = QStringLiteral("%1/%2 scored · Brier unavailable · NO EDGE YET")
                           .arg(scored)
                           .arg(floor);
        const QString ablate = outside_info_ablation_line(report);
        if (!ablate.isEmpty()) line += QStringLiteral(" · %1").arg(ablate);
        return line;
    }
    const double delta = brier_full.toDouble() - brier_mid.toDouble();
    // Negative Δ means the model beat the mid (lower Brier is better).
    const QString delta_text = QStringLiteral("%1%2")
                                   .arg(delta < 0 ? QStringLiteral("−") : QStringLiteral("+"))
                                   .arg(std::fabs(delta), 0, 'f', 4);
    // The bet-eligible evidence rides on the line because it is what the
    // refusal is now made of: without it this line printed a negative ΔBrier
    // (model better than the mid) beside "NO EDGE YET", which reads as the
    // screen contradicting its own number. The badge names which conjunct
    // decided (KalshiBotDecision::trust_badge_text).
    QString line = QStringLiteral("%1/%2 scored · ΔBrier %3 vs mid · %4 · %5")
                       .arg(scored)
                       .arg(floor)
                       .arg(delta_text,
                            KalshiBotDecision::bet_eligible_evidence_text(report),
                            KalshiBotDecision::trust_badge_text(report));
    if (trusted && !variant.isEmpty())
        line += QStringLiteral(" · %1").arg(variant);
    else {
        const QString ablate = outside_info_ablation_line(report);
        if (!ablate.isEmpty()) line += QStringLiteral(" · %1").arg(ablate);
    }
    return line;
}

/// Commodities 15m scoreboard line — same scored/ΔBrier/trust/ablation shape as
/// KXBTC15M, plus Pyth settlement-parity when the report carries it.
inline QString commodities_15m_scoreboard_line(const QJsonObject& report) {
    if (report.isEmpty()) {
        return QStringLiteral("NO %1 — GOLD/SILVER/WTI 15m calibrator has not published here yet")
            .arg(QString::fromLatin1(kKalshiCommodities15mCalibratorFile));
    }
    QString line = kxbtc15m_scoreboard_line(report);
    const QString parity = settlement_parity_line(report);
    if (!parity.isEmpty()) line += QStringLiteral(" · %1").arg(parity);
    return line;
}

/// Threshold / KXBTCD strike scoreboard — same measurement shape as the 15m
/// line, worded for the strike calibrator (`calibrator.json`). Paper ambition
/// pins this family above FLOW while 15m stays an observe/KPI scoreboard.
inline QString threshold_scoreboard_line(const QJsonObject& report) {
    if (report.isEmpty()) {
        return QStringLiteral("NO %1 — the strike/threshold calibrator has not published here yet")
            .arg(QString::fromLatin1(kKalshiCalibratorFile));
    }
    return kxbtc15m_scoreboard_line(report);
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

/// DECIDE reads GREEN "bidding" only when the winning bid's OWN `ts_ms` is
/// within this many ms of `now_ms`. `newest_bid` is drawn from
/// `read_kalshi_bot_ledger_tail()`, which is a 512 KB BYTE window, not a time
/// window — so an old bid can sit inside that window long after the loop
/// stopped bidding, and without this check DECIDE would keep reporting green
/// "bidding" on a loop that has been merely passing for a long stretch.
/// 3 x the bot's ~60s tick interval: a bid inside the last ~3 ticks counts as
/// "recent". Older than that, DECIDE falls to amber "passing" (the loop is
/// fine, just not bidding right now) — never red, and never a stale green.
inline constexpr qint64 kDecideBidRecencyMs = 180'000;

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

/// One rain column: a contract a calibrator is predicting right now.
struct BotCockpitColumn {
    QString ticker;
    QList<BotCockpitGlyph> glyphs;
    /// Which report owns this ticker: "threshold" | "kxbtc15m" |
    /// "commodities15m". Empty when unknown.
    QString signal_source;
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
    /// Multi-line inspect body for click-to-expand (outside-info ablations /
    /// parity). Empty when the node is not inspectable. Read-only — never arms.
    QString detail;
};

/// Feed/harvest health the pure presentation cannot see for itself — supplied
/// by the view from kalshi-ws-engine.json + the newest retained ticker event.
struct BotCockpitFeedHealth {
    bool    readable = false;            ///< the engine status parsed at all (false => UNKNOWN)
    bool    ws_connected = false;        ///< "connected"
    bool    credentials_ok = false;      ///< "credentials"
    qint64  newest_event_age_ms = -1;    ///< age of the newest Kalshi market event (-1 => none)
    QString last_error;                  ///< surfaced in the headline when HARVEST is red
};

/// One stage of the health strip. `role` is a colour vocabulary already used by
/// the scene: "green" | "amber" | "red" | "grey".
struct BotCockpitHealthStage {
    QString id;      ///< "harvest" | "calibrate_threshold" | "calibrate_15m" | "decide" | "settle"
    QString label;   ///< "HARVEST" / "THR" / "15M" ...
    QString value;   ///< the one datum ("2s", "7/100", "trusted", "bidding", "32")
    QString role = QStringLiteral("grey");
};

/// Compact source tag for a rain column header ("THR" / "15m" / "COM"). Empty
/// when the column has no family stamp — paint must not invent one.
inline QString bot_cockpit_source_tag(const QString& signal_source) {
    if (signal_source == QLatin1String("kxbtc15m")) return QStringLiteral("15m");
    if (signal_source == QLatin1String("commodities15m")) return QStringLiteral("COM");
    if (signal_source == QLatin1String("threshold")) return QStringLiteral("THR");
    return {};
}

/// Rain sort rank: strike/threshold books first (paper ambition), then BTC 15m,
/// then commodities. Unknown sources sort last.
inline int bot_cockpit_rain_family_rank(const QString& signal_source) {
    if (signal_source == QLatin1String("threshold")) return 0;
    if (signal_source == QLatin1String("kxbtc15m")) return 1;
    if (signal_source == QLatin1String("commodities15m")) return 2;
    return 3;
}

/// Multi-line outside-info inspect body for a family report (ablations,
/// trusted_variant, settlement_parity). Pure — no I/O. Empty report → stated
/// absence. Never invents trust from parity alone.
inline QString outside_info_inspect_detail(const QJsonObject& report) {
    if (report.isEmpty())
        return QStringLiteral("NO REPORT — this family has not published here yet");
    QStringList lines;
    const int scored = report.value(QStringLiteral("scored_contracts")).toInt();
    const int floor = report.value(QStringLiteral("min_scored_contracts")).toInt(100);
    const QString variant = report.value(QStringLiteral("trusted_variant")).toString();
    const QString variant_eligible =
        report.value(QStringLiteral("trusted_variant_eligible")).toString();
    lines << QStringLiteral("scored %1/%2 · trust %3 · trusted_variant %4")
                 .arg(scored)
                 .arg(floor)
                 .arg(KalshiBotDecision::trust_badge_text(report),
                      variant.isEmpty() ? QStringLiteral("—") : variant);
    const QJsonValue brier_full = report.value(QStringLiteral("brier_full"));
    const QJsonValue brier_mid = report.value(QStringLiteral("brier_market_mid_raw"));
    if (brier_full.isDouble() && brier_mid.isDouble()) {
        const double delta = brier_full.toDouble() - brier_mid.toDouble();
        lines << QStringLiteral("brier %1 vs mid %2 (Δ %3%4)")
                     .arg(brier_full.toDouble(), 0, 'f', 4)
                     .arg(brier_mid.toDouble(), 0, 'f', 4)
                     .arg(delta < 0 ? QStringLiteral("−") : QStringLiteral("+"))
                     .arg(std::fabs(delta), 0, 'f', 4);
    } else {
        lines << QStringLiteral("brier unavailable");
    }
    // The refusal's own evidence, on the inspect body where a reader has gone
    // looking for it. A negative Δ above beside "NO EDGE WHERE IT BETS" is
    // only readable with these two numbers present.
    lines << KalshiBotDecision::bet_eligible_evidence_text(report);
    lines << KalshiBotDecision::trust_verdict_text(report);
    // Which variant won each board. With the flag now requiring the SAME
    // variant on both, a disagreement here IS the refusal, and a reader who
    // cannot see both names cannot tell that from a subset that simply lost.
    if (!variant.isEmpty() || !variant_eligible.isEmpty())
        lines << QStringLiteral("trusted_variant (full) %1 · (bet-eligible) %2")
                     .arg(variant.isEmpty() ? QStringLiteral("—") : variant,
                          variant_eligible.isEmpty() ? QStringLiteral("—") : variant_eligible);
    const QJsonObject ablations = report.value(QStringLiteral("ablations")).toObject();
    if (ablations.isEmpty()) {
        lines << QStringLiteral("ablations: (none)");
    } else {
        lines << QStringLiteral("ablations (observe → veto/confirm → trust at n≥100):");
        QStringList keys = ablations.keys();
        keys.sort();
        for (const QString& key : keys) {
            const QJsonObject row = ablations.value(key).toObject();
            const QJsonValue brier = row.value(QStringLiteral("brier"));
            const int n = row.value(QStringLiteral("scored_contracts")).toInt();
            const bool beats = row.value(QStringLiteral("beats_mid")).toBool();
            lines << QStringLiteral("  %1  brier=%2  n=%3  %4")
                         .arg(key,
                              brier.isDouble() ? QString::number(brier.toDouble(), 'f', 4)
                                               : QStringLiteral("—"))
                         .arg(n)
                         .arg(beats ? QStringLiteral("beats_mid") : QStringLiteral("no_edge"));
        }
    }
    const QString parity = settlement_parity_line(report);
    if (!parity.isEmpty()) lines << parity;
    return lines.join(QLatin1Char('\n'));
}

struct BotCockpitScene {
    // ── mood ───────────────────────────────────────────────────────────────
    QString mood = QString::fromLatin1(kBotCockpitMoodDormant);
    bool live = false;     ///< real money went through the newest readable tick
    bool dormant = true;   ///< stopped / stale / off / unreadable — a dead cockpit
    QString banner;        ///< the unmissable line at the top of the scene
    QString mood_reason;   ///< why this mood and not another
    /// One advisory line from the paper strategy-grid (kalshi-strategy-grid-latest.json):
    /// UNAVAILABLE / no measured edge / a forming candidate / a measured survivor,
    /// STALE-tagged. Advisory only — it sits beside the gate, never drives it.
    QString grid_line;

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
    /// Click-to-inspect body for the PM KPI (not an orbit node). Always filled
    /// by present_bot_cockpit so the strip entry stays inspectable. Read-only.
    QString postmortem_detail;

    // ── what the record teaches (issue #174) ───────────────────────────────
    /// The edge autopsy's standing conclusions, header first — taken from the
    /// BOT panel's view rather than re-read here, for the same reason the mood
    /// is taken from it: a second reader of one artifact is a second thing to
    /// disagree with the CLI. `lessons_roles` is one colour role per line at
    /// the same index; a stale artifact arrives with its greens already
    /// demoted, so this scene cannot paint a stale lesson healthy.
    QStringList lessons;
    QStringList lessons_roles;
    /// The same lines without the claim and key numbers, for the widths this
    /// scene actually has. Drawing `lessons` in a fixed-width scene elides
    /// from the right, which is where the sample size is — so a narrow cockpit
    /// would show conclusions with no denominator. Same indices as `lessons`.
    QStringList lessons_compact;
    bool lessons_available = false;
    bool lessons_stale = false;

    /// The BOT tab suggests the cockpit exactly while the loop is running.
    bool suggest_cockpit = false;

    // ── health-first strip (HARVEST -> THR -> 15M -> DECIDE -> SETTLE) ─────
    // Additive: the mood/banner above answer "is the loop ticking"; these
    // answer "is the pipeline that feeds it healthy" — a dead feed or a stale
    // report must not hide behind a ticking paper loop. CALIBRATE is split so
    // a trusted threshold book cannot hide an untrusted 15m scoreboard.
    QList<BotCockpitHealthStage> health_stages;
    QString health_banner;                       ///< the health-first headline (worst stage)
    QString health_role = QStringLiteral("grey");///< colour of the headline

    // ── THRESHOLD / KXBTCD scoreboard hero (pinned above the rain) ─────────
    // Paper ambition family. Same facts as the CALIBRATOR orbit node / KPI,
    // never truncated by the strip. `hero_known` means Brier numbers exist.
    QString threshold_hero_line;
    QString threshold_hero_role = QStringLiteral("grey");
    int threshold_hero_scored = 0;
    int threshold_hero_floor = 100;
    bool threshold_hero_known = false;

    // KXBTC15M remains on the KPI strip + orbit node (observe/scoreboard).
    // Kept as hero_* fields only for a one-release paint fallback if the
    // threshold report is missing — prefer threshold_hero_* in the view.
    QString kxbtc15m_hero_line;
    QString kxbtc15m_hero_role = QStringLiteral("grey");
    int kxbtc15m_hero_scored = 0;
    int kxbtc15m_hero_floor = 100;
    bool kxbtc15m_hero_known = false;

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
///
/// Distance glyph: threshold books fall `sigma` (required_move_sigma);
/// KXBTC15M directional books fall `open` (signed_distance_bps from the
/// window open). Mixing them would make a 15m race look like a strike book.
inline void build_glyphs(const QJsonObject& prediction, BotCockpitColumn* column) {
    const QJsonObject features = prediction.value(QStringLiteral("features")).toObject();
    bool mid_known = false;
    const double mid =
        number_or(prediction.value(QStringLiteral("market_yes_mid")), 0.0, &mid_known);
    bool p_known = false;
    const double p = number_or(prediction.value(QStringLiteral("p_yes_full")), 0.0, &p_known);
    // `open_price` is the KXBTC15M feature; threshold books also carry
    // signed_distance_bps (vs strike), so that alone must not flip the glyph.
    const bool directional = features.contains(QStringLiteral("open_price"));
    bool sigma_known = false;
    const double sigma =
        number_or(features.value(QStringLiteral("required_move_sigma")), 0.0, &sigma_known);
    bool open_bps_known = false;
    const double open_bps =
        number_or(features.value(QStringLiteral("signed_distance_bps")), 0.0, &open_bps_known);

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
    if (directional) {
        column->glyphs << (open_bps_known
                               ? glyph(QStringLiteral("open"), open_bps,
                                       QStringLiteral("%1%2")
                                           .arg(open_bps >= 0 ? QStringLiteral("+") : QString())
                                           .arg(open_bps, 0, 'f', 1))
                               : missing_glyph(QStringLiteral("open")));
    } else {
        column->glyphs << (sigma_known ? glyph(QStringLiteral("sigma"), sigma,
                                               QString::number(sigma, 'f', 2))
                                       : missing_glyph(QStringLiteral("sigma")));
    }
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
/// what the bot is doing. `report` is calibrator.json (threshold families),
/// `kxbtc15m_report` is kxbtc15m-calibrator.json (KXBTC15M only),
/// `commodities_15m_report` is commodities-15m-calibrator.json (GOLD/SILVER/WTI),
/// `gate` kalshi-bot-gate.json, `ledger_rows` the ledger tail, `live_status` the
/// `kalshi auto live status` object the screen polls. KXBTC15M / commodity-15m
/// predictions that appear inside the threshold report are ignored — each
/// directional calibrator owns its family, matching `KalshiBotDecision`.
/// Synthetic inspect id for the PM KPI strip entry (not an orbit node).
inline constexpr auto kBotCockpitPostmortemInspectId = "__kpi_postmortem__";

/// True when a postmortem summary object has a usable settled count.
inline bool postmortem_summary_known(const QJsonObject& summary) {
    return !summary.isEmpty() &&
           kalshi_bot_detail::is_number(summary.value(QStringLiteral("settled")));
}

/// Prefer current-rules summary for judging new trading; fall back to historic.
inline QJsonObject postmortem_judge_summary(const QJsonObject& current,
                                           const QJsonObject& historic) {
    if (postmortem_summary_known(current)) return current;
    return historic;
}

/// Compact KPI strip line. Prefer current-rules cohort (new trading scorecard).
/// Always returns a line (UNAVAILABLE when missing) so the strip length stays
/// stable for the additive health pin — never invents W/L from the gate.
inline QString postmortem_kpi_line(const QJsonObject& summary) {
    if (!postmortem_summary_known(summary)) {
        return QStringLiteral(
            "PM UNAVAILABLE · run kalshi bot postmortem");
    }
    const QString cohort = summary.value(QStringLiteral("cohort")).toString();
    const QString tag = cohort == QLatin1String("current_rules")
                            ? QStringLiteral("CUR ")
                            : cohort == QLatin1String("historic")
                                  ? QStringLiteral("HIST ")
                                  : QString();
    const int wins = summary.value(QStringLiteral("wins")).toInt();
    const int losses = summary.value(QStringLiteral("losses")).toInt();
    const int early = summary.value(QStringLiteral("early_exits")).toInt();
    const double net = summary.value(QStringLiteral("net_realized_pnl_usd")).toDouble();
    QString top = QStringLiteral("n/a");
    const QJsonArray modes = summary.value(QStringLiteral("loss_primary_modes")).toArray();
    if (!modes.isEmpty()) {
        const QJsonArray pair = modes.first().toArray();
        if (pair.size() >= 1) top = pair.at(0).toString(top);
    }
    if (wins + losses == 0 && early > 0) {
        return QStringLiteral("PM %1%2 cashouts · net %3")
            .arg(tag)
            .arg(early)
            .arg(bot_cockpit_detail::money(net));
    }
    return QStringLiteral("PM %1%2W/%3L net %4 · top=%5")
        .arg(tag)
        .arg(wins)
        .arg(losses)
        .arg(bot_cockpit_detail::money(net), top);
}

/// Append one cohort's autopsy body (no title). Empty when summary unknown.
inline void append_postmortem_cohort_detail(QStringList& lines, const QJsonObject& summary,
                                            const QString& heading) {
    using bot_cockpit_detail::money;
    if (!postmortem_summary_known(summary)) return;
    lines << heading;
    const int settled = summary.value(QStringLiteral("settled")).toInt();
    const int wins = summary.value(QStringLiteral("wins")).toInt();
    const int losses = summary.value(QStringLiteral("losses")).toInt();
    const int early = summary.value(QStringLiteral("early_exits")).toInt();
    const double net = summary.value(QStringLiteral("net_realized_pnl_usd")).toDouble();
    const double fees = summary.value(QStringLiteral("fees_usd")).toDouble();
    lines << QStringLiteral("settled %1 · %2W / %3L · cashouts %4 · net %5 · fees %6")
                 .arg(settled)
                 .arg(wins)
                 .arg(losses)
                 .arg(early)
                 .arg(money(net), money(fees));
    if (summary.value(QStringLiteral("mean_win_usd")).isDouble() ||
        summary.value(QStringLiteral("mean_loss_usd")).isDouble()) {
        lines << QStringLiteral("mean win %1 · mean loss %2")
                     .arg(summary.value(QStringLiteral("mean_win_usd")).isDouble()
                              ? money(summary.value(QStringLiteral("mean_win_usd")).toDouble())
                              : QStringLiteral("—"),
                          summary.value(QStringLiteral("mean_loss_usd")).isDouble()
                              ? money(summary.value(QStringLiteral("mean_loss_usd")).toDouble())
                              : QStringLiteral("—"));
    }
    const QJsonObject meas = summary.value(QStringLiteral("measurement")).toObject();
    if (!meas.isEmpty()) {
        const QJsonObject gf = meas.value(QStringLiteral("gate_filter")).toObject();
        if (!gf.isEmpty()) {
            lines << QStringLiteral("filter %1 since_ms=%2")
                         .arg(gf.value(QStringLiteral("mode")).toString(QStringLiteral("—")),
                              QString::number(
                                  static_cast<qint64>(gf.value(QStringLiteral("since_ms")).toDouble())));
        }
        lines << QStringLiteral(
                     "measurement: fade_lifts=%1 · early=%2 · cheap_no=%3 · fav_lost=%4 · "
                     "cross_wrong=%5 · mid_path≥2=%6")
                     .arg(meas.value(QStringLiteral("fade_ban_lifts")).toInt())
                     .arg(meas.value(QStringLiteral("early_exits")).toInt())
                     .arg(meas.value(QStringLiteral("cheap_no_crushed_by_yes")).toInt())
                     .arg(meas.value(QStringLiteral("favourite_lost_full_stake")).toInt())
                     .arg(meas.value(QStringLiteral("wrong_side_after_crossing_spread")).toInt())
                     .arg(meas.value(QStringLiteral("settlements_with_mid_path")).toInt());
    }
    const QJsonObject by_family = summary.value(QStringLiteral("by_family")).toObject();
    if (!by_family.isEmpty()) {
        lines << QStringLiteral("by family:");
        QStringList fams = by_family.keys();
        fams.sort();
        for (const QString& fam : fams) {
            const QJsonObject row = by_family.value(fam).toObject();
            const int fam_early = row.value(QStringLiteral("early_exits")).toInt();
            lines << QStringLiteral("  %1  %2W/%3L%4  net %5")
                         .arg(fam)
                         .arg(row.value(QStringLiteral("wins")).toInt())
                         .arg(row.value(QStringLiteral("losses")).toInt())
                         .arg(fam_early > 0 ? QStringLiteral(" · %1xo").arg(fam_early) : QString(),
                              money(row.value(QStringLiteral("net_pnl_usd")).toDouble()));
        }
    }
    const QJsonArray modes = summary.value(QStringLiteral("loss_primary_modes")).toArray();
    if (!modes.isEmpty()) {
        lines << QStringLiteral("loss modes:");
        for (int i = 0; i < modes.size() && i < 8; ++i) {
            const QJsonArray pair = modes.at(i).toArray();
            if (pair.size() < 2) continue;
            lines << QStringLiteral("  %1×  %2")
                         .arg(pair.at(1).toInt())
                         .arg(pair.at(0).toString());
        }
    }
    const QJsonArray worst = summary.value(QStringLiteral("worst_losses")).toArray();
    if (!worst.isEmpty()) {
        lines << QStringLiteral("worst losses:");
        for (int i = 0; i < worst.size() && i < 6; ++i) {
            const QJsonObject row = worst.at(i).toObject();
            lines << QStringLiteral("  %1  %2  %3@%4 → %5  mode=%6  gamma=%7  quote=%8")
                         .arg(money(row.value(QStringLiteral("pnl")).toDouble()),
                              row.value(QStringLiteral("ticker")).toString(),
                              row.value(QStringLiteral("side")).toString())
                         .arg(row.value(QStringLiteral("price")).toDouble(), 0, 'f', 2)
                         .arg(row.value(QStringLiteral("result")).toString(),
                              row.value(QStringLiteral("mode")).toString(QStringLiteral("—")),
                              row.value(QStringLiteral("gamma")).toString(QStringLiteral("—")),
                              row.value(QStringLiteral("quote")).toString(QStringLiteral("—")));
            const QJsonArray lessons = row.value(QStringLiteral("lessons")).toArray();
            for (int j = 0; j < lessons.size() && j < 2; ++j) {
                const QString lesson = lessons.at(j).toString();
                if (!lesson.isEmpty()) lines << QStringLiteral("      · %1").arg(lesson);
            }
        }
    }
    const QJsonArray recs = summary.value(QStringLiteral("recommendations")).toArray();
    if (!recs.isEmpty()) {
        lines << QStringLiteral("recommendations:");
        for (int i = 0; i < recs.size() && i < 4; ++i) {
            const QJsonObject rec = recs.at(i).toObject();
            lines << QStringLiteral("  [%1] %2")
                         .arg(rec.value(QStringLiteral("priority")).toInt())
                         .arg(rec.value(QStringLiteral("id")).toString());
            const QString claim = rec.value(QStringLiteral("claim")).toString();
            const QString action = rec.value(QStringLiteral("action")).toString();
            if (!claim.isEmpty()) lines << QStringLiteral("      %1").arg(claim);
            if (!action.isEmpty()) lines << QStringLiteral("      → %1").arg(action);
        }
    }
}

/// Multi-line bid-postmortem inspect body. Current-rules first (new scorecard);
/// historic kept for learning — never conflate the two. Read-only.
inline QString postmortem_inspect_detail(const QJsonObject& summary,
                                         const QJsonObject& historic = {}) {
    if (!postmortem_summary_known(summary) && !postmortem_summary_known(historic)) {
        return QStringLiteral(
            "BID POSTMORTEM UNAVAILABLE\n"
            "No kalshi-bot-postmortem-summary.json yet.\n"
            "Rebuild from the BOT tab (REBUILD POSTMORTEM) or:\n"
            "  openterminalcli kalshi bot postmortem\n"
            "Inspect only — never arms.");
    }
    QStringList lines;
    lines << QStringLiteral("BID POSTMORTEM · inspect only · never arms");
    lines << QStringLiteral(
        "CURRENT RULES = new trading scorecard · HISTORIC = learn / sealed gate");
    const QJsonObject current =
        summary.value(QStringLiteral("cohort")).toString() == QLatin1String("current_rules")
            ? summary
            : (postmortem_summary_known(summary) &&
                       summary.value(QStringLiteral("cohort")).toString() !=
                           QLatin1String("historic")
                   ? summary
                   : QJsonObject{});
    const QJsonObject hist = postmortem_summary_known(historic)
                                 ? historic
                                 : (summary.value(QStringLiteral("cohort")).toString() ==
                                            QLatin1String("historic")
                                        ? summary
                                        : QJsonObject{});
    const QJsonObject judge = postmortem_judge_summary(current, hist);
    if (postmortem_summary_known(current)) {
        append_postmortem_cohort_detail(
            lines, current,
            QStringLiteral("── CURRENT RULES (judge new trading here) ──"));
    } else if (postmortem_summary_known(judge) &&
               judge.value(QStringLiteral("cohort")).toString() != QLatin1String("historic")) {
        append_postmortem_cohort_detail(lines, judge,
                                        QStringLiteral("── SCORECARD ──"));
    } else {
        lines << QStringLiteral(
            "── CURRENT RULES ── unavailable (no mid_path / fade markers yet)");
    }
    if (postmortem_summary_known(hist)) {
        if (!lines.isEmpty()) lines << QString();
        append_postmortem_cohort_detail(
            lines, hist,
            QStringLiteral("── HISTORIC (learning + sealed-gate ledger) ──"));
    } else if (postmortem_summary_known(summary) &&
               summary.value(QStringLiteral("cohort")).toString() ==
                   QLatin1String("historic")) {
        append_postmortem_cohort_detail(
            lines, summary,
            QStringLiteral("── HISTORIC (learning + sealed-gate ledger) ──"));
    }
    lines << QStringLiteral("Tip: REBUILD POSTMORTEM writes both cohorts · never arms");
    return lines.join(QLatin1Char('\n'));
}

inline BotCockpitScene present_bot_cockpit(const KalshiBotPanelView& panel,
                                           const QJsonObject& report,
                                           const QJsonObject& gate,
                                           const QJsonArray& ledger_rows,
                                           const QJsonObject& live_status,
                                           qint64 now_ms,
                                           const QByteArray& grid_json = QByteArray(),
                                           int max_columns = kBotCockpitMaxColumns,
                                           int max_pulses = kBotCockpitMaxPulses,
                                           const BotCockpitFeedHealth& feed = {},
                                           const QJsonObject& kxbtc15m_report = {},
                                           const QJsonObject& commodities_15m_report = {},
                                           const QJsonObject& postmortem_summary = {},
                                           const QJsonObject& postmortem_historic = {}) {
    using namespace bot_cockpit_detail;
    BotCockpitScene scene;
    // Advisory strategy-grid line, read-only over the engine's verdict file.
    scene.grid_line = services::prediction::kalshi_ns::grid_cockpit_line(
        services::prediction::kalshi_ns::parse_grid_latest(grid_json, now_ms));

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

    // ── what the record teaches (issue #174) ───────────────────────────────
    // Straight off the panel's view: same artifact, same formatter, same
    // sample sizes and same roles the BOT tab and `kalshi bot lessons` show.
    scene.lessons = panel.lessons;
    scene.lessons_roles = panel.lessons_roles;
    scene.lessons_compact = panel.lessons_compact;
    scene.lessons_available = panel.lessons_available;
    scene.lessons_stale = panel.lessons_stale;

    // ── rain: one column per prediction, split by calibrator family ────────
    // Threshold books from calibrator.json; KXBTC15M from kxbtc15m-calibrator;
    // GOLD/SILVER/WTI from commodities-15m-calibrator. Each source freezes on
    // its own generation stamp — a fresh race must not keep a stale threshold
    // column falling, and trust never crosses family boundaries.
    struct ReportStamp {
        qint64 generated_at_ms = 0;
        qint64 age_ms = -1;
        bool frozen = true;
        QString freeze_reason;
        QString source;  ///< "threshold" | "kxbtc15m" | "commodities15m"
    };
    const qint64 freeze_bound = bot_cockpit_freeze_bound_ms();
    const auto stamp_of = [&](const QJsonObject& source_report, const QString& source) {
        ReportStamp stamp;
        stamp.source = source;
        stamp.generated_at_ms =
            static_cast<qint64>(source_report.value(QStringLiteral("generated_at_ms")).toDouble());
        if (stamp.generated_at_ms > 0) stamp.age_ms = now_ms - stamp.generated_at_ms;
        stamp.frozen =
            auto_cockpit_detail::timestamp_stale(stamp.generated_at_ms, now_ms, freeze_bound);
        stamp.freeze_reason =
            stamp.generated_at_ms <= 0
                ? QStringLiteral("the %1 report carries no generation time").arg(source)
            : stamp.age_ms < 0
                ? QStringLiteral("the %1 report is dated in the future — clock skew, not freshness")
                      .arg(source)
                : QStringLiteral("the %1 report is %2, past the %3s age the bot itself refuses it at")
                      .arg(source, auto_cockpit_age_text(stamp.age_ms))
                      .arg(freeze_bound / 1'000);
        return stamp;
    };
    const ReportStamp threshold_stamp = stamp_of(report, QStringLiteral("threshold"));
    const ReportStamp kxbtc15m_stamp = stamp_of(kxbtc15m_report, QStringLiteral("kxbtc15m"));
    const ReportStamp commodities_stamp =
        stamp_of(commodities_15m_report, QStringLiteral("commodities15m"));

    QJsonObject predictions;
    QHash<QString, ReportStamp> stamp_by_ticker;
    const auto take_filtered = [&](const QJsonObject& filtered, const ReportStamp& stamp) {
        for (auto it = filtered.constBegin(); it != filtered.constEnd(); ++it) {
            predictions.insert(it.key(), it.value());
            stamp_by_ticker.insert(it.key(), stamp);
        }
    };
    const auto take_family = [&](const QJsonObject& source_report, const ReportStamp& stamp,
                                 bool keep_kxbtc15m) {
        take_filtered(
            services::prediction::kalshi_ns::KalshiBotDecision::filter_predictions_for_family(
                source_report, keep_kxbtc15m)
                .value(QStringLiteral("predictions"))
                .toObject(),
            stamp);
    };
    take_family(report, threshold_stamp, /*keep_kxbtc15m=*/false);
    take_family(kxbtc15m_report, kxbtc15m_stamp, /*keep_kxbtc15m=*/true);
    take_filtered(services::prediction::kalshi_ns::KalshiBotDecision::filter_commodity_15m_predictions(
                      commodities_15m_report)
                      .value(QStringLiteral("predictions"))
                      .toObject(),
                  commodities_stamp);

    scene.report_present = !predictions.isEmpty();
    // Newest contributing source age — the census/health stamp for "is any
    // report current", not a claim that every column shares one clock.
    qint64 generated_at_ms = 0;
    bool any_unfrozen = false;
    for (auto it = stamp_by_ticker.constBegin(); it != stamp_by_ticker.constEnd(); ++it) {
        generated_at_ms = std::max(generated_at_ms, it.value().generated_at_ms);
        if (!it.value().frozen) any_unfrozen = true;
    }
    if (generated_at_ms > 0) scene.report_age_ms = now_ms - generated_at_ms;
    const bool report_frozen = scene.report_present && !any_unfrozen;
    const QString freeze_reason =
        report_frozen
            ? QStringLiteral("every drawn report is past the age the bot itself refuses")
            : QString();

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
    // The newest bid's OWN timestamp, across every ticker — not the newest
    // ledger row of any kind. DECIDE's recency check (kDecideBidRecencyMs)
    // needs the age of the winning bid itself, since the byte-window tail can
    // carry a fresh PASS beside a long-stale bid.
    qint64 newest_bid_ts_ms = -1;
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
                if (ts > newest_bid_ts_ms) newest_bid_ts_ms = ts;
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
    const auto pulse_report = [&](const ReportStamp& stamp, const QString& label, int count) {
        if (stamp.generated_at_ms <= 0 || count <= 0) return;
        BotCockpitPulse pulse;
        pulse.kind = QStringLiteral("calibrator");
        pulse.key = QStringLiteral("%1:%2").arg(stamp.source).arg(stamp.generated_at_ms);
        pulse.ts_ms = stamp.generated_at_ms;
        pulse.role = stamp.frozen ? QStringLiteral("amber") : QStringLiteral("cyan");
        pulse.text = QStringLiteral("%1 REFRESH · %2 contracts · %3")
                         .arg(label)
                         .arg(count)
                         .arg(stamp.frozen ? stamp.freeze_reason
                                           : auto_cockpit_age_text(stamp.age_ms));
        pulses << pulse;
    };
    int threshold_count = 0;
    int kxbtc15m_count = 0;
    int commodities_count = 0;
    for (auto it = stamp_by_ticker.constBegin(); it != stamp_by_ticker.constEnd(); ++it) {
        if (it.value().source == QLatin1String("kxbtc15m")) ++kxbtc15m_count;
        else if (it.value().source == QLatin1String("commodities15m")) ++commodities_count;
        else ++threshold_count;
    }
    pulse_report(threshold_stamp, QStringLiteral("CALIBRATOR"), threshold_count);
    pulse_report(kxbtc15m_stamp, QStringLiteral("KXBTC15M"), kxbtc15m_count);
    pulse_report(commodities_stamp, QStringLiteral("COMMOD15M"), commodities_count);
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
    int closed_15m_dropped = 0;
    const QStringList tickers = predictions.keys();
    for (const QString& ticker : tickers) {
        // Defense in depth: calibrator should not publish closed races, but
        // frozen daemon seconds_left historically did — never paint them as FLOW.
        if (!bot_cockpit_15m_still_open(ticker, now_ms)) {
            ++closed_15m_dropped;
            continue;
        }
        BotCockpitColumn column;
        column.ticker = ticker;
        build_glyphs(predictions.value(ticker).toObject(), &column);
        const ReportStamp stamp = stamp_by_ticker.value(ticker);
        column.signal_source = stamp.source;
        column.frozen = stamp.frozen;
        if (stamp.frozen) column.frozen_reason = stamp.freeze_reason;
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

    // Family census counts drawn columns (closed 15m already dropped).
    threshold_count = 0;
    kxbtc15m_count = 0;
    commodities_count = 0;
    for (const auto& column : columns) {
        if (column.signal_source == QLatin1String("kxbtc15m")) ++kxbtc15m_count;
        else if (column.signal_source == QLatin1String("commodities15m")) ++commodities_count;
        else ++threshold_count;
    }

    // columns_total = open/watched only (closed 15m never count as FLOW).
    scene.columns_total = columns.size();
    // Strike/threshold first (paper ambition), then BTC 15m, then commodities;
    // within a family |edge| so a cap drops quiet books before open races.
    // Unmeasured edges sort last; ticker breaks ties.
    std::stable_sort(columns.begin(), columns.end(),
                     [](const BotCockpitColumn& a, const BotCockpitColumn& b) {
                         const int ar = bot_cockpit_rain_family_rank(a.signal_source);
                         const int br = bot_cockpit_rain_family_rank(b.signal_source);
                         if (ar != br) return ar < br;
                         if (a.edge_known != b.edge_known) return a.edge_known;
                         if (a.edge_known && a.abs_edge != b.abs_edge) return a.abs_edge > b.abs_edge;
                         return a.ticker < b.ticker;
                     });
    if (max_columns >= 0 && columns.size() > max_columns) columns = columns.mid(0, max_columns);
    scene.columns = columns;
    for (const auto& column : std::as_const(scene.columns))
        if (column.frozen) ++scene.columns_frozen;

    if (!scene.report_present) {
        scene.census = QStringLiteral("NO FLOW · no predictions in %1, %2, or %3 — nothing for this "
                                      "cockpit to render")
                           .arg(QString::fromLatin1(kKalshiCalibratorFile),
                                QString::fromLatin1(kKalshiKxbtc15mCalibratorFile),
                                QString::fromLatin1(kKalshiCommodities15mCalibratorFile));
    } else if (scene.columns.isEmpty()) {
        // Report files exist, but every open contract was filtered (closed 15m)
        // or the family has not published the next window yet.
        scene.census = QStringLiteral("NO FLOW · waiting for next open contract · L→R · ambition "
                                      "KXBTCD · 0 threshold · 0 kxbtc15m · 0 commodities15m");
    } else if (scene.columns_total > scene.columns.size()) {
        scene.census = QStringLiteral("%1 of %2 watched contracts · L→R · threshold first · ambition "
                                      "KXBTCD · |edge| · %3 not drawn · %4 threshold · %5 kxbtc15m · "
                                      "%6 commodities15m")
                           .arg(scene.columns.size())
                           .arg(scene.columns_total)
                           .arg(scene.columns_total - scene.columns.size())
                           .arg(threshold_count)
                           .arg(kxbtc15m_count)
                           .arg(commodities_count);
    } else {
        scene.census = QStringLiteral("%1 watched contracts · all drawn · L→R · threshold first · "
                                      "ambition KXBTCD · %2 threshold · %3 kxbtc15m · %4 "
                                      "commodities15m")
                           .arg(scene.columns_total)
                           .arg(threshold_count)
                           .arg(kxbtc15m_count)
                           .arg(commodities_count);
    }
    if (closed_15m_dropped > 0)
        scene.census += QStringLiteral(" · %1 closed 15m omitted").arg(closed_15m_dropped);
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
                                   const QString& role, bool known,
                                   const QString& detail = {}) {
        BotCockpitNode node;
        node.id = id;
        node.label = label;
        node.value = value;
        node.role = role;
        node.known = known;
        node.detail = detail;
        scene.nodes << node;
    };

    // CALIBRATOR — the threshold report's OWN track record (issue #171). The
    // gate's brier_beats_market scores the bot's settled bids — a different
    // measure; both stay on this scene with labels that say which is which.
    const QJsonValue brier_full = report.value(QStringLiteral("brier_full"));
    const QJsonValue brier_mid_raw = report.value(QStringLiteral("brier_market_mid_raw"));
    if (is_number(brier_full) && is_number(brier_mid_raw)) {
        const bool adds_value = KalshiBotDecision::signal_trusted(report);
        add_node(QStringLiteral("calibrator"),
                 QStringLiteral("CALIBRATOR — THRESHOLD TRACK RECORD"),
                 // Same seam as kxbtc15m_scoreboard_line: on this deployment
                 // brier_full < brier_market_mid_raw while the node read "NO
                 // EDGE YET", because the refusal comes from the bet-eligible
                 // subset. That evidence is now on the node beside the verdict
                 // that rests on it.
                 QStringLiteral("Brier %1 vs raw mid %2 on %3 scored contracts · %4 · %5")
                     .arg(brier_full.toDouble(), 0, 'f', 4)
                     .arg(brier_mid_raw.toDouble(), 0, 'f', 4)
                     .arg(report.value(QStringLiteral("scored_contracts")).toInt())
                     .arg(KalshiBotDecision::bet_eligible_evidence_text(report),
                          KalshiBotDecision::trust_badge_text(report)),
                 adds_value ? QStringLiteral("green") : QStringLiteral("amber"), true);
    } else {
        add_node(QStringLiteral("calibrator"), QStringLiteral("CALIBRATOR — THRESHOLD"),
                 report.isEmpty()
                     ? QStringLiteral("MISSING · no %1")
                           .arg(QString::fromLatin1(kKalshiCalibratorFile))
                     : QStringLiteral("NOT SCORED · the report carries no Brier track record"),
                 QStringLiteral("grey"), false);
    }

    // KXBTC15M — the directional scoreboard. Own Brier, own floor, own trust;
    // never borrowed from the threshold calibrator.
    {
        const QString line = kxbtc15m_scoreboard_line(kxbtc15m_report);
        const bool known = !kxbtc15m_report.isEmpty() &&
                           kxbtc15m_report.value(QStringLiteral("brier_full")).isDouble() &&
                           kxbtc15m_report.value(QStringLiteral("brier_market_mid_raw")).isDouble();
        const bool adds_value = KalshiBotDecision::signal_trusted(kxbtc15m_report);
        add_node(QStringLiteral("kxbtc15m"),
                 QStringLiteral("KXBTC15M — DIRECTIONAL SCOREBOARD · click"), line,
                 !known              ? QStringLiteral("grey")
                 : adds_value        ? QStringLiteral("green")
                                     : QStringLiteral("amber"),
                 known, outside_info_inspect_detail(kxbtc15m_report));
    }

    // Commodities 15m — GOLD/SILVER/WTI races. Own file, own trust.
    {
        const QString line = commodities_15m_scoreboard_line(commodities_15m_report);
        const bool known = !commodities_15m_report.isEmpty() &&
                           commodities_15m_report.value(QStringLiteral("brier_full")).isDouble() &&
                           commodities_15m_report.value(QStringLiteral("brier_market_mid_raw"))
                               .isDouble();
        const bool adds_value = KalshiBotDecision::signal_trusted(commodities_15m_report);
        add_node(QStringLiteral("commodities15m"),
                 QStringLiteral("COMMODITIES 15M — DIRECTIONAL SCOREBOARD · click"), line,
                 !known       ? QStringLiteral("grey")
                 : adds_value ? QStringLiteral("green")
                              : QStringLiteral("amber"),
                 known, outside_info_inspect_detail(commodities_15m_report));
    }

    // MODEL vs BOT-SETTLED Brier — the split that decides whether paper may
    // promote. Calibrator `brier_full` scores the model; gate
    // `brier_beats_market.brier_bot` scores the bot's settled bids. They must
    // never be conflated on one line.
    {
        QJsonObject brier_crit;
        for (const auto& value : gate.value(QStringLiteral("criteria")).toArray()) {
            const QJsonObject c = value.toObject();
            if (c.value(QStringLiteral("id")).toString() == QLatin1String("brier_beats_market")) {
                brier_crit = c;
                break;
            }
        }
        const bool bot_known = brier_crit.value(QStringLiteral("brier_available")).toBool() &&
                               brier_crit.value(QStringLiteral("brier_bot")).isDouble() &&
                               brier_crit.value(QStringLiteral("brier_market_baseline")).isDouble();
        const bool model_known = is_number(brier_full) && is_number(brier_mid_raw);
        if (bot_known || model_known) {
            const bool bot_beats =
                bot_known && brier_crit.value(QStringLiteral("brier_bot")).toDouble() <
                                 brier_crit.value(QStringLiteral("brier_market_baseline")).toDouble();
            const QString model_text =
                model_known ? QStringLiteral("model %1 vs mid %2")
                                  .arg(brier_full.toDouble(), 0, 'f', 4)
                                  .arg(brier_mid_raw.toDouble(), 0, 'f', 4)
                            : QStringLiteral("model unavailable");
            const QString bot_text =
                bot_known ? QStringLiteral("bot-settled %1 vs mid %2 on %3")
                                .arg(brier_crit.value(QStringLiteral("brier_bot")).toDouble(), 0,
                                     'f', 4)
                                .arg(brier_crit.value(QStringLiteral("brier_market_baseline"))
                                         .toDouble(),
                                     0, 'f', 4)
                                .arg(brier_crit.value(QStringLiteral("scored_contracts")).toInt())
                          : QStringLiteral("bot-settled unavailable");
            add_node(QStringLiteral("brier_split"),
                     QStringLiteral("BRIER — MODEL vs BOT-SETTLED"),
                     QStringLiteral("%1 · %2 · %3")
                         .arg(model_text, bot_text,
                              !bot_known ? QStringLiteral("NO BOT SCORE")
                              : bot_beats  ? QStringLiteral("BOT BEATS MID")
                                           : QStringLiteral("BOT LOSES TO MID")),
                     !bot_known ? QStringLiteral("grey")
                     : bot_beats  ? QStringLiteral("green")
                                  : QStringLiteral("red"),
                     bot_known || model_known);
        } else {
            add_node(QStringLiteral("brier_split"),
                     QStringLiteral("BRIER — MODEL vs BOT-SETTLED"),
                     QStringLiteral("UNAVAILABLE — need calibrator Brier and a gate evaluation"),
                     QStringLiteral("grey"), false);
        }
    }

    // SETTLEMENTS / KPI — prefer CURRENT RULES postmortem (new trading
    // scorecard). Lifetime gate ledger stays on SEALED GATE + DRAWDOWN only;
    // do not judge new decide/cashout work by pre-gate losses.
    const QJsonObject gate_ledger = gate.value(QStringLiteral("ledger")).toObject();
    const bool gate_evaluated = !gate.isEmpty() &&
                                gate.value(QStringLiteral("evaluated")).toBool() &&
                                !gate_ledger.isEmpty();
    const QJsonObject current_pm =
        (postmortem_summary.value(QStringLiteral("cohort")).toString() ==
             QLatin1String("current_rules") ||
         (postmortem_summary_known(postmortem_summary) &&
          postmortem_summary.value(QStringLiteral("cohort")).toString() !=
              QLatin1String("historic")))
            ? postmortem_summary
            : QJsonObject{};
    const bool have_current_pm = postmortem_summary_known(current_pm);
    if (have_current_pm || gate_evaluated) {
        scene.kpi_available = true;
        const int settled =
            have_current_pm ? current_pm.value(QStringLiteral("settled")).toInt()
                            : gate_ledger.value(QStringLiteral("settled_bids")).toInt();
        const int wins = have_current_pm ? current_pm.value(QStringLiteral("wins")).toInt()
                                         : gate_ledger.value(QStringLiteral("wins")).toInt();
        const int losses = have_current_pm ? current_pm.value(QStringLiteral("losses")).toInt()
                                           : gate_ledger.value(QStringLiteral("losses")).toInt();
        const int early =
            have_current_pm ? current_pm.value(QStringLiteral("early_exits")).toInt() : 0;
        const double net =
            have_current_pm ? current_pm.value(QStringLiteral("net_realized_pnl_usd")).toDouble()
                            : gate_ledger.value(QStringLiteral("net_pnl_usd")).toDouble();
        const double fees =
            have_current_pm ? current_pm.value(QStringLiteral("fees_usd")).toDouble()
                            : gate_ledger.value(QStringLiteral("fees_usd")).toDouble();
        const double drawdown = gate_ledger.value(QStringLiteral("max_drawdown_usd")).toDouble();
        const QJsonValue drawdown_cap =
            gate.value(QStringLiteral("params")).toObject().value(QStringLiteral("max_drawdown_usd"));
        const bool drawdown_within_cap =
            is_number(drawdown_cap) && drawdown <= drawdown_cap.toDouble();
        const QString settled_tag =
            have_current_pm ? QStringLiteral("CUR ") : QStringLiteral("HIST ");
        scene.kpi << QStringLiteral("SETTLED %1%2").arg(settled_tag).arg(settled)
                  << (have_current_pm && wins + losses == 0 && early > 0
                          ? QStringLiteral("CASHOUTS %1").arg(early)
                          : QStringLiteral("W/L %1-%2").arg(wins).arg(losses))
                  << QStringLiteral("NET %1%2").arg(settled_tag, money(net));
        if (gate_evaluated) {
            scene.kpi << QStringLiteral("HIST DD %1 / %2")
                             .arg(money(drawdown),
                                  is_number(drawdown_cap) ? money(drawdown_cap.toDouble())
                                                          : QStringLiteral("cap unknown"));
        }
        // The one number the issue asks to be coloured by sign. A zero P&L is
        // neither a win nor a loss and is not painted as one.
        scene.kpi_roles << QStringLiteral("grey") << QStringLiteral("grey")
                        << (net > 0   ? QStringLiteral("green")
                            : net < 0 ? QStringLiteral("red")
                                      : QStringLiteral("grey"));
        if (gate_evaluated) {
            // Lifetime drawdown vs sealed cap — promotion fact, not new-rules P&L.
            scene.kpi_roles << (!is_number(drawdown_cap) ? QStringLiteral("grey")
                                : drawdown_within_cap    ? QStringLiteral("grey")
                                                         : QStringLiteral("red"));
        }
        QString settle_value;
        if (have_current_pm) {
            settle_value =
                QStringLiteral(
                    "CURRENT RULES · %1 settled · %2W / %3L · %4 cashouts · net %5 after %6 fees")
                    .arg(settled)
                    .arg(wins)
                    .arg(losses)
                    .arg(early)
                    .arg(money(net), money(fees));
            if (gate_evaluated) {
                settle_value += QStringLiteral(
                                    " · lifetime gate %1W/%2L net %3 (not this scorecard)")
                                    .arg(gate_ledger.value(QStringLiteral("wins")).toInt())
                                    .arg(gate_ledger.value(QStringLiteral("losses")).toInt())
                                    .arg(money(gate_ledger.value(QStringLiteral("net_pnl_usd"))
                                                   .toDouble()));
            }
        } else {
            settle_value =
                QStringLiteral(
                    "HISTORIC (gate) · %1 settled · %2W / %3L · net %4 after %5 fees · no "
                    "current-rules window yet")
                    .arg(settled)
                    .arg(wins)
                    .arg(losses)
                    .arg(money(net), money(fees));
        }
        add_node(QStringLiteral("settlements"),
                 have_current_pm ? QStringLiteral("SETTLEMENTS · CURRENT RULES")
                                 : QStringLiteral("SETTLEMENTS · HISTORIC"),
                 settle_value,
                 net > 0 ? QStringLiteral("green")
                         : net < 0 ? QStringLiteral("red") : QStringLiteral("grey"),
                 true);
    } else {
        scene.kpi_unavailable_reason =
            gate.isEmpty()
                ? QStringLiteral("no current-rules postmortem and no %1 — run `kalshi bot "
                                 "postmortem`; the promotion gate has not scored the paper record")
                      .arg(QString::fromLatin1(kKalshiBotGateFile))
                : QStringLiteral("no current-rules postmortem and the gate published no ledger "
                                 "block (%1)")
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

    // THRESHOLD / KXBTCD pinned hero — paper ambition family. Same sentence as
    // the CALIBRATOR orbit node so hero / node cannot disagree. Not a 7th KPI
    // (pin stays 6); the strip still carries KXBTC15M as the observe scoreboard.
    // Leave hero fields empty when the strike report is absent so the view can
    // fall back to the 15m scoreboard pin.
    if (!report.isEmpty()) {
        const QString line = threshold_scoreboard_line(report);
        const bool known = report.value(QStringLiteral("brier_full")).isDouble() &&
                           report.value(QStringLiteral("brier_market_mid_raw")).isDouble();
        const bool adds_value = KalshiBotDecision::signal_trusted(report);
        const QString role = !known       ? QStringLiteral("grey")
                             : adds_value ? QStringLiteral("green")
                                          : QStringLiteral("amber");
        scene.threshold_hero_line = QStringLiteral("KXBTCD · %1").arg(line);
        scene.threshold_hero_role = role;
        scene.threshold_hero_scored = report.value(QStringLiteral("scored_contracts")).toInt();
        scene.threshold_hero_floor =
            report.value(QStringLiteral("min_scored_contracts")).toInt(100);
        scene.threshold_hero_known = known;
    }

    // KXBTC15M pace on the strip (observe/scoreboard; not the pinned ambition).
    {
        const QString line = kxbtc15m_scoreboard_line(kxbtc15m_report);
        const bool known = !kxbtc15m_report.isEmpty() &&
                           kxbtc15m_report.value(QStringLiteral("brier_full")).isDouble() &&
                           kxbtc15m_report.value(QStringLiteral("brier_market_mid_raw")).isDouble();
        const bool adds_value = KalshiBotDecision::signal_trusted(kxbtc15m_report);
        const QString role = !known       ? QStringLiteral("grey")
                             : adds_value ? QStringLiteral("green")
                                          : QStringLiteral("amber");
        scene.kpi << QStringLiteral("KXBTC15M %1").arg(line);
        scene.kpi_roles << role;
        scene.kxbtc15m_hero_line = QStringLiteral("KXBTC15M · %1").arg(line);
        scene.kxbtc15m_hero_role = role;
        scene.kxbtc15m_hero_scored =
            kxbtc15m_report.value(QStringLiteral("scored_contracts")).toInt();
        scene.kxbtc15m_hero_floor =
            kxbtc15m_report.value(QStringLiteral("min_scored_contracts")).toInt(100);
        scene.kxbtc15m_hero_known = known;
    }

    // Commodities 15m pace on the strip (orbit node twin).
    {
        const QString line = commodities_15m_scoreboard_line(commodities_15m_report);
        const bool known =
            !commodities_15m_report.isEmpty() &&
            commodities_15m_report.value(QStringLiteral("brier_full")).isDouble() &&
            commodities_15m_report.value(QStringLiteral("brier_market_mid_raw")).isDouble();
        const bool adds_value = KalshiBotDecision::signal_trusted(commodities_15m_report);
        const QString role = !known       ? QStringLiteral("grey")
                             : adds_value ? QStringLiteral("green")
                                          : QStringLiteral("amber");
        scene.kpi << QStringLiteral("COMMOD15M %1").arg(line);
        scene.kpi_roles << role;
    }

    // Strip twin of the brier_split orbit node — model vs bot-settled.
    {
        QJsonObject brier_crit;
        for (const auto& value : gate.value(QStringLiteral("criteria")).toArray()) {
            const QJsonObject c = value.toObject();
            if (c.value(QStringLiteral("id")).toString() == QLatin1String("brier_beats_market")) {
                brier_crit = c;
                break;
            }
        }
        const bool bot_known = brier_crit.value(QStringLiteral("brier_available")).toBool() &&
                               brier_crit.value(QStringLiteral("brier_bot")).isDouble();
        const bool model_known = is_number(report.value(QStringLiteral("brier_full")));
        const bool bot_beats =
            bot_known && brier_crit.value(QStringLiteral("brier_bot")).toDouble() <
                             brier_crit.value(QStringLiteral("brier_market_baseline")).toDouble();
        scene.kpi << QStringLiteral("BRIER model %1 | bot %2")
                         .arg(model_known ? QString::number(
                                                report.value(QStringLiteral("brier_full")).toDouble(),
                                                'f', 4)
                                          : QStringLiteral("—"),
                              bot_known ? QString::number(
                                              brier_crit.value(QStringLiteral("brier_bot")).toDouble(),
                                              'f', 4)
                                        : QStringLiteral("—"));
        scene.kpi_roles << (!bot_known ? QStringLiteral("grey")
                            : bot_beats  ? QStringLiteral("green")
                                         : QStringLiteral("red"));
    }

    // Bid postmortem strip (always present; UNAVAILABLE when the summary
    // file is missing). Display only — does not arm. No new orbit node.
    // Prefer current-rules (new trading scorecard); historic is inspect-only.
    {
        const QJsonObject judge =
            postmortem_judge_summary(postmortem_summary, postmortem_historic);
        const QString line = postmortem_kpi_line(judge);
        const bool known = postmortem_summary_known(judge);
        const double net = judge.value(QStringLiteral("net_realized_pnl_usd")).toDouble();
        scene.kpi << line;
        scene.kpi_roles << (!known     ? QStringLiteral("grey")
                            : net > 0  ? QStringLiteral("green")
                            : net < 0  ? QStringLiteral("red")
                                       : QStringLiteral("grey"));
        scene.postmortem_detail =
            postmortem_inspect_detail(postmortem_summary, postmortem_historic);
    }

    // SEALED GATE — the verdict and its criteria, rendered by the BOT panel's
    // own criterion formatter so the two surfaces print identical lines.
    if (gate.isEmpty()) {
        add_node(QStringLiteral("gate"), QStringLiteral("SEALED GATE · LIFETIME"),
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
        // Lifetime sealed promotion scorecard — not the new-rules P&L judge.
        // Age/colour match the BOT panel (issue #167).
        add_node(QStringLiteral("gate"), QStringLiteral("SEALED GATE · LIFETIME"),
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

    // ── health-first classifier ─────────────────────────────────────────────
    // The mood/banner above answer "is the loop ticking"; this answers "is the
    // pipeline that feeds it healthy" — a dead feed or a stale report must not
    // hide behind a ticking paper loop. Four stages, worst wins. Pure over
    // `feed`, `report`, `panel` and the ledger scan already done above
    // (`generated_at_ms`, `newest_bid`, `settlement`).
    {
        BotCockpitHealthStage harvest_stage;
        harvest_stage.id = QStringLiteral("harvest");
        harvest_stage.label = QStringLiteral("HARVEST");
        QString harvest_reason;
        if (!feed.readable) {
            // Rule: an UNKNOWN feed (the view supplied nothing) must never
            // render HARVEST green — grey/unknown is the honest answer.
            harvest_stage.role = QStringLiteral("grey");
            harvest_stage.value = QStringLiteral("unknown");
            harvest_reason = QStringLiteral("FEED UNKNOWN — no engine status was supplied");
        } else if (feed.newest_event_age_ms < 0 || feed.newest_event_age_ms > 300'000) {
            // Genuinely stale — the real down signal — regardless of the
            // connected bit: a market event age this old means the feed is
            // down whether or not the socket still reports connected.
            harvest_stage.role = QStringLiteral("red");
            harvest_stage.value = QStringLiteral("down");
            harvest_reason = QStringLiteral("FEED DOWN — %1")
                                  .arg(feed.last_error.isEmpty()
                                           ? QStringLiteral("no reason given")
                                           : feed.last_error);
        } else if (!feed.ws_connected && feed.newest_event_age_ms > 60'000) {
            harvest_stage.role = QStringLiteral("red");
            harvest_stage.value = QStringLiteral("down");
            harvest_reason = QStringLiteral("FEED DOWN — %1")
                                  .arg(feed.last_error.isEmpty()
                                           ? QStringLiteral("no reason given")
                                           : feed.last_error);
        } else if (feed.newest_event_age_ms > 60'000) {
            harvest_stage.role = QStringLiteral("amber");
            harvest_stage.value = span_text(feed.newest_event_age_ms);
            harvest_reason = QStringLiteral("FEED SLOW — newest market event %1 old")
                                  .arg(span_text(feed.newest_event_age_ms));
        } else if (!feed.ws_connected) {
            // A momentary disconnect beside still-fresh events (anchored on
            // the market-event AGE, a smooth clock) is de-bounced to amber
            // rather than flashing red on a single dropped connected bit.
            harvest_stage.role = QStringLiteral("amber");
            harvest_stage.value = span_text(feed.newest_event_age_ms);
            harvest_reason = QStringLiteral(
                "FEED SLOW — websocket disconnected but newest market event is %1 old")
                                  .arg(span_text(feed.newest_event_age_ms));
        } else {
            harvest_stage.role = QStringLiteral("green");
            harvest_stage.value = span_text(feed.newest_event_age_ms);
            harvest_reason = QStringLiteral("FEED LIVE");
        }

        // Per-family CALIBRATE chips: THR and 15M. Idle (no columns from that
        // source) is grey so a quiet family cannot redden the strip; the
        // banner still alarms when *no* family is active.
        const bool threshold_active = threshold_count > 0;
        const bool kxbtc15m_active = kxbtc15m_count > 0;
        const bool any_active = threshold_active || kxbtc15m_active;
        const bool threshold_trusted = KalshiBotDecision::signal_trusted(report);
        const bool kxbtc15m_trusted = KalshiBotDecision::signal_trusted(kxbtc15m_report);
        const int scored_15m = kxbtc15m_report.value(QStringLiteral("scored_contracts")).toInt();
        const int floor_15m =
            kxbtc15m_report.value(QStringLiteral("min_scored_contracts")).toInt(100);
        const QString progress_15m =
            QStringLiteral("%1/%2").arg(scored_15m).arg(floor_15m);

        const auto family_calibrate = [](const QString& id, const QString& label, bool active,
                                         const ReportStamp& stamp, bool trusted,
                                         const QString& trusted_value,
                                         const QString& untrusted_value) {
            BotCockpitHealthStage stage;
            stage.id = id;
            stage.label = label;
            if (!active) {
                stage.role = QStringLiteral("grey");
                stage.value = QStringLiteral("idle");
                return stage;
            }
            if (stamp.generated_at_ms <= 0 || stamp.frozen) {
                stage.role = QStringLiteral("red");
                stage.value = QStringLiteral("stale");
                return stage;
            }
            if (trusted) {
                stage.role = QStringLiteral("green");
                stage.value = trusted_value;
            } else {
                stage.role = QStringLiteral("amber");
                stage.value = untrusted_value;
            }
            return stage;
        };
        const BotCockpitHealthStage thr_stage = family_calibrate(
            QStringLiteral("calibrate_threshold"), QStringLiteral("THR"), threshold_active,
            threshold_stamp, threshold_trusted, QStringLiteral("trusted"),
            QStringLiteral("untrusted"));
        const BotCockpitHealthStage m15_stage = family_calibrate(
            QStringLiteral("calibrate_15m"), QStringLiteral("15M"), kxbtc15m_active,
            kxbtc15m_stamp, kxbtc15m_trusted, progress_15m, progress_15m);

        QString calibrate_reason;
        const bool any_stale =
            (threshold_active && threshold_stamp.frozen) || (kxbtc15m_active && kxbtc15m_stamp.frozen);
        const bool all_active_trusted =
            (!threshold_active || threshold_trusted) && (!kxbtc15m_active || kxbtc15m_trusted);
        if (!any_active || generated_at_ms <= 0 || scene.report_age_ms < 0 || any_stale) {
            calibrate_reason = QStringLiteral("CALIBRATOR STALE");
        } else if (all_active_trusted) {
            calibrate_reason = QStringLiteral("CALIBRATOR TRUSTED");
        } else {
            calibrate_reason = QStringLiteral(
                "SIGNAL UNTRUSTED — a live family has not beaten its market mid");
        }
        const bool calibrate_red =
            !any_active || thr_stage.role == QLatin1String("red") ||
            m15_stage.role == QLatin1String("red");
        const bool calibrate_amber =
            thr_stage.role == QLatin1String("amber") || m15_stage.role == QLatin1String("amber");

        BotCockpitHealthStage decide_stage;
        decide_stage.id = QStringLiteral("decide");
        decide_stage.label = QStringLiteral("DECIDE");
        QString decide_reason;
        // GREEN "bidding" needs the winning bid itself to be recent
        // (kDecideBidRecencyMs) — `newest_bid` comes from a BYTE-window
        // ledger tail, not a time window, so an old bid can still be sitting
        // in that window long after the loop stopped bidding. An old bid on a
        // running loop is "passing", not "bidding": the loop is fine, it just
        // has nothing to bid on right now.
        const bool has_recent_bid =
            !newest_bid.isEmpty() && newest_bid_ts_ms >= 0 &&
            now_ms - newest_bid_ts_ms <= kDecideBidRecencyMs;
        const QString newest_reason =
            newest_decision.value(QStringLiteral("reason_code")).toString();
        const bool paper_paused =
            newest_reason == QLatin1String(services::prediction::kalshi_ns::KalshiBotDecision::kGateFail) ||
            newest_reason ==
                QLatin1String(services::prediction::kalshi_ns::KalshiBotDecision::kDrawdownCap);
        if (panel.state != QStringLiteral("running")) {
            // Stopped (kill switch) or no recent tick (stale/off): the loop is
            // never blamed for its input, but it IS blamed for not looping.
            decide_stage.role = QStringLiteral("red");
            decide_stage.value = QStringLiteral("stopped");
            decide_reason = QStringLiteral("LOOP STOPPED");
        } else if (paper_paused) {
            // Gate FAIL / drawdown pause is intentional journal-only — amber,
            // not red: the loop is healthy, bidding is withheld on purpose.
            decide_stage.role = QStringLiteral("amber");
            decide_stage.value =
                newest_reason ==
                        QLatin1String(
                            services::prediction::kalshi_ns::KalshiBotDecision::kDrawdownCap)
                    ? QStringLiteral("drawdown")
                    : QStringLiteral("gate-fail");
            decide_reason = QStringLiteral("PAPER PAUSED — %1").arg(newest_reason);
        } else if (has_recent_bid) {
            decide_stage.role = QStringLiteral("green");
            decide_stage.value = QStringLiteral("bidding");
            decide_reason = QStringLiteral("BIDDING");
        } else {
            decide_stage.role = QStringLiteral("amber");
            decide_stage.value = QStringLiteral("passing");
            decide_reason = QStringLiteral("NO EDGE — the loop is ticking and passing");
        }

        BotCockpitHealthStage settle_stage;
        settle_stage.id = QStringLiteral("settle");
        settle_stage.label = QStringLiteral("SETTLE");
        // Informational only (never an alarm colour): context, not health.
        settle_stage.role = settlement.isEmpty() ? QStringLiteral("grey") : QStringLiteral("green");
        settle_stage.value = QString::number(settlement.size());

        scene.health_stages = {harvest_stage, thr_stage, m15_stage, decide_stage, settle_stage};

        if (harvest_stage.role == QStringLiteral("red")) {
            scene.health_role = QStringLiteral("red");
            scene.health_banner = harvest_reason;
        } else if (calibrate_red) {
            scene.health_role = QStringLiteral("red");
            scene.health_banner = calibrate_reason;
        } else if (decide_stage.role == QStringLiteral("red")) {
            scene.health_role = QStringLiteral("red");
            scene.health_banner = decide_reason;
        } else if (harvest_stage.role == QStringLiteral("amber")) {
            scene.health_role = QStringLiteral("amber");
            scene.health_banner = harvest_reason;
        } else if (calibrate_amber) {
            scene.health_role = QStringLiteral("amber");
            scene.health_banner = calibrate_reason;
        } else if (decide_stage.role == QStringLiteral("amber")) {
            scene.health_role = QStringLiteral("amber");
            scene.health_banner = decide_reason;
        } else if (harvest_stage.role == QStringLiteral("grey")) {
            // Grey (unknown) HARVEST is neither red nor amber above, so
            // without this clause the ladder falls straight through to GREEN
            // "ACTING" whenever CALIBRATE and DECIDE are both healthy — an
            // unreadable feed would then be rendered as a fully healthy
            // pipeline. An unknown feed caps the banner at amber; it is never
            // allowed to green.
            scene.health_role = QStringLiteral("amber");
            scene.health_banner = harvest_reason;
        } else {
            scene.health_role = QStringLiteral("green");
            scene.health_banner = QStringLiteral("ACTING — %1").arg(decide_reason);
        }
    }

    return scene;
}

/// The scene as the screen renders it: the four evidence files, read through
/// the one path module, and the BOT panel view built from three of them.
/// `feed` is supplied by the caller (the view, from kalshi-ws-engine.json and
/// the newest retained ticker event) — this function does no socket-health
/// I/O of its own, it only forwards what it is given to the pure presenter.
inline BotCockpitScene load_bot_cockpit_scene(const QJsonObject& live_status, qint64 now_ms,
                                              const BotCockpitFeedHealth& feed = {}) {
    const QJsonArray ledger = read_kalshi_bot_ledger_tail();
    const QJsonObject gate = read_kalshi_bot_gate();
    // The lessons artifact is read here and handed to the panel presenter, so
    // the cockpit's card comes out of the same call the BOT tab's does.
    const KalshiBotPanelView panel =
        present_kalshi_bot_panel(ledger, gate, live_status, now_ms, 8,
                                 read_kalshi_bot_stop_file(), {},
                                 kalshi_edge_read_report_file(kalshi_edge_report_path()));
    const auto read_report = [](const QString& path) {
        QJsonObject object;
        QFile file(path);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
            if (document.isObject()) object = document.object();
        }
        return object;
    };
    const QJsonObject report = read_report(kalshi_calibrator_path());
    const QJsonObject kxbtc15m_report = read_report(kalshi_kxbtc15m_calibrator_path());
    const QJsonObject commodities_15m_report =
        read_report(kalshi_commodities_15m_calibrator_path());
    // The advisory strategy-grid verdict, read-only; missing/garbage/stale is
    // handled inside present_bot_cockpit (fails closed to UNAVAILABLE/STALE).
    QByteArray grid_json;
    QFile grid_file(cli::kalshi_evidence_path(
        QStringLiteral("kalshi-strategy-grid-latest.json")));
    if (grid_file.open(QIODevice::ReadOnly)) grid_json = grid_file.readAll();
    // Current-rules first (cockpit reset for new trading); historic for learning.
    const QJsonObject postmortem_current = read_report(cli::kalshi_evidence_path(
        QStringLiteral("kalshi-bot-postmortem-summary-current.json")));
    const QJsonObject postmortem_historic = read_report(
        cli::kalshi_evidence_path(QStringLiteral("kalshi-bot-postmortem-summary.json")));
    return present_bot_cockpit(panel, report, gate, ledger, live_status, now_ms, grid_json,
                               kBotCockpitMaxColumns, kBotCockpitMaxPulses, feed,
                               kxbtc15m_report, commodities_15m_report, postmortem_current,
                               postmortem_historic);
}

} // namespace openmarketterminal::screens::kalshi
