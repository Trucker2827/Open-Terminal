// `kalshi bot` command family — the PAPER Kalshi bot, ladder rungs 1, 2, 4
// and 6.
//
// One tick (`once`/`run`) is: check the kill switch, settle whatever the
// exchange has actually resolved, manage the orders already working (fill /
// cancel on TTL / cancel on a vanished edge), then read calibrator.json and
// kxbtc15m-calibrator.json (threshold vs KXBTC15M directional) and journal
// one decision row for every contract they offer. Every row lands in
// kalshi-bot-decisions.jsonl, passes included: a tick that bids nothing still
// has to say why.
//
// A bid is an ORDER, not a position (rung 6). It rests until something
// observable fills it, its TTL runs out, or its edge goes — and while it rests
// its remainder is counted as exposure at the limit price, the same PR #44
// rule the live execution ledger uses.
//
// `gate` scores that ledger against criteria preregistered and sealed BEFORE
// the record was read (rung 2). It only reads and reports; acting on the
// verdict is a later rung's job.
//
// Since issue #167 the gate also re-evaluates ITSELF, on every paper tick,
// through the very same writer (publish_gate_verdict) — the verdict that
// decides paper->live must not depend on someone remembering to run a CLI
// command. The `gate` subcommand is unchanged and still writes the same file;
// what changed is that it is no longer the only thing that does. A LIVE tick
// deliberately does NOT re-evaluate: refreshing the verdict that admits it to
// live is self-authorisation, whatever the arithmetic says.
//
// Paper bids pause when the *current generation's* drawdown exceeds the
// sealed cap (`DRAWDOWN_CAP`), then auto-rotate the live ledger into the next
// KeepAllGenerations slot so a fresh generation can keep learning. Lifetime
// gate FAIL alone does not deadlock paper — live admission still requires a
// full-record PASS via KalshiBotLive::permit.
//
// Rung 4 (the observable loop) adds three subcommands and one file:
//   `kalshi bot stop`   — throws the kill switch (writes kalshi-bot-stop.json)
//   `kalshi bot resume` — clears it (removes that file)
//   `kalshi bot status` — prints exactly what the GUI BOT chip shows, from the
//                         same classifiers and the same staleness constant
//                         (KalshiBotRuntime.h). Two renderers, one truth. That
//                         includes the MODE word since issue #155: it was the
//                         literal "paper" here, so a live tick read LIVE in the
//                         window and paper in the shell.
// The loop re-reads the stop file at the top of EVERY tick, before any bid;
// `once` refuses that tick, `run` journals the refusal and exits, so the
// switch halts bidding within one tick either way. The stop short-circuits the
// WHOLE tick, ahead of the settlement and order-lifecycle passes: a stopped bot
// takes no venue action at all, and its orders stay resting and stay counted as
// exposure rather than being quietly released.
//
// Rung 5 adds `--mode live`, and adds NO authority with it. PAPER remains the
// default in every entry point (`once`, `run`, and the launchd plist); live is
// admitted only when KalshiBotLive::permit() finds ALL of the charter's
// carve-out conditions true at once — a human-armed session, bounded in time,
// whose promotion gate reads a fresh PASS, with the kill switch clear — and
// `kalshi bot run --mode live` exits non-zero with the refusal code otherwise.
// When it IS admitted, a bid becomes an intent handed to the EXISTING
// prepare_order/submit_order tools: submit_order re-reads the kill switch, the
// arm, the venue, credentials, the stake cap, the all-in ceiling, the rolling
// hour, the cumulative experiment cap, and the per-contract duplicate guard
// from the immutable draft, and remains the sole trading authority. This file
// re-implements none of those checks and can bypass none of them.
//
// A live tick does NOT run the paper passes. Settlement, the TTL sweep and the
// conditional-mid fill model are the PAPER book's; a live order's lifecycle
// belongs to the venue and to kalshi_live_orders. Applying paper machinery to a
// live order would invent fills, which is why the mode split is structural
// (KalshiBotLive::is_live_row) rather than a convention.
//
// All decision, lifecycle, verdict and live-admission math lives in the pure,
// unit-tested KalshiBotDecision / KalshiBotOrders / KalshiBotGate /
// KalshiBotLive; this file only does I/O, flags, and printing.
//
// Own TU, excluded from unity builds like the other cli command families
// (MSVC front-end capacity; see CommandDispatch.cpp).

#include "cli/KalshiBotCommands.h"

#include "cli/ServeCommand.h"
#include "services/prediction/kalshi/KalshiBotDecision.h"
#include "services/prediction/kalshi/KalshiBotFunnel.h"
#include "services/prediction/kalshi/KalshiBotGate.h"
#include "services/prediction/kalshi/KalshiBotLive.h"
#include "services/prediction/kalshi/KalshiBotOrders.h"
#include "services/prediction/kalshi/KalshiBotRuntime.h"
#include "services/prediction/kalshi/KalshiEdgeLessons.h"
#include "services/prediction/kalshi/KalshiEvidenceEngine.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimeZone>
#include <QVector>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <unistd.h>

namespace openmarketterminal::cli {

// Shared CLI flag helper (defined in CommandDispatch.cpp; redeclared here to
// avoid pulling EdgeJournalShared.h's heavier includes into this small TU).
bool take_string_option(QStringList& args, const QString& flag, QString& out);
bool take_bool_flag(QStringList& args, const QString& flag);

// The live path's only two doors into the rest of the CLI, both defined in
// CommandDispatch.cpp (declared here for the same reason as the flag helpers).
// `kalshi_bot_call_tool` runs an EXISTING headless tool and returns its data
// object; `kalshi_bot_live_status` is the `kalshi auto live status` object the
// GUI BOT panel already renders its caps from. Neither is new authority: this
// file cannot arm anything, and every cap it quotes is re-checked at submit.
int kalshi_bot_call_tool(const GlobalOpts& opts, const QString& tool, const QJsonObject& args,
                         QJsonObject& out);
QJsonObject kalshi_bot_live_status();

namespace {

// Rung 4's runtime header contributes free functions and constants
// (kalshi_bot_loop_status, kKalshiBotStopFileName, …) alongside the classes,
// so the whole namespace is pulled in rather than named type by type.
using namespace services::prediction::kalshi_ns;

constexpr auto kLedgerFile = kKalshiBotDecisionLedgerFile;
constexpr auto kDecisionEvent = "kalshi_bot_decision";
constexpr auto kSettlementEvent = "kalshi_bot_paper_settlement";

QString bot_ledger_path() { return kalshi_evidence_path(QString::fromLatin1(kLedgerFile)); }
QString bot_stop_path() { return kalshi_evidence_path(QString::fromLatin1(kKalshiBotStopFileName)); }

/// Row filter for one ledger event type.
std::function<bool(const QJsonObject&)> is_event(const char* name) {
    return [name](const QJsonObject& row) {
        return row.value(QStringLiteral("event")).toString() == QLatin1String(name);
    };
}

/// Appends one row to the bot's decision ledger.
///
/// EVERY write to that ledger goes through here so the rotation policy cannot
/// be forgotten at a call site: the ledger is the sealed gate's evidence and
/// the book's only memory of what is resting and what has resolved, so no
/// generation of it may ever be recycled (issue #152).
bool journal_ledger_row(const QString& path, const QJsonObject& row) {
    return KalshiEvidenceEngine::append_jsonl(path, row,
                                              KalshiEvidenceEngine::Rotation::KeepAllGenerations);
}

/// The bot's whole decision record, every generation of it, oldest first.
/// Thin alias for the shared reader so the CLI and the GUI cannot drift apart.
QJsonArray read_ledger(const QString& path,
                       const std::function<bool(const QJsonObject&)>& keep) {
    return kalshi_bot_read_ledger(path, keep);
}

/// Reads a jsonl evidence file that is NOT the bot ledger (the ladder and
/// settlement feeds, which rotate as a window). `keep` filters rows as they are
/// parsed so a large evidence file never has to be held in memory whole.
QJsonArray read_jsonl(const QString& path,
                      const std::function<bool(const QJsonObject&)>& keep) {
    QJsonArray rows;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return rows;
    while (!file.atEnd()) {
        const QJsonDocument document = QJsonDocument::fromJson(file.readLine());
        if (!document.isObject()) continue;
        const QJsonObject row = document.object();
        if (keep(row)) rows.append(row);
    }
    return rows;
}

/// Settlements in the live generation only (base path — not `.1`, `.2`, …).
/// Paper drawdown pause / auto-rotate score this slice; the sealed gate still
/// scores every generation via `read_ledger`.
QJsonArray read_live_generation_settlements(const QString& path) {
    return read_jsonl(path, is_event(kSettlementEvent));
}

/// Archive the live paper ledger into the next KeepAllGenerations slot so a
/// new generation can bid. Never deletes history and never backfills a gap
/// (same rule as size-based rotation in append_jsonl).
bool rotate_paper_generation(const QString& path) {
    if (!QFileInfo::exists(path)) return true;
    const QString target = kalshi_bot_next_generation_path(path);
    if (QFileInfo::exists(target)) return false;
    return QFile::rename(path, target);
}

bool write_json_file(const QString& path, const QJsonObject& object) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return file.commit();
}

/// Publishes the conversion funnel over the record this PAPER tick just
/// replayed (issue #153), atomically, so the file is exactly as fresh as the
/// loop and no reader has to re-read a 38 MB ledger to learn the denominators.
///
/// Called only from the paper tick, and deliberately: a LIVE tick replays no
/// paper book (its orders' lifecycle belongs to the venue), so it has nothing
/// to measure and publishes nothing. The resulting age of the file is what the
/// renderers show — a funnel older than two tick intervals carries its age on
/// every line rather than being quietly presented as current.
void publish_funnel(const QJsonArray& ledger, qint64 now_ms) {
    const KalshiBotFunnel funnel = KalshiBotFunnel::measure(
        ledger, KalshiBotGate::load_params_file(
                    kalshi_evidence_path(QString::fromLatin1(KalshiBotGate::kParamsFile))));
    const QString path = kalshi_evidence_path(QString::fromLatin1(kKalshiBotFunnelFile));
    if (!write_json_file(path, funnel.to_json(now_ms)))
        std::fprintf(stderr, "kalshi bot: cannot write %s — the funnel is not published this tick\n",
                     qUtf8Printable(path));
}

/// One evidence JSON object. An unreadable or unparseable file returns an
/// empty object — the bot never substitutes a previous report for a missing
/// one.
QJsonObject read_evidence_json(const QString& file_name) {
    QFile file(kalshi_evidence_path(file_name));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject();
}

/// Threshold / hourly spot calibrator (calibrator.json).
QJsonObject read_calibrator_report() {
    return read_evidence_json(QStringLiteral("calibrator.json"));
}

/// KXBTC15M directional calibrator (kxbtc15m-calibrator.json).
QJsonObject read_kxbtc15m_calibrator_report() {
    return read_evidence_json(QStringLiteral("kxbtc15m-calibrator.json"));
}

/// Commodities 15m directional calibrator (commodities-15m-calibrator.json).
QJsonObject read_commodities_15m_calibrator_report() {
    return read_evidence_json(QStringLiteral("commodities-15m-calibrator.json"));
}

/// Decide once per family report so trust cannot leak across families: an
/// untrusted 15m scoreboard must not block a trusted threshold bid, and
/// directional families are stripped from the threshold report (each owns
/// its own calibrator).
QJsonArray decide_family_reports(const QJsonObject& threshold_report,
                                 const QJsonObject& kxbtc15m_report,
                                 const QJsonObject& commodities_15m_report,
                                 const QJsonArray& open_positions,
                                 const QJsonArray& settled_positions,
                                 qint64 now_ms,
                                 const KalshiBotDecision::Config& config,
                                 const KalshiBotStopFile& stop,
                                 const KalshiBotDecision::Exposure& exposure) {
    const bool has_threshold =
        threshold_report.value(QStringLiteral("generated_at_ms")).toDouble() > 0.0;
    const bool has_15m =
        kxbtc15m_report.value(QStringLiteral("generated_at_ms")).toDouble() > 0.0;
    const bool has_commodities =
        commodities_15m_report.value(QStringLiteral("generated_at_ms")).toDouble() > 0.0;
    if (!has_threshold && !has_15m && !has_commodities)
        return KalshiBotDecision::decide({}, open_positions, settled_positions, now_ms, config,
                                         stop, exposure);

    QJsonArray rows;
    const auto append_filtered = [&](const QJsonObject& filtered) {
        if (filtered.value(QStringLiteral("generated_at_ms")).toDouble() <= 0.0) return;
        if (filtered.value(QStringLiteral("predictions")).toObject().isEmpty()) return;
        const QJsonArray part = KalshiBotDecision::decide(
            filtered, open_positions, settled_positions, now_ms, config, stop, exposure);
        for (const auto& value : part) rows.append(value);
    };
    append_filtered(
        KalshiBotDecision::filter_predictions_for_family(threshold_report, /*keep_kxbtc15m=*/false));
    append_filtered(
        KalshiBotDecision::filter_predictions_for_family(kxbtc15m_report, /*keep_kxbtc15m=*/true));
    append_filtered(KalshiBotDecision::filter_commodity_15m_predictions(commodities_15m_report));
    if (!rows.isEmpty()) return rows;
    // Source file(s) exist but no family has a live prediction — one honest
    // row from a present source, never a false REPORT_MISSING.
    if (has_commodities)
        return KalshiBotDecision::decide(
            KalshiBotDecision::filter_commodity_15m_predictions(commodities_15m_report),
            open_positions, settled_positions, now_ms, config, stop, exposure);
    if (has_15m)
        return KalshiBotDecision::decide(
            KalshiBotDecision::filter_predictions_for_family(kxbtc15m_report, /*keep_kxbtc15m=*/true),
            open_positions, settled_positions, now_ms, config, stop, exposure);
    return KalshiBotDecision::decide(
        KalshiBotDecision::filter_predictions_for_family(threshold_report, /*keep_kxbtc15m=*/false),
        open_positions, settled_positions, now_ms, config, stop, exposure);
}

/// The published gate verdict (rung 2's kalshi-bot-gate.json), read exactly as
/// the BOT panel reads it. The verdict is NOT re-derived here: one scorer, many
/// readers, so the bot and the screen beside it can never disagree about
/// whether the signal is promoted.
QJsonObject read_gate_verdict() {
    QFile file(kalshi_evidence_path(QString::fromLatin1(KalshiBotGate::kVerdictFile)));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject();
}

/// True when a calibrator report should be refreshed before the next decide.
/// Proactive: kick at 2/3 of max age so LaunchAgent lag does not land on
/// REPORT_STALE. Missing / negative age always needs a refresh.
bool calibrator_needs_refresh(const QJsonObject& report, qint64 now_ms, qint64 max_age_ms) {
    const qint64 generated_ms =
        static_cast<qint64>(report.value(QStringLiteral("generated_at_ms")).toDouble());
    if (generated_ms <= 0) return true;
    const qint64 age_ms = now_ms - generated_ms;
    if (age_ms < 0) return true;
    const qint64 kick_after_ms = (max_age_ms * 2) / 3;
    return age_ms >= kick_after_ms;
}

QString calibrator_python_for(const QString& script_name) {
    // spot_calibrator needs the numpy venv; directional jobs are stdlib.
    if (script_name.contains(QStringLiteral("spot_calibrator"))) {
        const QString venv = QDir::homePath() + QStringLiteral(
            "/Library/Application Support/org.openterminal.OpenTerminal/venv-numpy2/bin/python3");
        if (QFileInfo::exists(venv)) return venv;
    }
    const QString found = QStandardPaths::findExecutable(QStringLiteral("python3"));
    return found.isEmpty() ? QStringLiteral("/usr/bin/python3") : found;
}

QString calibrator_script_abs_path(const QString& script_name) {
    const QString bundled = QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("scripts/kalshi_advise/") + script_name);
    if (QFileInfo::exists(bundled)) return bundled;
    const QString fallback =
        QStringLiteral("/Users/haydarevich/src/Open-Terminal/openmarketterminal-qt/scripts/"
                       "kalshi_advise/") +
        script_name;
    return QFileInfo::exists(fallback) ? fallback : QString();
}

/// Spawn a detached refresher WITHOUT inheriting our stdio.
///
/// QProcess::startDetached's static overload leaves the child attached to our
/// stdout. A calibrator that prints its report then lands in the middle of
/// `--json` output and corrupts it for any consumer -- observed as
/// "JSONDecodeError: Extra data" when a caller piped `--json kalshi bot once`
/// into json.load while a stale calibrator was being refreshed. The child is a
/// background refresher; its output belongs to its own log, never to ours.
bool start_detached_quiet(const QString& program, const QStringList& args,
                          const QString& working_dir = QString()) {
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    if (!working_dir.isEmpty()) process.setWorkingDirectory(working_dir);
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    return process.startDetached();
}

/// Kick one calibrator when its report is missing or aging toward stale.
/// Non-blocking: prefers `launchctl kickstart` of the steady-state LaunchAgent,
/// falls back to a detached `python once`. Throttled so a stuck publisher
/// cannot fork-bomb the tick loop.
void maybe_kick_calibrator(const QJsonObject& report, qint64 now_ms, qint64 max_age_ms,
                           const QString& script_name, const QString& launch_label) {
    if (!calibrator_needs_refresh(report, now_ms, max_age_ms)) return;

    static QHash<QString, qint64> last_kick_ms;
    constexpr qint64 kMinKickGapMs = 45'000;
    const qint64 prev = last_kick_ms.value(script_name, 0);
    if (prev > 0 && now_ms - prev < kMinKickGapMs) return;
    last_kick_ms.insert(script_name, now_ms);

    const qint64 generated_ms =
        static_cast<qint64>(report.value(QStringLiteral("generated_at_ms")).toDouble());
    const qint64 age_ms = generated_ms > 0 ? now_ms - generated_ms : -1;

    bool kicked = false;
    if (!launch_label.isEmpty()) {
        const QString target =
            QStringLiteral("gui/%1/%2").arg(static_cast<unsigned long>(::getuid())).arg(launch_label);
        kicked = start_detached_quiet(QStringLiteral("/bin/launchctl"),
                                      {QStringLiteral("kickstart"), QStringLiteral("-k"), target});
        if (kicked) {
            std::fprintf(stderr,
                         "kalshi bot: auto-refresh kickstart %s (report_age_ms=%lld)\n",
                         qUtf8Printable(launch_label), static_cast<long long>(age_ms));
            return;
        }
    }

    const QString path = calibrator_script_abs_path(script_name);
    if (path.isEmpty()) return;
    kicked = start_detached_quiet(calibrator_python_for(script_name),
                                  {path, QStringLiteral("once")},
                                  QFileInfo(path).absolutePath());
    if (kicked) {
        std::fprintf(stderr,
                     "kalshi bot: auto-refresh once %s (report_age_ms=%lld)\n",
                     qUtf8Printable(script_name), static_cast<long long>(age_ms));
    }
}

/// Refresh every family the bot reads. LaunchAgents are the steady-state loop;
/// this is the unattended safety net (overnight / App Nap / missed interval).
void refresh_stale_calibrators(const QJsonObject& threshold_report,
                               const QJsonObject& kxbtc15m_report,
                               const QJsonObject& commodities_15m_report, qint64 now_ms,
                               qint64 max_age_ms) {
    maybe_kick_calibrator(threshold_report, now_ms, max_age_ms,
                          QStringLiteral("spot_calibrator.py"),
                          QStringLiteral("org.openterminal.spot-calibrator"));
    maybe_kick_calibrator(kxbtc15m_report, now_ms, max_age_ms,
                          QStringLiteral("kxbtc15m_calibrator.py"),
                          QStringLiteral("org.openterminal.kxbtc15m-calibrator"));
    maybe_kick_calibrator(commodities_15m_report, now_ms, max_age_ms,
                          QStringLiteral("commodities_15m_calibrator.py"),
                          QStringLiteral("org.openterminal.commodities-15m-calibrator"));
}

/// Evaluates the sealed promotion gate and publishes the verdict — the ONE
/// writer of kalshi-bot-gate.json (issue #167).
///
/// `kalshi bot gate` and every paper tick call this same function, so an
/// automated evaluation is the same computation, over the same inputs, written
/// through the same path as a manual one. There is deliberately no second
/// implementation and no "automated" flag: a refusal (TAMPERED,
/// NOT_PREREGISTERED, RECORD_INCOMPLETE) is therefore identical whether a human
/// or the loop asked, which is the property that makes automation safe here.
///
/// `record_rows` are the ledger's decision AND settlement rows; `evaluate()`
/// does its own event filtering, so the tick hands the array it already read
/// rather than paying a second full-record read. Passing an empty array reads
/// the record from disk (the CLI command's path).
///
/// A failed write is reported and NOT swallowed: the previous verdict stays on
/// disk, and because the surfaces now render a verdict's age, that stale file
/// shows as STALE rather than as current.
QJsonObject publish_gate_verdict(qint64 now_ms, const QJsonArray& record_rows) {
    const QString ledger_path = bot_ledger_path();
    // What the gate is allowed to know about the record's completeness, read
    // from disk rather than inferred from the rows: a truncated record is
    // indistinguishable from a shorter one once it is just an array of rows.
    KalshiBotGate::RecordIntegrity integrity;
    integrity.missing_generations = kalshi_bot_ledger_record(ledger_path).missing;
    integrity.oldest_row_ts_ms = kalshi_bot_oldest_row_ts_ms(ledger_path);
    integrity.published_first_settled_ts_ms = KalshiBotGate::published_anchor_ms(read_gate_verdict());

    const QJsonValue params = KalshiBotGate::load_params_file(
        kalshi_evidence_path(QString::fromLatin1(KalshiBotGate::kParamsFile)));
    const QJsonObject out =
        record_rows.isEmpty()
            ? KalshiBotGate::evaluate(params, read_ledger(ledger_path, is_event(kDecisionEvent)),
                                      read_ledger(ledger_path, is_event(kSettlementEvent)), now_ms,
                                      integrity)
            // The same array twice: `evaluate()` filters decision rows out of
            // the first and settlement rows out of the second itself.
            : KalshiBotGate::evaluate(params, record_rows, record_rows, now_ms, integrity);

    const QString path = kalshi_evidence_path(QString::fromLatin1(KalshiBotGate::kVerdictFile));
    if (!write_json_file(path, out))
        std::fprintf(stderr, "kalshi bot: cannot write %s — the verdict on disk is the previous "
                             "one and its age says so\n", qUtf8Printable(path));
    return out;
}

/// Re-read from disk on every call: the arm, the gate verdict and the kill
/// switch are all revocable, so a `run` loop asks again each tick rather than
/// latching an answer at startup.
KalshiBotLive::Permission live_permission(qint64 now_ms) {
    return KalshiBotLive::permit(kalshi_bot_live_status(), read_gate_verdict(),
                                 kalshi_bot_read_stop_file(bot_stop_path()), now_ms);
}

struct TickResult {
    int bids = 0;
    int passes = 0;
    /// Live bids the submit path refused. Counted separately from `bids`: an
    /// order the venue never took is not a bid the bot placed.
    int submit_rejected = 0;
    bool mode_live = false;
    int settled = 0;
    int still_open = 0;
    int filled = 0;
    int canceled = 0;
    int unconfirmed_cancels = 0;
    int resting = 0;
    /// Paper cashout sells closed this tick (early_exit settlements).
    int cashouts = 0;
    double settled_pnl = 0.0;
    double exposure_usd = 0.0;
    double resting_usd = 0.0;
    double session_opened_usd = 0.0;
    QString state;      ///< "ok", or the refusal reason code
    bool signal_trusted = false;
    bool stopped = false;
};

/// CUT_EDGE_REVERSED hysteresis across paper ticks in this process. Cleared on
/// generation rotate so a fresh generation does not inherit stale streaks.
KalshiBotDecision::CashoutStreak& paper_cashout_streak() {
    static KalshiBotDecision::CashoutStreak streak;
    return streak;
}

/// Per-position YES-mid samples while a paper fill is open. Attached onto
/// settlement/cashout rows for gamma honesty; cleared on generation rotate.
KalshiBotDecision::MidPathStore& paper_mid_path() {
    static KalshiBotDecision::MidPathStore store;
    return store;
}

/// The paper venue's canceller. The paper book IS the decision ledger, so a
/// cancel is effective exactly when its row lands — there is no separate venue
/// to answer, and inventing a refusal it never gave would be a lie. If the
/// append below fails, no cancel row exists, the order is still resting on the
/// next replay, and it is retried: unconfirmed cancels are handled by the same
/// path either way. A live rung injects a canceller that asks the exchange.
bool paper_cancel(const QJsonObject&, const QString&) { return true; }

/// One tick. `stop` was read from disk immediately before this call, so the
/// kill switch is re-read every tick rather than latched at startup.
/// `session_opened_usd` is this run's committed all-in so far; it is carried
/// through every exit path, including the stopped one.
TickResult run_tick(const KalshiBotDecision::Config& config, qint64 now_ms,
                    const KalshiBotStopFile& stop, double session_opened_usd,
                    bool auto_refresh_calibrators = true) {
    const QString ledger_path = bot_ledger_path();

    // The lifetime session budget (issue #125) is a LIVE bounded-run safety;
    // the perpetual paper loop must not be bricked by it once cumulative paper
    // all-in reaches the cap. Paper is bounded by max_open_exposure_usd (open
    // risk) instead. Live keeps the budget (run_live_tick, default true).
    KalshiBotDecision::Config paper_config = config;
    paper_config.enforce_session_budget = false;

    // The switch short-circuits the whole tick: no settlement pass, no order
    // lifecycle pass, no report read, no bid — only the journaled refusal.
    // KalshiBotDecision::decide() checks it again (it is the layer that owns
    // the single path to a bid); this early return is what makes the refusal
    // total. Deliberately ahead of reconcile(): a cancel is a venue action,
    // and a stopped bot takes none. Its resting orders therefore stay resting
    // and stay counted as exposure — the conservative outcome. The ledger is
    // still replayed below, for reporting only: printing zeros for a book the
    // bot demonstrably still has out would be the dishonest saving.
    if (stop.engaged) {
        TickResult stopped;
        stopped.stopped = true;
        stopped.state = QString::fromLatin1(KalshiBotDecision::kBotStopped);
        // Carried, not reset: a stop/resume must not hand the run a fresh
        // session budget (issue #125's "a re-arm resets lifetime exposure").
        stopped.session_opened_usd = session_opened_usd;
        const QJsonArray rows = KalshiBotDecision::decide({}, {}, {}, now_ms, paper_config, stop);
        for (const auto& value : rows) {
            journal_ledger_row(ledger_path, value.toObject());
            ++stopped.passes;
        }
        // The book is still out there while the bot is stopped; report it
        // rather than printing zeros the ledger does not support. Shared with
        // the main path below (kalshi_bot_is_replay_event) so the two reads
        // cannot drift the way they did before (issue #189's void kind reached
        // one and not the other).
        const QJsonArray record = read_ledger(ledger_path, kalshi_bot_is_replay_event);
        const KalshiBotOrders::Book book = KalshiBotOrders::replay(record);
        stopped.still_open = static_cast<int>(book.positions.size());
        stopped.resting = static_cast<int>(book.resting.size());
        stopped.exposure_usd = book.exposure_usd;
        stopped.resting_usd = book.resting_usd;
        // The record is no less real for the loop being stopped, and its
        // denominators do not change when bidding halts: a stopped tick that
        // let the funnel go stale would make the file's age look like the
        // record's age.
        publish_funnel(record, now_ms);
        // And the verdict, for the same reason (issue #167): the paper record a
        // stopped tick refuses to add to is the record the gate scores, so its
        // verdict is exactly as valid — and exactly as current — as it was one
        // tick ago. Letting it age here would make the operator's screen show a
        // stale verdict the moment the kill switch went on.
        publish_gate_verdict(now_ms, record);
        return stopped;
    }

    QJsonArray ledger = read_ledger(ledger_path, kalshi_bot_is_replay_event);
    const auto journal = [&ledger, &ledger_path](const QJsonObject& row) {
        journal_ledger_row(ledger_path, row);
        ledger.append(row);
    };
    KalshiBotOrders::Book book = KalshiBotOrders::replay(ledger);
    TickResult result;

    // --- the exchange's real results, for everything the bot has working ---
    QSet<QString> wanted;
    for (const QJsonArray* group : {&book.positions, &book.resting})
        for (const auto& value : *group)
            wanted.insert(value.toObject().value(QStringLiteral("ticker")).toString());
    const auto keep_wanted = [&wanted](const QString& key) {
        return [&wanted, key](const QJsonObject& row) {
            return wanted.contains(row.value(key).toString());
        };
    };
    const QJsonArray settlements =
        wanted.isEmpty() ? QJsonArray()
                         : KalshiBotDecision::normalize_settlements(
                               read_jsonl(kalshi_evidence_path(
                                              QStringLiteral("kalshi-account-settlements.jsonl")),
                                          keep_wanted(QStringLiteral("market_id"))),
                               read_jsonl(kalshi_evidence_path(QStringLiteral("kalshi-settlements.jsonl")),
                                          keep_wanted(QStringLiteral("kalshi_market_id"))));

    // --- calibrator reports (needed for mid-at-settle + reconcile) ---------
    QJsonObject threshold_report = read_calibrator_report();
    QJsonObject kxbtc15m_report = read_kxbtc15m_calibrator_report();
    QJsonObject commodities_15m_report = read_commodities_15m_calibrator_report();
    // Unattended refresh before decide/settle: LaunchAgents are primary; this
    // kicks when a report ages toward max_report_age (avoids REPORT_STALE).
    if (auto_refresh_calibrators) {
        refresh_stale_calibrators(threshold_report, kxbtc15m_report, commodities_15m_report,
                                  now_ms, paper_config.max_report_age_ms);
        // Re-read once after kicks so a fast publisher can still serve this tick.
        threshold_report = read_calibrator_report();
        kxbtc15m_report = read_kxbtc15m_calibrator_report();
        commodities_15m_report = read_commodities_15m_calibrator_report();
    }
    const QJsonObject report = KalshiBotDecision::merge_family_reports(
        threshold_report, kxbtc15m_report, now_ms, paper_config, commodities_15m_report);
    QHash<QString, double> market_mid_by_ticker;
    const QJsonObject predictions = report.value(QStringLiteral("predictions")).toObject();
    for (auto it = predictions.constBegin(); it != predictions.constEnd(); ++it) {
        const double mid = it.value().toObject().value(QStringLiteral("market_yes_mid")).toDouble();
        if (mid > 0.0 && mid < 1.0) market_mid_by_ticker.insert(it.key(), mid);
    }

    // --- settle filled positions first, against those results only ---------
    // Sample mid path before close so the settle-tick mid is on the path.
    // Bid snapshot + optional YES mid + mid_path go on the settlement row.
    if (!book.positions.isEmpty()) {
        KalshiBotDecision::sample_mid_path(
            book.positions, predictions, now_ms, &paper_mid_path());
        const QJsonArray fresh = KalshiBotDecision::settle_paper(
            book.positions, settlements, now_ms, market_mid_by_ticker);
        for (const auto& value : fresh) {
            QJsonObject row = value.toObject();
            KalshiBotDecision::attach_mid_path(row, &paper_mid_path());
            journal(row);
            result.settled_pnl += row.value(QStringLiteral("realized_pnl")).toDouble();
        }
        result.settled = static_cast<int>(fresh.size());
    }

    // --- paper cashout (sell-to-close) before resting reconcile -------------
    // Exchange settlements already won above. Cashout sells at the observed
    // held-side bid only; hold-to-settle remains when the evaluator says HOLD.
    book = KalshiBotOrders::replay(ledger);
    if (paper_config.enable_paper_cashout && !book.positions.isEmpty()) {
        KalshiBotDecision::sample_mid_path(
            book.positions, predictions, now_ms, &paper_mid_path());
        const QJsonArray cashout_rows = KalshiBotDecision::paper_cashout(
            book.positions, report, now_ms, paper_config, &paper_cashout_streak());
        for (const auto& value : cashout_rows) {
            QJsonObject row = value.toObject();
            if (row.value(QStringLiteral("event")).toString() ==
                QLatin1String("kalshi_bot_paper_settlement")) {
                KalshiBotDecision::attach_mid_path(row, &paper_mid_path());
                journal(row);
                ++result.cashouts;
                ++result.settled;
                result.settled_pnl += row.value(QStringLiteral("realized_pnl")).toDouble();
            } else {
                journal(row);
                if (row.value(QStringLiteral("action")).toString() ==
                    QStringLiteral("pass")) {
                    ++result.passes;
                }
            }
        }
        if (result.cashouts > 0) book = KalshiBotOrders::replay(ledger);
    }

    // --- then manage what is still working (rung 6) ------------------------
    const QJsonArray lifecycle =
        KalshiBotOrders::reconcile(book, report, settlements, now_ms, paper_config, paper_cancel);
    for (const auto& value : lifecycle) {
        const QJsonObject row = value.toObject();
        journal(row);
        const QString reason = row.value(QStringLiteral("reason_code")).toString();
        if (row.value(QStringLiteral("action")).toString() == QStringLiteral("fill"))
            ++result.filled;
        else if (reason == QLatin1String(KalshiBotOrders::kUnconfirmedCancel))
            ++result.unconfirmed_cancels;
        else
            ++result.canceled;
    }

    // --- current-gen drawdown: pause + auto-rotate before deciding ---------
    // Score settlements in the LIVE file only. Lifetime gate FAIL must not
    // deadlock paper (that used to require a manual ledger reset); live still
    // needs a full-record PASS. When the sealed cap is breached on this
    // generation, journal DRAWDOWN_CAP, archive the live file, and continue
    // deciding on a fresh generation so learning does not need a human reset.
    book = KalshiBotOrders::replay(ledger);
    const QJsonObject gate_now = publish_gate_verdict(now_ms, ledger);
    const KalshiBotDecision::PaperGenerationRisk current_gen =
        KalshiBotDecision::score_paper_generation(read_live_generation_settlements(ledger_path));
    const KalshiBotDecision::PaperBidPause pause =
        KalshiBotDecision::paper_bid_pause(gate_now, current_gen);
    if (pause.paused) {
        const QJsonObject pause_row{
            {QStringLiteral("event"), QStringLiteral("kalshi_bot_decision")},
            {QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
            {QStringLiteral("ts"),
             QDateTime::fromMSecsSinceEpoch(now_ms, QTimeZone::UTC).toString(Qt::ISODateWithMs)},
            {QStringLiteral("mode"), QStringLiteral("paper")},
            {QStringLiteral("live_eligible"), false},
            {QStringLiteral("ticker"), QString()},
            {QStringLiteral("action"), QStringLiteral("pass")},
            {QStringLiteral("reason_code"), pause.reason_code},
            {QStringLiteral("pause_detail"), pause.detail},
            {QStringLiteral("current_gen_settled_bids"), current_gen.settled_bids},
            {QStringLiteral("current_gen_max_drawdown_usd"), current_gen.max_drawdown_usd},
            {QStringLiteral("current_gen_net_pnl_usd"), current_gen.net_pnl_usd},
            {QStringLiteral("signal_trusted"), false},
            {QStringLiteral("gate_verdict"),
             gate_now.value(QStringLiteral("verdict")).toString()}};
        journal(pause_row);
        ++result.passes;

        const QString archive_target = kalshi_bot_next_generation_path(ledger_path);
        const bool rotated = pause.should_rotate && rotate_paper_generation(ledger_path);
        if (rotated) {
            const int archived_gen =
                kalshi_bot_generation_index(QFileInfo(ledger_path).fileName(),
                                            QFileInfo(archive_target).fileName());
            const QJsonObject rotate_row{
                {QStringLiteral("event"), QStringLiteral("kalshi_bot_decision")},
                {QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
                {QStringLiteral("ts"),
                 QDateTime::fromMSecsSinceEpoch(now_ms, QTimeZone::UTC)
                     .toString(Qt::ISODateWithMs)},
                {QStringLiteral("mode"), QStringLiteral("paper")},
                {QStringLiteral("live_eligible"), false},
                {QStringLiteral("ticker"), QString()},
                {QStringLiteral("action"), QStringLiteral("pass")},
                {QStringLiteral("reason_code"),
                 QString::fromLatin1(KalshiBotDecision::kPaperGenerationRotated)},
                {QStringLiteral("pause_detail"),
                 QStringLiteral("archived current paper generation to .%1 — bidding resumes "
                                "on a fresh generation; sealed gate still scores full record")
                     .arg(archived_gen)},
                {QStringLiteral("archived_generation"), archived_gen},
                {QStringLiteral("signal_trusted"), false},
                {QStringLiteral("gate_verdict"),
                 gate_now.value(QStringLiteral("verdict")).toString()}};
            journal_ledger_row(ledger_path, rotate_row);
            ledger = read_ledger(ledger_path, kalshi_bot_is_replay_event);
            book = KalshiBotOrders::replay(ledger);
            paper_cashout_streak().clear();
            paper_mid_path().clear();
            result.state = QString::fromLatin1(KalshiBotDecision::kPaperGenerationRotated);
            // Fall through to decide on the fresh generation.
        } else {
            // Rotate failed (target slot occupied) — hard-pause this tick only.
            result.state = pause.reason_code;
            result.signal_trusted = false;
            result.still_open = static_cast<int>(book.positions.size());
            result.resting = static_cast<int>(book.resting.size());
            result.exposure_usd = book.exposure_usd;
            result.resting_usd = book.resting_usd;
            result.session_opened_usd = session_opened_usd;
            publish_funnel(ledger, now_ms);
            return result;
        }
    }

    // --- only then decide, against the book as it now stands ---------------
    KalshiBotDecision::Exposure exposure;
    exposure.at_risk_usd = book.exposure_usd;
    exposure.session_opened_usd = session_opened_usd;
    exposure.resting = book.resting;
    exposure.requoted = KalshiBotOrders::requotable(lifecycle);

    // `book.settled`, not the settlement events alone: a quote that rested
    // through its market's resolution settles into no position and leaves no
    // settlement row, and without its CANCELED_MARKET_SETTLED the contract
    // would be quoted again on the next tick.
    // Per-family decide: each calibrator owns its trust. Untrusted families
    // still journal SIGNAL_UNTRUSTED and never bid.
    const QJsonArray rows = decide_family_reports(
        threshold_report, kxbtc15m_report, commodities_15m_report, book.positions, book.settled,
        now_ms, paper_config, stop, exposure);
    for (const auto& value : rows) {
        const QJsonObject row = value.toObject();
        journal(row);
        if (row.value(QStringLiteral("action")).toString() == QStringLiteral("bid")) {
            ++result.bids;
            session_opened_usd += row.value(QStringLiteral("all_in_usd")).toDouble();
        } else {
            ++result.passes;
        }
    }
    // The decision math's own predicate, not a second reading of the flag: a
    // tick that passed every contract SIGNAL_UNTRUSTED must never print TRUSTED.
    // Families may be trusted independently.
    result.signal_trusted = KalshiBotDecision::signal_trusted(threshold_report) ||
                            KalshiBotDecision::signal_trusted(kxbtc15m_report) ||
                            KalshiBotDecision::signal_trusted(commodities_15m_report);
    result.state = QStringLiteral("ok");
    if (rows.size() == 1) {
        const QString reason = rows.first().toObject().value(QStringLiteral("reason_code")).toString();
        if (reason == QLatin1String(KalshiBotDecision::kReportMissing) ||
            reason == QLatin1String(KalshiBotDecision::kReportStale))
            result.state = reason;
    }

    book = KalshiBotOrders::replay(ledger);
    result.still_open = static_cast<int>(book.positions.size());
    result.resting = static_cast<int>(book.resting.size());
    result.exposure_usd = book.exposure_usd;
    result.resting_usd = book.resting_usd;
    result.session_opened_usd = session_opened_usd;
    // Over `ledger`, which now holds the whole record INCLUDING every row this
    // tick just journaled — the same rows the book above was replayed from, so
    // the funnel and the exposure can never be describing different records.
    //
    // (`journal` appends to `ledger` without checking the disk write, so a row
    // that failed to land is still measured here. That is the funnel's existing
    // exposure and the verdict below now shares it; the alternative — measuring
    // a record the tick knows it just added to — would be the worse lie, and
    // KeepAllGenerations makes a failed append loud rather than silent.)
    publish_funnel(ledger, now_ms);
    // Re-score after any new bids/passes so the published verdict matches the
    // record including this tick (gate was already scored once for the pause
    // check above, before decide).
    publish_gate_verdict(now_ms, ledger);
    return result;
}

/// One LIVE tick (ladder rung 5).
///
/// Deliberately short, and deliberately NOT a variant of run_tick(): the paper
/// settlement pass, the TTL sweep and the conditional-mid fill model are the
/// paper book's machinery and would fabricate a live order's lifecycle. A live
/// tick reads the report, decides, and hands each bid to the existing
/// prepare_order/submit_order pair. Everything after that — whether the order
/// is allowed, sized, priced, counted against the hour and against the
/// experiment cap, and whether it reaches the exchange at all — is
/// submit_order's, re-checked from the immutable draft.
TickResult run_live_tick(const GlobalOpts& opts, const KalshiBotDecision::Config& config,
                         qint64 now_ms, const KalshiBotStopFile& stop,
                         const KalshiBotLive::Permission& permission,
                         double session_opened_usd, bool auto_refresh_calibrators = true) {
    const QString ledger_path = bot_ledger_path();
    const auto journal = [&ledger_path](const QJsonObject& row) {
        journal_ledger_row(ledger_path, row);
    };
    TickResult result;
    result.mode_live = true;
    result.session_opened_usd = session_opened_usd;

    // The kill switch short-circuits the whole live tick before the report is
    // read and before any order is built. decide() checks it again — it owns
    // the single path to a bid — and this early return is what makes the live
    // refusal total: no prepare, no submit, no venue contact of any kind.
    if (stop.engaged) {
        result.stopped = true;
        result.state = QString::fromLatin1(KalshiBotDecision::kBotStopped);
        for (const auto& value : KalshiBotDecision::decide({}, {}, {}, now_ms, config, stop)) {
            journal(KalshiBotLive::live_row(value.toObject(), permission));
            ++result.passes;
        }
        return result;
    }

    QJsonObject threshold_report = read_calibrator_report();
    QJsonObject kxbtc15m_report = read_kxbtc15m_calibrator_report();
    QJsonObject commodities_15m_report = read_commodities_15m_calibrator_report();
    if (auto_refresh_calibrators) {
        refresh_stale_calibrators(threshold_report, kxbtc15m_report, commodities_15m_report, now_ms,
                                  config.max_report_age_ms);
        threshold_report = read_calibrator_report();
        kxbtc15m_report = read_kxbtc15m_calibrator_report();
        commodities_15m_report = read_commodities_15m_calibrator_report();
    }
    // No book of any kind is replayed here. A live order's fills and lifecycle
    // live at the venue, and the paper model would invent them; and the bot's
    // own ledger is not the authority on what the venue holds. One bot order
    // per contract is enforced where the authority is — submit_order's
    // per-contract duplicate guard, against the immutable drafts (#141). A
    // re-bid therefore reaches submit_order and is refused there, before the
    // adapter, and the refusal is journaled like any other.
    const QJsonArray rows =
        decide_family_reports(threshold_report, kxbtc15m_report, commodities_15m_report, {}, {},
                              now_ms, config, stop, {});
    for (const auto& value : rows) {
        const QJsonObject row = value.toObject();
        if (row.value(QStringLiteral("action")).toString() != QStringLiteral("bid")) {
            // A live tick's passes are live decisions and are journaled as
            // such: a tick that bid nothing still has to say why.
            journal(KalshiBotLive::live_row(row, permission));
            ++result.passes;
            continue;
        }

        QJsonObject submission;
        const QJsonObject intent = KalshiBotLive::live_intent(row, permission);
        if (intent.isEmpty()) {
            submission = QJsonObject{
                {QStringLiteral("status"), QStringLiteral("rejected")},
                {QStringLiteral("reason"),
                 QStringLiteral("the bid row carries no usable contract, price, size, or fee — no "
                                "order was built")}};
        } else {
            QJsonObject prepared;
            const int rc = kalshi_bot_call_tool(opts, QStringLiteral("prepare_order"), intent,
                                                prepared);
            const QString draft_id = prepared.value(QStringLiteral("draft_id")).toString();
            if (rc != 0 || draft_id.isEmpty() ||
                prepared.value(QStringLiteral("status")).toString() != QStringLiteral("prepared")) {
                submission = QJsonObject{
                    {QStringLiteral("status"), QStringLiteral("rejected")},
                    {QStringLiteral("reason"),
                     prepared.value(QStringLiteral("reason"))
                         .toString(QStringLiteral("prepare_order did not return a usable draft"))}};
            } else {
                // The one call that can reach the exchange, and the only one:
                // every gate in the carve-out is re-run inside it.
                kalshi_bot_call_tool(opts, QStringLiteral("submit_order"),
                                     QJsonObject{{QStringLiteral("draft_id"), draft_id},
                                                 {QStringLiteral("mode"), QStringLiteral("live")}},
                                     submission);
                submission.insert(QStringLiteral("draft_id"), draft_id);
            }
        }

        const QJsonObject live = KalshiBotLive::live_row(row, submission, permission);
        journal(live);
        if (live.value(QStringLiteral("reason_code")).toString() ==
            QLatin1String(KalshiBotLive::kLiveSubmitted)) {
            ++result.bids;
            session_opened_usd += row.value(QStringLiteral("all_in_usd")).toDouble();
        } else {
            ++result.submit_rejected;
        }
    }

    // The decision math's own predicate, not a second reading of the flag: a
    // tick that passed every contract SIGNAL_UNTRUSTED must never print TRUSTED.
    result.signal_trusted = KalshiBotDecision::signal_trusted(threshold_report) ||
                            KalshiBotDecision::signal_trusted(kxbtc15m_report) ||
                            KalshiBotDecision::signal_trusted(commodities_15m_report);
    result.state = QStringLiteral("ok");
    if (rows.size() == 1) {
        const QString reason =
            rows.first().toObject().value(QStringLiteral("reason_code")).toString();
        if (reason == QLatin1String(KalshiBotDecision::kReportMissing) ||
            reason == QLatin1String(KalshiBotDecision::kReportStale))
            result.state = reason;
    }
    result.session_opened_usd = session_opened_usd;
    return result;
}

QJsonObject tick_summary(const TickResult& tick, const KalshiBotDecision::Config& config) {
    return QJsonObject{
        // Mode-aware, and never optimistic: `live_eligible` is true only on a
        // tick that was actually admitted to live mode.
        {QStringLiteral("mode"), tick.mode_live ? QStringLiteral("live") : QStringLiteral("paper")},
        {QStringLiteral("live_eligible"), tick.mode_live},
        {QStringLiteral("submit_rejected"), tick.submit_rejected},
        {QStringLiteral("state"), tick.state},
        {QStringLiteral("stopped"), tick.stopped},
        {QStringLiteral("bids"), tick.bids},
        {QStringLiteral("passes"), tick.passes},
        {QStringLiteral("settled"), tick.settled},
        {QStringLiteral("settled_pnl"), tick.settled_pnl},
        {QStringLiteral("cashouts"), tick.cashouts},
        {QStringLiteral("open_positions"), tick.still_open},
        {QStringLiteral("filled"), tick.filled},
        {QStringLiteral("canceled"), tick.canceled},
        {QStringLiteral("unconfirmed_cancels"), tick.unconfirmed_cancels},
        {QStringLiteral("resting_orders"), tick.resting},
        {QStringLiteral("resting_usd"), tick.resting_usd},
        {QStringLiteral("exposure_usd"), tick.exposure_usd},
        {QStringLiteral("session_opened_usd"), tick.session_opened_usd},
        {QStringLiteral("signal_trusted"), tick.signal_trusted},
        {QStringLiteral("edge_threshold"), config.edge_threshold},
        {QStringLiteral("cross_margin_usd"), config.cross_margin_usd},
        {QStringLiteral("rest_premium_usd"), config.rest_premium_usd},
        {QStringLiteral("max_stake_usd"), config.max_stake_usd},
        {QStringLiteral("max_all_in_usd"), config.max_all_in_usd},
        {QStringLiteral("quote_ttl_seconds"), config.quote_ttl_seconds},
        {QStringLiteral("max_open_exposure_usd"), config.max_open_exposure_usd},
        {QStringLiteral("session_budget_usd"), config.session_budget_usd},
        {QStringLiteral("min_runway_seconds"), config.min_runway_seconds},
        {QStringLiteral("max_report_age_ms"), static_cast<double>(config.max_report_age_ms)},
        {QStringLiteral("enable_paper_cashout"), config.enable_paper_cashout},
        {QStringLiteral("ledger"), kalshi_evidence_path(QString::fromLatin1(kLedgerFile))}};
}

void print_tick(const GlobalOpts& opts, const TickResult& tick,
                const KalshiBotDecision::Config& config) {
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(tick_summary(tick, config))
                                .toJson(QJsonDocument::Compact).constData());
        return;
    }
    std::printf("KALSHI BOT · %s · %s\n", tick.mode_live ? "LIVE" : "PAPER",
                qUtf8Printable(tick.state));
    if (tick.stopped)
        std::printf("  kill switch engaged — this tick placed no bid; refusal journaled as %s\n",
                    KalshiBotDecision::kBotStopped);
    if (tick.mode_live) {
        // A live tick reports what the SUBMIT PATH did, not what the decision
        // math wanted: an order the venue refused is not a bid the bot placed.
        std::printf("  live orders accepted %d · refused by submit_order %d · passes %d\n",
                    tick.bids, tick.submit_rejected, tick.passes);
        std::printf("  session committed $%.2f (all-in of accepted orders) · ledger rows carry "
                    "mode=live\n", tick.session_opened_usd);
        std::printf("  ledger %s\n",
                    qUtf8Printable(kalshi_evidence_path(QString::fromLatin1(kLedgerFile))));
        return;
    }
    std::printf("  bids %d · passes %d · open %d\n", tick.bids, tick.passes, tick.still_open);
    // Resting is risk: it is printed on every tick, not only when it changes.
    std::printf("  resting %d ($%.2f at limit) · exposure $%.2f of $%.2f · session $%.2f of $%.2f\n",
                tick.resting, tick.resting_usd, tick.exposure_usd, config.max_open_exposure_usd,
                tick.session_opened_usd, config.session_budget_usd);
    if (tick.filled > 0 || tick.canceled > 0 || tick.unconfirmed_cancels > 0)
        std::printf("  filled %d · canceled %d · UNCONFIRMED cancels %d (still counted as risk)\n",
                    tick.filled, tick.canceled, tick.unconfirmed_cancels);
    if (tick.settled > 0 || tick.cashouts > 0)
        std::printf("  settled %d · cashouts %d · realized $%.2f%s\n", tick.settled, tick.cashouts,
                    tick.settled_pnl,
                    config.enable_paper_cashout ? "" : " · paper-cashout OFF");
    if (tick.state == QLatin1String("ok"))
        std::printf("  signal %s\n",
                    tick.signal_trusted
                        ? "TRUSTED (calibrator beats market baseline)"
                        : "UNTRUSTED — the tick placed NO bid; refusal journaled as "
                          "SIGNAL_UNTRUSTED");
    std::printf("  ledger %s\n",
                qUtf8Printable(kalshi_evidence_path(QString::fromLatin1(kLedgerFile))));
}

void bot_usage() {
    std::fprintf(stderr,
                 "usage: kalshi bot once|run [--mode paper|live] [--edge-threshold X] [--max-stake X]\n"
                 "                          [--max-all-in X] [--min-runway-sec N]\n"
                 "                          [--max-report-age-sec N] [--quote-ttl-sec N]\n"
                 "                          [--max-exposure X] [--session-budget X]\n"
                 "                          [--cross-margin X] [--rest-premium X]\n"
                 "                          [--no-paper-cashout]  (paper: disable sell-to-close)\n"
                 "                          [--no-auto-refresh-calibrators]  (disable stale kicks)\n"
                 "       kalshi bot run [--interval N] [--iterations N]\n"
                 "       kalshi bot gate [--json]\n"
                 "       kalshi bot gate seal '{\"min_settled_bids\":300,\"max_drawdown_usd\":5}'\n"
                 "       kalshi bot stop [--reason \"why\"]   throw the kill switch\n"
                 "       kalshi bot resume                  clear it\n"
                 "       kalshi bot status                  what the GUI BOT chip shows\n"
                 "       kalshi bot scoreboard [--json]     threshold + BTC15m + commodities15m + gate\n"
                 "       kalshi bot calibrate once|report|run [--family all|threshold|kxbtc15m|commodities15m]\n"
                 "                                            [--interval N]\n"
                 "       kalshi bot lessons [--refresh]     what the record teaches (issue #174)\n"
                 "       kalshi bot postmortem [--json] [--post-gate|--since-ms N]\n"
                 "                                          per-settlement bid autopsy (esp. losses)\n"
                 "\n"
                 "`scoreboard` prints every paper calibrator report the bot reads (no GUI).\n"
                 "`calibrate` runs the same Python publishers launchd/kickstart use.\n"
                 "`postmortem` joins every paper settlement back to its bid and classifies\n"
                 "wins/losses (favourite arithmetic, near-close fades, early_exit cashouts).\n"
                 "Always writes HISTORIC (learning/gate) + CURRENT RULES (new-trading scorecard)\n"
                 "when mid_path / fade markers exist. Cockpit PM judges CURRENT, not lifetime.\n"
                 "`--post-gate` requires markers (exit 3 if none); `--since-ms` overrides window.\n"
                 "Paper cashout (default ON): sell-to-close at held-side bid on LOCK_WIN /\n"
                 "CUT_EDGE_REVERSED; hold-to-settle otherwise. `--no-paper-cashout` disables.\n"
                 "Auto-refresh calibrators (default ON): each tick kickstarts LaunchAgents\n"
                 "(or runs `once`) when a report ages past 2/3 of --max-report-age-sec, so\n"
                 "REPORT_STALE does not stick without human intervention. Opt out with\n"
                 "`--no-auto-refresh-calibrators`.\n"
                 "\n"
                 "`lessons` renders %s — the edge autopsy's conclusions, one short line per\n"
                 "question, each with its verdict, its key numbers, the SAMPLE SIZE it was\n"
                 "measured on and the span it covers. --refresh re-runs the analysis first\n"
                 "(minutes; the same publisher the weekly launchd job runs). It is display\n"
                 "only: no lesson changes what the bot does.\n"
                 "\n"
                 "PAPER is the default everywhere. A bid rests until it fills, its TTL\n"
                 "expires, or its edge goes; a resting remainder counts against\n"
                 "--max-exposure at its limit price.\n"
                 "A bid quotes at the mid and waits UNLESS the edge still clears\n"
                 "--cross-margin after paying the real spread and the taker fee, in which\n"
                 "case it crosses and quotes at the ask. Every row says which tier priced\n"
                 "it (quote_style) and shows the arithmetic. A contract whose report\n"
                 "carries no ask always rests.\n"
                 "A RESTING quote is adversely selected — it fills when the market came to\n"
                 "it — so it must clear --edge-threshold PLUS --rest-premium; the crossing\n"
                 "hurdle is unchanged, so a contract can cross where it may not rest\n"
                 "(REST_EDGE_BELOW_PREMIUM).\n"
                 "When the calibrator's own track record says it does not beat the market\n"
                 "(or carries no Brier at all), the tick places NO order in either mode and\n"
                 "journals one SIGNAL_UNTRUSTED pass.\n"
                 "`gate` scores the PAPER ledger against sealed, preregistered criteria and\n"
                 "writes the verdict to %s. It never acts on it.\n"
                 "\n"
                 "--mode live is REFUSED (rc 4) unless every one of these holds right now:\n"
                 "  * a human armed the shared Kalshi live session (`kalshi auto live session`)\n"
                 "    and the GUI live gate is on — nothing here can set either;\n"
                 "  * that session is BOUNDED (arm 1h/6h/12h; a 24/7 arm is refused);\n"
                 "  * kalshi-bot-gate.json reads PASS and is less than an hour old;\n"
                 "  * the kill switch is clear.\n"
                 "A permitted live bid becomes a prepare_order/submit_order call with the\n"
                 "session's own caps; submit_order re-checks all of them and is the only\n"
                 "thing that can reach the exchange.\n",
                 qUtf8Printable(kalshi_evidence_path(QString::fromLatin1(kKalshiEdgeReportFile))),
                 qUtf8Printable(kalshi_evidence_path(QString::fromLatin1(KalshiBotGate::kVerdictFile))));
}

// --- gate (rung 2) --------------------------------------------------------

/// Criterion numbers are printed as they were measured: the settled-bid count
/// is a count, money and Brier are printed to four places. A criterion with
/// nothing to measure prints its note instead of a number.
QString criterion_line(const QJsonObject& criterion) {
    const QString id = criterion.value(QStringLiteral("id")).toString();
    const QJsonValue observed = criterion.value(QStringLiteral("observed"));
    if (!observed.isDouble())
        return QStringLiteral("%1 — %2").arg(id, criterion.value(QStringLiteral("note")).toString());
    const bool counts = id == QLatin1String(KalshiBotGate::kCriterionSettled);
    const auto text = [counts](double value) {
        return counts ? QString::number(static_cast<qint64>(value)) : QString::number(value, 'f', 4);
    };
    return QStringLiteral("%1  observed %2 · required %3 (%4)")
        .arg(id, text(observed.toDouble()),
             text(criterion.value(QStringLiteral("required")).toDouble()),
             criterion.value(QStringLiteral("comparison")).toString());
}

void print_gate(const GlobalOpts& opts, const QJsonObject& out, const QString& evidence_path) {
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        return;
    }
    std::printf("KALSHI BOT GATE · %s\n",
                qUtf8Printable(out.value(QStringLiteral("verdict")).toString()));
    if (!out.value(QStringLiteral("evaluated")).toBool()) {
        // A refusal states why and stops. No criteria, no numbers, no verdict
        // about the paper record — the gate did not judge it.
        std::printf("  %s\n", qUtf8Printable(out.value(QStringLiteral("reason")).toString()));
        std::printf("  params %s\n",
                    qUtf8Printable(kalshi_evidence_path(QString::fromLatin1(KalshiBotGate::kParamsFile))));
        std::printf("  verdict written to %s\n", qUtf8Printable(evidence_path));
        return;
    }
    const QJsonObject ledger = out.value(QStringLiteral("ledger")).toObject();
    std::printf("  gate %s · sealed %lld\n",
                qUtf8Printable(out.value(QStringLiteral("gate_id")).toString()),
                static_cast<long long>(out.value(QStringLiteral("sealed_at_ms")).toDouble()));
    std::printf("  settled %d (%d won / %d lost) · scored %d · unscored %d\n",
                ledger.value(QStringLiteral("settled_bids")).toInt(),
                ledger.value(QStringLiteral("wins")).toInt(),
                ledger.value(QStringLiteral("losses")).toInt(),
                ledger.value(QStringLiteral("scored_contracts")).toInt(),
                ledger.value(QStringLiteral("unscored_contracts")).toInt());
    for (const auto& value : out.value(QStringLiteral("criteria")).toArray()) {
        const QJsonObject criterion = value.toObject();
        std::printf("  %-9s %s\n", criterion.value(QStringLiteral("met")).toBool() ? "[MET]" : "[NOT MET]",
                    qUtf8Printable(criterion_line(criterion)));
    }
    std::printf("  verdict written to %s\n", qUtf8Printable(evidence_path));
}

int gate_seal(const GlobalOpts& opts, const QString& raw_params) {
    const QString path = kalshi_evidence_path(QString::fromLatin1(KalshiBotGate::kParamsFile));
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(raw_params.toUtf8(), &parse_error);
    if (!document.isObject()) {
        std::fprintf(stderr, "kalshi bot gate seal: params must be a JSON object (%s)\n",
                     qUtf8Printable(parse_error.errorString()));
        return 2;
    }
    QString error;
    const QJsonObject record = KalshiBotGate::preregister(path, document.object(),
                                                          QDateTime::currentMSecsSinceEpoch(), &error);
    if (record.isEmpty()) {
        std::fprintf(stderr, "kalshi bot gate seal: %s\n", qUtf8Printable(error));
        return 3;
    }
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(record).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("KALSHI BOT GATE · SEALED\n");
    std::printf("  gate %s\n", qUtf8Printable(record.value(QStringLiteral("gate_id")).toString()));
    std::printf("  params %s\n",
                QJsonDocument(record.value(QStringLiteral("params")).toObject())
                    .toJson(QJsonDocument::Compact).constData());
    std::printf("  seal %s\n", qUtf8Printable(record.value(QStringLiteral("seal_sha256")).toString()));
    std::printf("  written read-only to %s\n", qUtf8Printable(path));
    return 0;
}

int gate_command(const GlobalOpts& opts, QStringList args) {
    if (!args.isEmpty() && args.first().trimmed().toLower() == QStringLiteral("seal")) {
        args.removeFirst();
        if (args.size() != 1) {
            bot_usage();
            return 2;
        }
        return gate_seal(opts, args.first());
    }
    if (!args.isEmpty()) {
        std::fprintf(stderr, "kalshi bot gate: unknown argument '%s'\n", qUtf8Printable(args.first()));
        bot_usage();
        return 2;
    }

    // The manual evaluation, through the SAME writer the loop tick uses (issue
    // #167). It reads the record from disk (no tick has handed it one) and is
    // otherwise identical — same inputs, same refusals, same file.
    const QJsonObject out = publish_gate_verdict(QDateTime::currentMSecsSinceEpoch(), {});
    const QString evidence_path = kalshi_evidence_path(QString::fromLatin1(KalshiBotGate::kVerdictFile));
    print_gate(opts, out, evidence_path);
    // A refusal is not a verdict about the record, so it does not exit 0.
    return out.value(QStringLiteral("evaluated")).toBool() ? 0 : 3;
}

// --- the kill switch and the status the GUI chip mirrors --------------------

/// The loop as `kalshi bot status` reports it: the freshness classification and
/// the mode word, both derived from the SAME ledger tail in one read. Two reads
/// could straddle a tick and describe two different ledgers.
struct BotStatusReading {
    KalshiBotLoopStatus status;
    KalshiBotMode mode;
};

BotStatusReading current_loop_status(qint64 now_ms) {
    // The panel's window (KalshiBotPanelPresentation.h's read_kalshi_bot_ledger_tail),
    // so both surfaces classify over the same rows.
    const QJsonArray rows = kalshi_bot_read_ledger_tail(bot_ledger_path());
    return {kalshi_bot_loop_status(kalshi_bot_newest_ts_ms(rows),
                                   kalshi_bot_read_stop_file(bot_stop_path()), now_ms),
            kalshi_bot_mode(rows)};
}

KalshiBotFunnelFile current_funnel_file() {
    return kalshi_bot_read_funnel_file(
        kalshi_evidence_path(QString::fromLatin1(kKalshiBotFunnelFile)));
}

QJsonObject read_postmortem_summary_file(const QString& name) {
    QFile file(kalshi_evidence_path(name));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}

QJsonObject read_postmortem_summary() {
    return read_postmortem_summary_file(QStringLiteral("kalshi-bot-postmortem-summary.json"));
}

QJsonObject read_postmortem_summary_current() {
    return read_postmortem_summary_file(
        QStringLiteral("kalshi-bot-postmortem-summary-current.json"));
}

/// Compact operator line from one bid-autopsy cohort (display only).
QString postmortem_status_line(const QJsonObject& summary, const QString& label) {
    const QJsonValue settled_v = summary.value(QStringLiteral("settled"));
    // Qt 6.9+ may store whole numbers outside isDouble(); type-exclude non-numerics.
    const bool settled_known =
        settled_v.isDouble() ||
        (settled_v.type() != QJsonValue::Null && settled_v.type() != QJsonValue::Bool &&
         settled_v.type() != QJsonValue::String && settled_v.type() != QJsonValue::Array &&
         settled_v.type() != QJsonValue::Object && settled_v.type() != QJsonValue::Undefined);
    if (summary.isEmpty() || !settled_known)
        return QStringLiteral("%1 UNAVAILABLE · run `kalshi bot postmortem`").arg(label);
    const int wins = summary.value(QStringLiteral("wins")).toInt();
    const int losses = summary.value(QStringLiteral("losses")).toInt();
    const double net = summary.value(QStringLiteral("net_realized_pnl_usd")).toDouble();
    QString top = QStringLiteral("n/a");
    const QJsonArray modes = summary.value(QStringLiteral("loss_primary_modes")).toArray();
    if (!modes.isEmpty()) {
        const QJsonArray pair = modes.first().toArray();
        if (pair.size() >= 1) top = pair.at(0).toString(top);
    }
    const int early = summary.value(QStringLiteral("early_exits")).toInt();
    return QStringLiteral("%1 %2W/%3L · %4 cashouts · net $%5 · top=%6")
        .arg(label)
        .arg(wins)
        .arg(losses)
        .arg(early)
        .arg(QString::number(net, 'f', 2), top);
}

QJsonObject status_summary(const BotStatusReading& reading, qint64 now_ms,
                           const KalshiBotFunnelFile& funnel) {
    const KalshiBotLoopStatus& status = reading.status;
    QJsonObject out{
        {QStringLiteral("state"), status.state},
        {QStringLiteral("color_role"), kalshi_bot_state_color_role(status.state)},
        {QStringLiteral("headline"), status.headline},
        {QStringLiteral("stopped"), status.stop.engaged},
        {QStringLiteral("stale_after_ms"), static_cast<double>(kKalshiBotStaleMs)},
        {QStringLiteral("interval_seconds"), kKalshiBotIntervalSeconds},
        {QStringLiteral("now_ms"), static_cast<double>(now_ms)},
        {QStringLiteral("ledger"), bot_ledger_path()},
        {QStringLiteral("stop_file"), bot_stop_path()}};
    // The mode of the last tick, from the classifier the BOT badge renders
    // (issue #155). It was the literal "paper" here until then, which reported
    // a live tick as paper and an unreadable one as paper too. A record that
    // claims no mode carries NO key, the same absent-is-absent rule
    // `last_decision_age_ms` follows below — never a `paper` nothing stated.
    if (reading.mode.stated()) out.insert(QStringLiteral("mode"), reading.mode.mode);
    // An age the ledger cannot support is absent, not zero.
    if (status.age_ms >= 0)
        out.insert(QStringLiteral("last_decision_age_ms"), static_cast<double>(status.age_ms));
    if (status.stop.engaged) {
        if (status.stop.ts_ms > 0)
            out.insert(QStringLiteral("stopped_at_ms"), static_cast<double>(status.stop.ts_ms));
        if (!status.stop.source.isEmpty())
            out.insert(QStringLiteral("stop_source"), status.stop.source);
        if (!status.stop.reason.isEmpty())
            out.insert(QStringLiteral("stop_reason"), status.stop.reason);
    }

    // --- the conversion funnel (issue #153) ---------------------------------
    // The published file's own object, verbatim: its absent keys stay absent
    // here, so a record with no bid carries NO `fill_rate` rather than a 0.0 a
    // script would read as "nothing ever fills". An unavailable file publishes
    // its reason and NO counts at all.
    out.insert(QStringLiteral("funnel_file"),
               kalshi_evidence_path(QString::fromLatin1(kKalshiBotFunnelFile)));
    out.insert(QStringLiteral("funnel_available"), funnel.available);
    if (funnel.available) {
        out.insert(QStringLiteral("funnel"), funnel.object);
        const auto published_ms =
            static_cast<qint64>(funnel.object.value(QStringLiteral("ts_ms")).toDouble());
        if (published_ms > 0 && now_ms >= published_ms)
            out.insert(QStringLiteral("funnel_age_ms"), static_cast<double>(now_ms - published_ms));
    } else {
        out.insert(QStringLiteral("funnel_unavailable_reason"), funnel.why);
    }
    // The rendered lines, from the ONE formatter the BOT panel renders from —
    // so `--json` and the human text below can never say different things, and
    // neither can the window (criterion 5).
    out.insert(QStringLiteral("funnel_lines"),
               QJsonArray::fromStringList(kalshi_bot_funnel_lines(funnel, now_ms)));
    const QJsonObject historic = read_postmortem_summary();
    const QJsonObject current = read_postmortem_summary_current();
    out.insert(QStringLiteral("postmortem_current_line"),
               postmortem_status_line(current, QStringLiteral("CURRENT RULES")));
    out.insert(QStringLiteral("postmortem_historic_line"),
               postmortem_status_line(historic, QStringLiteral("HISTORIC")));
    // Backward-compatible single line: prefer current-rules scorecard.
    const auto settled_ok = [](const QJsonObject& o) {
        const QJsonValue v = o.value(QStringLiteral("settled"));
        if (o.isEmpty()) return false;
        if (v.isDouble()) return true;
        return v.type() != QJsonValue::Null && v.type() != QJsonValue::Bool &&
               v.type() != QJsonValue::String && v.type() != QJsonValue::Array &&
               v.type() != QJsonValue::Object && v.type() != QJsonValue::Undefined;
    };
    out.insert(QStringLiteral("postmortem_line"),
               settled_ok(current)
                   ? postmortem_status_line(current, QStringLiteral("CURRENT RULES"))
                   : postmortem_status_line(historic, QStringLiteral("HISTORIC")));
    if (!current.isEmpty()) out.insert(QStringLiteral("postmortem_current"), current);
    if (!historic.isEmpty()) {
        out.insert(QStringLiteral("postmortem_historic"), historic);
        out.insert(QStringLiteral("postmortem"), historic); // legacy key = lifetime
    }
    return out;
}

int bot_status_command(const GlobalOpts& opts, QStringList& args) {
    if (!args.isEmpty()) {
        // An option this command does not understand is refused, not ignored:
        // a silently dropped `--json` would make a script read the human text
        // as a status object.
        std::fprintf(stderr, "kalshi bot status: unknown option '%s' (JSON is the global flag: "
                             "`openterminalcli --json kalshi bot status`)\n",
                     qUtf8Printable(args.first()));
        return 2;
    }
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const BotStatusReading reading = current_loop_status(now_ms);
    const KalshiBotLoopStatus& status = reading.status;
    const KalshiBotFunnelFile funnel = current_funnel_file();
    const QJsonObject historic = read_postmortem_summary();
    const QJsonObject current = read_postmortem_summary_current();
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(status_summary(reading, now_ms, funnel))
                                .toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    // Same headline, same state, same colour role, same `[MODE]` prefix the chip
    // paints — this text is produced by kalshi_bot_loop_status() and
    // kalshi_bot_mode_headline(), not re-derived here. A record that claims no
    // mode gets no badge rather than a word the shell invented.
    std::printf("%s\n", qUtf8Printable(kalshi_bot_mode_headline(status.headline, reading.mode)));
    std::printf("  chip %s (%s) · stale after %llds\n", qUtf8Printable(status.state.toUpper()),
                qUtf8Printable(kalshi_bot_state_color_role(status.state)),
                static_cast<long long>(kKalshiBotStaleMs / 1000));
    // The funnel, from the same formatter the BOT panel and `--json` render
    // from. Unavailable prints one refusal line and no numbers at all.
    for (const QString& line : kalshi_bot_funnel_lines(funnel, now_ms))
        std::printf("  %s\n", qUtf8Printable(line));
    // Current-rules first: cockpit/status judge new trading here, not lifetime.
    std::printf("  %s\n", qUtf8Printable(postmortem_status_line(
                              current, QStringLiteral("CURRENT RULES"))));
    std::printf("  %s\n", qUtf8Printable(postmortem_status_line(
                              historic, QStringLiteral("HISTORIC (learn/gate)"))));
    std::printf("  funnel %s\n",
                qUtf8Printable(kalshi_evidence_path(QString::fromLatin1(kKalshiBotFunnelFile))));
    std::printf("  ledger %s\n", qUtf8Printable(bot_ledger_path()));
    std::printf("  stop file %s%s\n", qUtf8Printable(bot_stop_path()),
                status.stop.engaged ? " (PRESENT — kill switch engaged)" : " (absent)");
    return 0;
}

int bot_stop_command(const GlobalOpts& opts, QStringList& args) {
    QString reason;
    take_string_option(args, QStringLiteral("--reason"), reason);
    if (!args.isEmpty()) {
        std::fprintf(stderr, "kalshi bot stop: unknown option '%s'\n", qUtf8Printable(args.first()));
        return 2;
    }
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const QString path = bot_stop_path();
    if (!kalshi_bot_write_stop_file(path, kalshi_bot_stop_payload(QStringLiteral("cli"), reason,
                                                                  now_ms))) {
        // A kill switch that failed to arm must say so loudly: reporting
        // success here would be the worst possible lie in this file.
        std::fprintf(stderr, "kalshi bot stop: FAILED to write %s — the bot is NOT stopped\n",
                     qUtf8Printable(path));
        return 1;
    }
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(QJsonObject{{QStringLiteral("stopped"), true},
                                                      {QStringLiteral("stop_file"), path},
                                                      {QStringLiteral("ts_ms"),
                                                       static_cast<double>(now_ms)}})
                                .toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("KALSHI BOT · KILL SWITCH ENGAGED\n");
    std::printf("  %s\n", qUtf8Printable(path));
    std::printf("  the loop refuses and exits on its next tick (<= %ds); `kalshi bot resume` "
                "clears it\n", kKalshiBotIntervalSeconds);
    return 0;
}

int bot_resume_command(const GlobalOpts& opts, QStringList& args) {
    if (!args.isEmpty()) {
        std::fprintf(stderr, "kalshi bot resume: unknown option '%s'\n",
                     qUtf8Printable(args.first()));
        return 2;
    }
    const QString path = bot_stop_path();
    const bool was_engaged = kalshi_bot_read_stop_file(path).engaged;
    if (!kalshi_bot_clear_stop_file(path)) {
        std::fprintf(stderr, "kalshi bot resume: FAILED to remove %s — the bot is still stopped\n",
                     qUtf8Printable(path));
        return 1;
    }
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(QJsonObject{{QStringLiteral("stopped"), false},
                                                      {QStringLiteral("was_stopped"), was_engaged},
                                                      {QStringLiteral("stop_file"), path}})
                                .toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("KALSHI BOT · KILL SWITCH CLEARED%s\n",
                was_engaged ? "" : " (it was not engaged)");
    std::printf("  the launchd job is not restarted by this command — the operator does that "
                "deliberately (see org.openterminal.kalshi-bot.plist)\n");
    return 0;
}

// --- WHAT THE RECORD TEACHES (issue #174) ----------------------------------
//
// `kalshi bot lessons` renders kalshi-edge-report.json through the SAME
// formatter the BOT tab and the cockpit render — one artifact, two renderers,
// exactly as the funnel is. Nothing here derives a conclusion: the verdicts
// are the driver's, computed from #169's scripts over the live ledgers.
//
// `--refresh` runs that driver, which is also what the weekly launchd job
// runs. Two writers of one artifact is how a scheduled number and an on-demand
// number end up disagreeing, so there is exactly one.

QString bot_lessons_path() {
    return kalshi_evidence_path(QString::fromLatin1(kKalshiEdgeReportFile));
}

/// The publisher script, resolved the way `cli_script_path` resolves the other
/// repo scripts — from the executable's own directory first, then the working
/// directory. Empty when it cannot be found, which is reported rather than
/// papered over with a "refresh failed" that says nothing.
QString edge_report_script_path() {
    const QString relative = QStringLiteral("scripts/research/kalshi_edge_report.py");
    const QDir exe_dir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        // …/openmarketterminal-qt/build/openterminalcli → repo root
        exe_dir.absoluteFilePath(QStringLiteral("../../") + relative),
        exe_dir.absoluteFilePath(relative),
        QDir::current().absoluteFilePath(relative),
        QDir::current().absoluteFilePath(QStringLiteral("../") + relative),
    };
    for (const QString& candidate : candidates)
        if (QFileInfo::exists(candidate)) return QFileInfo(candidate).canonicalFilePath();
    return {};
}

/// How long the whole refresh may take. The driver bounds each question
/// itself (Q1 pairs ~160k spot ticks against ~126k quotes and is the slow
/// one); this is the outer bound on all four plus start-up, so a wedged
/// interpreter cannot hang a shell forever.
constexpr int kLessonsRefreshTimeoutMs = 3'900'000;

/// Runs the publisher. Returns a CLI exit code; 0 means an artifact was
/// written this run.
int refresh_lessons() {
    const QString script = edge_report_script_path();
    if (script.isEmpty()) {
        std::fprintf(stderr,
                     "kalshi bot lessons --refresh: scripts/research/kalshi_edge_report.py not "
                     "found next to the binary or under the working directory — the artifact is "
                     "NOT refreshed\n");
        return 5;
    }
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty()) {
        std::fprintf(stderr, "kalshi bot lessons --refresh: python3 not found — the artifact is "
                             "NOT refreshed\n");
        return 5;
    }
    std::fprintf(stderr, "kalshi bot lessons: refreshing via %s (this reads the ledgers end to "
                         "end and takes minutes)\n", qUtf8Printable(script));
    std::fflush(stderr);
    QProcess process;
    // Forwarded, so the driver's own per-question progress and any refusal it
    // prints reach the operator verbatim rather than being summarised here.
    process.setProcessChannelMode(QProcess::ForwardedChannels);
    process.start(python, QStringList{script});
    if (!process.waitForStarted(30'000)) {
        std::fprintf(stderr, "kalshi bot lessons --refresh: %s would not start — the artifact is "
                             "NOT refreshed\n", qUtf8Printable(python));
        return 1;
    }
    if (!process.waitForFinished(kLessonsRefreshTimeoutMs)) {
        process.kill();
        process.waitForFinished(5'000);
        std::fprintf(stderr, "kalshi bot lessons --refresh: the publisher did not finish within "
                             "%ds and was killed — the PREVIOUS artifact is left intact (the "
                             "driver writes atomically)\n", kLessonsRefreshTimeoutMs / 1000);
        return 1;
    }
    const int code = process.exitCode();
    if (process.exitStatus() != QProcess::NormalExit || (code != 0 && code != 3)) {
        std::fprintf(stderr, "kalshi bot lessons --refresh: the publisher exited %d — the "
                             "artifact below may be the PREVIOUS one\n", code);
        return 1;
    }
    // 3 is the driver's "every question came back INSUFFICIENT_DATA": a
    // well-formed artifact saying nothing could be measured. It IS published,
    // and the card will say so, so this is a warning rather than a failure.
    if (code == 3)
        std::fprintf(stderr, "kalshi bot lessons --refresh: published, but EVERY question came "
                             "back INSUFFICIENT_DATA\n");
    return 0;
}

int bot_lessons_command(const GlobalOpts& opts, QStringList& args) {
    const bool refresh = take_bool_flag(args, QStringLiteral("--refresh"));
    if (!args.isEmpty()) {
        std::fprintf(stderr, "kalshi bot lessons: unknown option '%s' (JSON is the global flag: "
                             "`openterminalcli --json kalshi bot lessons`)\n",
                     qUtf8Printable(args.first()));
        return 2;
    }
    // A failed refresh does not suppress the card: BOTH renderers still show
    // whatever artifact is on disk, carrying its real age, and the command
    // exits with the refresh's code. That is the honest pair — the operator
    // sees the previous conclusions and can see they are old, rather than
    // seeing nothing (which reads as "no lessons") or seeing them presented as
    // if the refresh had worked. `--json` gets the same treatment as the human
    // text for the same reason the two share a formatter at all.
    int refresh_rc = 0;
    if (refresh) refresh_rc = refresh_lessons();

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const KalshiEdgeReportFile file = kalshi_edge_read_report_file(bot_lessons_path());
    const KalshiEdgeLessonsView view = kalshi_edge_lessons(file, now_ms);

    if (opts.json) {
        QJsonArray rendered;
        for (const KalshiEdgeLesson& lesson : view.lessons)
            rendered.append(QJsonObject{{QStringLiteral("id"), lesson.id},
                                        {QStringLiteral("verdict"), lesson.verdict},
                                        {QStringLiteral("role"), lesson.role},
                                        {QStringLiteral("line"), lesson.text}});
        QJsonObject out{
            {QStringLiteral("report_file"), bot_lessons_path()},
            {QStringLiteral("available"), view.available},
            {QStringLiteral("stale"), view.stale},
            {QStringLiteral("header"), view.header},
            {QStringLiteral("lines"), QJsonArray::fromStringList(view.lines())},
            {QStringLiteral("lessons"), rendered}};
        // The artifact's own object verbatim when there is one: its absent
        // keys stay absent, so a machine reader never sees a fabricated zero.
        if (file.available) out.insert(QStringLiteral("report"), file.object);
        else out.insert(QStringLiteral("unavailable_reason"), file.why);
        // A refresh that failed is stated as a FIELD, not just as an exit code:
        // a reader that only parsed stdout would otherwise take this object for
        // the result of a successful refresh.
        if (refresh) out.insert(QStringLiteral("refreshed"), refresh_rc == 0);
        std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        return refresh_rc != 0 ? refresh_rc : (view.available ? 0 : 3);
    }

    for (const QString& line : view.lines()) std::printf("%s\n", qUtf8Printable(line));
    std::printf("  report %s\n", qUtf8Printable(bot_lessons_path()));
    std::printf("  a lesson here changes NOTHING about what the bot does — it becomes strategy "
                "only through its own issue and review\n");
    // A failed refresh outranks the read: the artifact above is real, but it
    // is not the one this invocation asked for. An absent artifact is not a
    // successful read of an empty record either.
    if (refresh_rc != 0) return refresh_rc;
    return view.available ? 0 : 3;
}

/// Parses a double flag; returns whether the flag was present. An unparseable
/// or non-positive cap sets `bad` — a bad cap is refused, never silently
/// replaced by the default.
bool take_double(QStringList& args, const QString& flag, double& out, bool& bad) {
    QString raw;
    take_string_option(args, flag, raw);
    if (raw.isEmpty()) return false;
    bool ok = false;
    const double value = raw.toDouble(&ok);
    if (!ok || value <= 0.0) {
        std::fprintf(stderr, "kalshi bot: %s expects a positive number (got '%s')\n",
                     qUtf8Printable(flag), qUtf8Printable(raw));
        bad = true;
        return false;
    }
    out = value;
    return true;
}

bool take_int(QStringList& args, const QString& flag, int& out, bool& bad, int minimum = 0) {
    QString raw;
    take_string_option(args, flag, raw);
    if (raw.isEmpty()) return false;
    bool ok = false;
    const int value = raw.toInt(&ok);
    if (!ok || value < minimum) {
        std::fprintf(stderr, "kalshi bot: %s expects an integer >= %d (got '%s')\n",
                     qUtf8Printable(flag), minimum, qUtf8Printable(raw));
        bad = true;
        return false;
    }
    out = value;
    return true;
}

/// Resolve scripts/kalshi_advise/<name> next to the binary, then the repo path.
QString calibrator_script_path(const QString& script_name) {
    const QString bundled = QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("scripts/kalshi_advise/") + script_name);
    if (QFileInfo::exists(bundled)) return bundled;
    const QString fallback =
        QStringLiteral("/Users/haydarevich/src/Open-Terminal/openmarketterminal-qt/scripts/"
                       "kalshi_advise/") +
        script_name;
    return QFileInfo::exists(fallback) ? fallback : QString();
}

/// Run one calibrator publisher synchronously; stdout is the JSON report/cycle.
int run_calibrator_script(const QString& script_name, const QStringList& script_args) {
    const QString path = calibrator_script_path(script_name);
    if (path.isEmpty()) {
        std::fprintf(stderr, "kalshi bot calibrate: %s not found\n", qUtf8Printable(script_name));
        return 5;
    }
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty()) {
        std::fprintf(stderr, "kalshi bot calibrate: python3 not found\n");
        return 5;
    }
    QProcess process;
    process.setProcessChannelMode(QProcess::ForwardedChannels);
    process.setWorkingDirectory(QFileInfo(path).absolutePath());
    QStringList argv{path};
    argv.append(script_args);
    process.start(python, argv);
    if (!process.waitForStarted(30'000)) {
        std::fprintf(stderr, "kalshi bot calibrate: failed to start %s\n", qUtf8Printable(python));
        return 1;
    }
    // once/report finish in seconds; run is indefinite — wait forever for it.
    if (!process.waitForFinished(-1)) {
        process.kill();
        process.waitForFinished(5'000);
        std::fprintf(stderr, "kalshi bot calibrate: %s did not finish\n", qUtf8Printable(script_name));
        return 1;
    }
    if (process.exitStatus() != QProcess::NormalExit) return 1;
    return process.exitCode();
}

QJsonObject scoreboard_family_summary(const QJsonObject& report, const QString& label,
                                      const QString& file) {
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const qint64 generated_ms =
        static_cast<qint64>(report.value(QStringLiteral("generated_at_ms")).toDouble());
    const qint64 age_ms = generated_ms > 0 ? now_ms - generated_ms : -1;
    const QJsonObject predictions = report.value(QStringLiteral("predictions")).toObject();
    QJsonObject out{
        {QStringLiteral("family"), label},
        {QStringLiteral("file"), file},
        {QStringLiteral("present"), !report.isEmpty()},
        {QStringLiteral("schema"), report.value(QStringLiteral("schema"))},
        {QStringLiteral("generated_at_ms"), generated_ms > 0 ? generated_ms : QJsonValue()},
        {QStringLiteral("age_ms"), age_ms},
        {QStringLiteral("scored_contracts"), report.value(QStringLiteral("scored_contracts"))},
        {QStringLiteral("min_scored_contracts"),
         report.value(QStringLiteral("min_scored_contracts"))},
        {QStringLiteral("training_observations"),
         report.value(QStringLiteral("training_observations"))},
        {QStringLiteral("brier_full"), report.value(QStringLiteral("brier_full"))},
        {QStringLiteral("brier_market_mid_raw"),
         report.value(QStringLiteral("brier_market_mid_raw"))},
        {QStringLiteral("adds_value_over_market"),
         report.value(QStringLiteral("adds_value_over_market")).toBool()},
        {QStringLiteral("adds_value_on_bet_eligible"),
         report.value(QStringLiteral("adds_value_on_bet_eligible")).toBool()},
        {QStringLiteral("brier_eligible_full"), report.value(QStringLiteral("brier_eligible_full"))},
        {QStringLiteral("brier_eligible_market_mid_raw"),
         report.value(QStringLiteral("brier_eligible_market_mid_raw"))},
        // The one rule the bot itself gates bidding on (issue #165/#XXX
        // bet-eligible tightening) -- distinct from `adds_value_over_market`
        // above, which is the raw full-population flag alone and can be true
        // while this is false. Screens must read THIS field, never derive
        // their own promotion state from the raw flag (KalshiBotCommands.cpp
        // "one scorer, many readers").
        {QStringLiteral("signal_trusted"), KalshiBotDecision::signal_trusted(report)},
        {QStringLiteral("trusted_variant"), report.value(QStringLiteral("trusted_variant"))},
        {QStringLiteral("open_predictions"), predictions.size()},
        {QStringLiteral("tickers"),
         [&] {
             QJsonArray tickers;
             for (auto it = predictions.constBegin(); it != predictions.constEnd(); ++it)
                 tickers.append(it.key());
             return tickers;
         }()}};
    // Outside-info Phase 1–3 detail the bot already scores — surface it for
    // agents/operators without forcing them to open the raw JSON files.
    if (report.contains(QStringLiteral("ablations")))
        out.insert(QStringLiteral("ablations"), report.value(QStringLiteral("ablations")));
    if (report.contains(QStringLiteral("settlement_parity")))
        out.insert(QStringLiteral("settlement_parity"),
                   report.value(QStringLiteral("settlement_parity")));
    if (report.contains(QStringLiteral("spot_feeds")))
        out.insert(QStringLiteral("spot_feeds"), report.value(QStringLiteral("spot_feeds")));
    return out;
}

void print_scoreboard_ablations_human(const QJsonObject& ablations) {
    if (ablations.isEmpty()) return;
    QStringList parts;
    for (auto it = ablations.constBegin(); it != ablations.constEnd(); ++it) {
        const QJsonObject row = it.value().toObject();
        const QJsonValue brier = row.value(QStringLiteral("brier"));
        const int n = row.value(QStringLiteral("scored_contracts")).toInt();
        const bool beats = row.value(QStringLiteral("beats_mid")).toBool();
        parts << QStringLiteral("%1 brier=%2 n=%3 %4")
                     .arg(it.key(),
                          brier.isDouble() ? QString::number(brier.toDouble(), 'f', 4)
                                           : QStringLiteral("—"))
                     .arg(n)
                     .arg(beats ? QStringLiteral("beats_mid") : QStringLiteral("no_edge"));
    }
    std::printf("  ablations: %s\n", qUtf8Printable(parts.join(QStringLiteral(" · "))));
}

void print_scoreboard_family_human(const QJsonObject& summary) {
    const bool present = summary.value(QStringLiteral("present")).toBool();
    const bool trusted = summary.value(QStringLiteral("signal_trusted")).toBool();
    const qint64 age_ms = static_cast<qint64>(summary.value(QStringLiteral("age_ms")).toDouble(-1));
    const QString variant = summary.value(QStringLiteral("trusted_variant")).toString();
    std::printf("%s  file=%s  %s  age=%s  scored=%s/%s  obs=%s  brier=%s vs mid=%s  trust=%s  "
                "variant=%s  open=%d\n",
                qUtf8Printable(summary.value(QStringLiteral("family")).toString()),
                qUtf8Printable(summary.value(QStringLiteral("file")).toString()),
                present ? "present" : "MISSING",
                age_ms < 0 ? "?"
                           : qUtf8Printable(QStringLiteral("%1s").arg(age_ms / 1000)),
                qUtf8Printable(summary.value(QStringLiteral("scored_contracts")).toVariant().toString()),
                qUtf8Printable(
                    summary.value(QStringLiteral("min_scored_contracts")).toVariant().toString()),
                qUtf8Printable(
                    summary.value(QStringLiteral("training_observations")).toVariant().toString()),
                qUtf8Printable(summary.value(QStringLiteral("brier_full")).toVariant().toString()),
                qUtf8Printable(
                    summary.value(QStringLiteral("brier_market_mid_raw")).toVariant().toString()),
                !present            ? "n/a"
                : trusted           ? "ADDS_VALUE"
                                    : "NO_EDGE_YET",
                variant.isEmpty() ? "—" : qUtf8Printable(variant),
                summary.value(QStringLiteral("open_predictions")).toInt());
    print_scoreboard_ablations_human(summary.value(QStringLiteral("ablations")).toObject());
    const QJsonObject parity = summary.value(QStringLiteral("settlement_parity")).toObject();
    if (!parity.isEmpty()) {
        const QJsonValue rate = parity.value(QStringLiteral("match_rate"));
        std::printf("  settlement_parity: checked=%s matched=%s rate=%s\n",
                    qUtf8Printable(parity.value(QStringLiteral("checked")).toVariant().toString()),
                    qUtf8Printable(parity.value(QStringLiteral("matched")).toVariant().toString()),
                    rate.isDouble() ? qUtf8Printable(QString::number(rate.toDouble(), 'f', 3))
                                    : "—");
    }
    const QJsonArray tickers = summary.value(QStringLiteral("tickers")).toArray();
    if (!tickers.isEmpty()) {
        QStringList names;
        for (const auto& value : tickers) names << value.toString();
        std::printf("  tickers: %s\n", qUtf8Printable(names.join(QStringLiteral(" "))));
    }
}

int bot_scoreboard_command(const GlobalOpts& opts, QStringList& args) {
    if (!args.isEmpty()) {
        std::fprintf(stderr,
                     "kalshi bot scoreboard: unknown option '%s' (JSON is the global flag: "
                     "`openterminalcli --json kalshi bot scoreboard`)\n",
                     qUtf8Printable(args.first()));
        return 2;
    }
    const QJsonObject threshold = read_calibrator_report();
    const QJsonObject kxbtc15m = read_kxbtc15m_calibrator_report();
    const QJsonObject commodities = read_commodities_15m_calibrator_report();
    const QJsonObject gate = read_gate_verdict();
    const QJsonObject out{
        {QStringLiteral("event"), QStringLiteral("kalshi_bot_scoreboard")},
        {QStringLiteral("threshold"),
         scoreboard_family_summary(threshold, QStringLiteral("threshold"),
                                   QStringLiteral("calibrator.json"))},
        {QStringLiteral("kxbtc15m"),
         scoreboard_family_summary(kxbtc15m, QStringLiteral("kxbtc15m"),
                                   QStringLiteral("kxbtc15m-calibrator.json"))},
        {QStringLiteral("commodities15m"),
         scoreboard_family_summary(commodities, QStringLiteral("commodities15m"),
                                   QStringLiteral("commodities-15m-calibrator.json"))},
        {QStringLiteral("gate"),
         QJsonObject{{QStringLiteral("verdict"), gate.value(QStringLiteral("verdict"))},
                     {QStringLiteral("evaluated"), gate.value(QStringLiteral("evaluated"))},
                     {QStringLiteral("settled_bids"),
                      gate.value(QStringLiteral("ledger")).toObject().value(
                          QStringLiteral("settled_bids"))},
                     {QStringLiteral("max_drawdown_usd"),
                      gate.value(QStringLiteral("ledger")).toObject().value(
                          QStringLiteral("max_drawdown_usd"))},
                     {QStringLiteral("net_pnl_usd"),
                      gate.value(QStringLiteral("ledger")).toObject().value(
                          QStringLiteral("net_pnl_usd"))}}}};
    if (opts.json) {
        std::printf("%s\n",
                    QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("KALSHI BOT SCOREBOARD (paper calibrators the bot actually reads)\n");
    print_scoreboard_family_human(out.value(QStringLiteral("threshold")).toObject());
    print_scoreboard_family_human(out.value(QStringLiteral("kxbtc15m")).toObject());
    print_scoreboard_family_human(out.value(QStringLiteral("commodities15m")).toObject());
    const QJsonObject g = out.value(QStringLiteral("gate")).toObject();
    std::printf("gate  verdict=%s  settled=%s  drawdown=%s  net_pnl=%s\n",
                qUtf8Printable(g.value(QStringLiteral("verdict")).toString(QStringLiteral("?"))),
                qUtf8Printable(g.value(QStringLiteral("settled_bids")).toVariant().toString()),
                qUtf8Printable(g.value(QStringLiteral("max_drawdown_usd")).toVariant().toString()),
                qUtf8Printable(g.value(QStringLiteral("net_pnl_usd")).toVariant().toString()));
    return 0;
}

/// Per-settlement bid autopsy — joins paper settlements to originating bids
/// and writes kalshi-bot-postmortems.jsonl + summary. Display/analysis only.
int bot_postmortem_command(const GlobalOpts& opts, QStringList& args) {
    QStringList script_args;
    if (opts.json) script_args << QStringLiteral("--json");
    while (!args.isEmpty()) {
        const QString arg = args.takeFirst();
        if (arg == QLatin1String("--post-gate")) {
            script_args << arg;
            continue;
        }
        if (arg == QLatin1String("--since-ms")) {
            if (args.isEmpty()) {
                std::fprintf(stderr, "kalshi bot postmortem: --since-ms needs a value\n");
                return 2;
            }
            script_args << arg << args.takeFirst();
            continue;
        }
        if (arg.startsWith(QLatin1String("--since-ms="))) {
            script_args << QStringLiteral("--since-ms") << arg.mid(QStringLiteral("--since-ms=").size());
            continue;
        }
        std::fprintf(stderr,
                     "kalshi bot postmortem: unknown option '%s'\n"
                     "usage: kalshi bot postmortem [--json] [--post-gate|--since-ms N]\n"
                     "(JSON may also be the global flag: `openterminalcli --json kalshi bot postmortem`)\n",
                     qUtf8Printable(arg));
        return 2;
    }
    return run_calibrator_script(QStringLiteral("bot_bid_postmortem.py"), script_args);
}

int bot_calibrate_command(const GlobalOpts& /*opts*/, QStringList& args) {
    const QString action = args.isEmpty() ? QString() : args.takeFirst().trimmed().toLower();
    if (action != QStringLiteral("once") && action != QStringLiteral("report") &&
        action != QStringLiteral("run")) {
        std::fprintf(stderr,
                     "usage: kalshi bot calibrate once|report|run "
                     "[--family all|threshold|kxbtc15m|commodities15m] [--interval N]\n");
        return 2;
    }
    QString family = QStringLiteral("all");
    take_string_option(args, QStringLiteral("--family"), family);
    family = family.trimmed().toLower();
    int interval = 60;
    const int interval_idx = args.indexOf(QStringLiteral("--interval"));
    if (interval_idx >= 0) {
        if (interval_idx + 1 >= args.size()) {
            std::fprintf(stderr, "kalshi bot calibrate: --interval needs a value\n");
            return 2;
        }
        bool ok = false;
        interval = args.at(interval_idx + 1).toInt(&ok);
        if (!ok || interval < 10) {
            std::fprintf(stderr, "kalshi bot calibrate: bad --interval\n");
            return 2;
        }
        args.removeAt(interval_idx + 1);
        args.removeAt(interval_idx);
    }
    if (!args.isEmpty()) {
        std::fprintf(stderr, "kalshi bot calibrate: unknown option '%s'\n",
                     qUtf8Printable(args.first()));
        return 2;
    }

    struct Job {
        QString family;
        QString script;
    };
    QVector<Job> jobs;
    if (family == QStringLiteral("all") || family == QStringLiteral("threshold"))
        jobs.append({QStringLiteral("threshold"), QStringLiteral("spot_calibrator.py")});
    if (family == QStringLiteral("all") || family == QStringLiteral("kxbtc15m") ||
        family == QStringLiteral("btc15m"))
        jobs.append({QStringLiteral("kxbtc15m"), QStringLiteral("kxbtc15m_calibrator.py")});
    if (family == QStringLiteral("all") || family == QStringLiteral("commodities15m") ||
        family == QStringLiteral("commodities"))
        jobs.append(
            {QStringLiteral("commodities15m"), QStringLiteral("commodities_15m_calibrator.py")});
    if (jobs.isEmpty()) {
        std::fprintf(stderr,
                     "kalshi bot calibrate: unknown --family '%s' "
                     "(all|threshold|kxbtc15m|commodities15m)\n",
                     qUtf8Printable(family));
        return 2;
    }

    QStringList script_args{action};
    if (action == QStringLiteral("run")) {
        script_args << QStringLiteral("--interval") << QString::number(interval);
        if (jobs.size() > 1) {
            std::fprintf(stderr,
                         "kalshi bot calibrate run: pick one --family "
                         "(cannot foreground-loop multiple publishers)\n");
            return 2;
        }
    }

    int rc = 0;
    for (const Job& job : jobs) {
        std::fprintf(stderr, "kalshi bot calibrate: %s → %s %s\n", qUtf8Printable(job.family),
                     qUtf8Printable(job.script), qUtf8Printable(script_args.join(QLatin1Char(' '))));
        std::fflush(stderr);
        const int job_rc = run_calibrator_script(job.script, script_args);
        if (job_rc != 0) rc = job_rc;
    }
    return rc;
}

} // namespace

int kalshi_bot_command(const GlobalOpts& opts, QStringList args) {
    const QString sub = args.isEmpty() ? QString() : args.takeFirst().trimmed().toLower();
    static const QStringList kSubcommands{QStringLiteral("once"),       QStringLiteral("run"),
                                          QStringLiteral("gate"),       QStringLiteral("status"),
                                          QStringLiteral("stop"),       QStringLiteral("resume"),
                                          QStringLiteral("lessons"),    QStringLiteral("scoreboard"),
                                          QStringLiteral("calibrate"),  QStringLiteral("postmortem")};
    if (opts.help || sub.isEmpty() || !kSubcommands.contains(sub)) {
        bot_usage();
        return opts.help ? 0 : 2;
    }

    // The gate reads a ledger and sealed criteria; the kill switch and the
    // status readout take no trading configuration either. None of the tick's
    // mode or cap flags apply to any of them, so they branch before those are
    // parsed.
    if (sub == QStringLiteral("gate")) return gate_command(opts, args);
    if (sub == QStringLiteral("status")) return bot_status_command(opts, args);
    if (sub == QStringLiteral("stop")) return bot_stop_command(opts, args);
    if (sub == QStringLiteral("resume")) return bot_resume_command(opts, args);
    if (sub == QStringLiteral("lessons")) return bot_lessons_command(opts, args);
    if (sub == QStringLiteral("scoreboard")) return bot_scoreboard_command(opts, args);
    if (sub == QStringLiteral("calibrate")) return bot_calibrate_command(opts, args);
    if (sub == QStringLiteral("postmortem")) return bot_postmortem_command(opts, args);

    // PAPER is the default, and stays the default: `--mode` has to be given
    // explicitly, and the only other value it accepts is `live`.
    QString mode = QStringLiteral("paper");
    take_string_option(args, QStringLiteral("--mode"), mode);
    const bool paper_flag = take_bool_flag(args, QStringLiteral("--paper"));
    mode = mode.trimmed().toLower();
    if (mode != QStringLiteral("paper") && mode != QStringLiteral("live")) {
        std::fprintf(stderr, "kalshi bot: unknown mode '%s' — expected paper or live\n",
                     qUtf8Printable(mode));
        return 2;
    }
    const bool live = mode == QStringLiteral("live");
    // `--paper` is the launchd job's whole safety story, so it must never be a
    // flag that silently loses an argument. Escalating a `--paper` invocation
    // to live because a `--mode live` came after it is the same class of defect
    // as silently degrading a live one to paper: the conflict is refused.
    if (paper_flag && live) {
        std::fprintf(stderr, "kalshi bot: --paper and --mode live contradict each other; "
                             "refusing rather than picking one\n");
        return 2;
    }

    KalshiBotDecision::Config config;
    bool bad = false;
    take_double(args, QStringLiteral("--edge-threshold"), config.edge_threshold, bad);
    take_double(args, QStringLiteral("--cross-margin"), config.cross_margin_usd, bad);
    take_double(args, QStringLiteral("--rest-premium"), config.rest_premium_usd, bad);
    take_double(args, QStringLiteral("--max-stake"), config.max_stake_usd, bad);
    take_double(args, QStringLiteral("--max-all-in"), config.max_all_in_usd, bad);
    take_int(args, QStringLiteral("--min-runway-sec"), config.min_runway_seconds, bad);
    take_int(args, QStringLiteral("--quote-ttl-sec"), config.quote_ttl_seconds, bad, 1);
    take_double(args, QStringLiteral("--max-exposure"), config.max_open_exposure_usd, bad);
    take_double(args, QStringLiteral("--session-budget"), config.session_budget_usd, bad);
    int max_age_sec = static_cast<int>(config.max_report_age_ms / 1000);
    if (take_int(args, QStringLiteral("--max-report-age-sec"), max_age_sec, bad, 1))
        config.max_report_age_ms = static_cast<qint64>(max_age_sec) * 1000;
    if (take_bool_flag(args, QStringLiteral("--no-paper-cashout")))
        config.enable_paper_cashout = false;
    // Default ON: unattended safety net against REPORT_STALE when LaunchAgents lag.
    const bool auto_refresh_calibrators =
        !take_bool_flag(args, QStringLiteral("--no-auto-refresh-calibrators"));

    int interval = kKalshiBotIntervalSeconds;
    int iterations = 0; // 0 = until interrupted
    take_int(args, QStringLiteral("--interval"), interval, bad, 1);
    take_int(args, QStringLiteral("--iterations"), iterations, bad, 1);
    if (bad) return 2;
    if (!args.isEmpty()) {
        std::fprintf(stderr, "kalshi bot: unknown option '%s'\n", qUtf8Printable(args.first()));
        bot_usage();
        return 2;
    }

    // Live admission, re-asked every tick below. A refusal is journaled to the
    // same ledger (mode=live, no numbers) and exits non-zero: a live run that
    // silently degraded to paper would be the dishonest outcome, and one that
    // said nothing at all would leave no evidence it was attempted.
    const auto admit = [&](qint64 now_ms) -> KalshiBotLive::Permission {
        const KalshiBotLive::Permission permission = live_permission(now_ms);
        if (permission.permitted) return permission;
        journal_ledger_row(bot_ledger_path(), KalshiBotLive::refusal_row(permission, now_ms));
        std::fprintf(stderr, "kalshi bot: LIVE REFUSED · %s\n  %s\n",
                     qUtf8Printable(permission.reason_code), qUtf8Printable(permission.detail));
        return permission;
    };
    // In live mode the caps the bot may bid inside are the ARMED SESSION's, not
    // the paper defaults — tightening only, since permit() already clamped them
    // to the charter's ceilings, and submit_order re-checks them regardless.
    const auto apply_session_caps = [&config](const KalshiBotLive::Permission& permission) {
        config.max_stake_usd = std::min(config.max_stake_usd, permission.max_stake_usd);
        config.max_all_in_usd = std::min(config.max_all_in_usd, permission.max_all_in_usd);
    };

    if (sub == QStringLiteral("once")) {
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        if (live) {
            const KalshiBotLive::Permission permission = admit(now_ms);
            if (!permission.permitted) return 4;
            apply_session_caps(permission);
            print_tick(opts,
                       run_live_tick(opts, config, now_ms,
                                     kalshi_bot_read_stop_file(bot_stop_path()), permission, 0.0,
                                     auto_refresh_calibrators),
                       config);
            return 0;
        }
        print_tick(opts,
                   run_tick(config, now_ms, kalshi_bot_read_stop_file(bot_stop_path()), 0.0,
                            auto_refresh_calibrators),
                   config);
        return 0;
    }

    // The session budget accumulates across this run's ticks and is never
    // reset by one: a bounded run is bounded in money as well as in time.
    double session_opened_usd = 0.0;
    for (int i = 0; iterations == 0 || i < iterations; ++i) {
        if (i > 0) QThread::sleep(static_cast<unsigned long>(interval));
        const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
        // Re-read every tick: a switch thrown while this loop slept must be
        // seen by the very next tick, before that tick can bid.
        const KalshiBotStopFile stop = kalshi_bot_read_stop_file(bot_stop_path());
        TickResult tick;
        if (live) {
            // Re-asked every tick, not latched at startup: the arm expires, the
            // gate verdict ages out, and the kill switch is thrown mid-run. A
            // lapsed permission ends the run rather than riding the old answer.
            const KalshiBotLive::Permission permission = admit(now_ms);
            if (!permission.permitted) return 4;
            apply_session_caps(permission);
            tick = run_live_tick(opts, config, now_ms, stop, permission, session_opened_usd,
                                 auto_refresh_calibrators);
        } else {
            tick = run_tick(config, now_ms, stop, session_opened_usd, auto_refresh_calibrators);
        }
        session_opened_usd = tick.session_opened_usd;
        print_tick(opts, tick, config);
        std::fflush(stdout);
        if (tick.stopped) {
            // Exit clean, within this tick. The launchd job's KeepAlive is
            // Crashed-only for exactly this reason: a kill switch that a
            // supervisor immediately respawned would not be a kill switch.
            std::fprintf(stderr, "kalshi bot: kill switch engaged (%s) — exiting\n",
                         qUtf8Printable(bot_stop_path()));
            return 0;
        }
    }
    return 0;
}

} // namespace openmarketterminal::cli
