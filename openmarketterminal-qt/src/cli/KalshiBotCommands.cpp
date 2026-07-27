// `kalshi bot` command family — the PAPER Kalshi bot, ladder rungs 1, 2, 4
// and 6.
//
// One tick (`once`/`run`) is: check the kill switch, settle whatever the
// exchange has actually resolved, manage the orders already working (fill /
// cancel on TTL / cancel on a vanished edge), then read calibrator.json and
// journal one decision row for every contract it offers. Every row lands in
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
#include "services/prediction/kalshi/KalshiEvidenceEngine.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QThread>

#include <algorithm>
#include <cstdio>
#include <functional>

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

/// The calibrator's live report. An unreadable or unparseable file returns an
/// empty object, which KalshiBotDecision::decide() journals as REPORT_MISSING
/// — the bot never substitutes a previous report for a missing one.
QJsonObject read_calibrator_report() {
    QFile file(kalshi_evidence_path(QStringLiteral("calibrator.json")));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject();
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
    double settled_pnl = 0.0;
    double exposure_usd = 0.0;
    double resting_usd = 0.0;
    double session_opened_usd = 0.0;
    QString state;      ///< "ok", or the refusal reason code
    bool signal_trusted = false;
    bool stopped = false;
};

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
                    const KalshiBotStopFile& stop, double session_opened_usd) {
    const QString ledger_path = bot_ledger_path();

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
        const QJsonArray rows = KalshiBotDecision::decide({}, {}, {}, now_ms, config, stop);
        for (const auto& value : rows) {
            journal_ledger_row(ledger_path, value.toObject());
            ++stopped.passes;
        }
        // The book is still out there while the bot is stopped; report it
        // rather than printing zeros the ledger does not support.
        const QJsonArray record = read_ledger(ledger_path, [](const QJsonObject& row) {
            const QString event = row.value(QStringLiteral("event")).toString();
            return event == QLatin1String(kDecisionEvent) ||
                   event == QLatin1String(kSettlementEvent);
        });
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
        return stopped;
    }

    QJsonArray ledger = read_ledger(ledger_path, [](const QJsonObject& row) {
        const QString event = row.value(QStringLiteral("event")).toString();
        return event == QLatin1String(kDecisionEvent) || event == QLatin1String(kSettlementEvent);
    });
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

    // --- settle filled positions first, against those results only ---------
    if (!book.positions.isEmpty()) {
        const QJsonArray fresh =
            KalshiBotDecision::settle_paper(book.positions, settlements, now_ms);
        for (const auto& value : fresh) {
            journal(value.toObject());
            result.settled_pnl += value.toObject().value(QStringLiteral("realized_pnl")).toDouble();
        }
        result.settled = static_cast<int>(fresh.size());
    }

    // --- then manage what is still working (rung 6) ------------------------
    const QJsonObject report = read_calibrator_report();
    const QJsonArray lifecycle =
        KalshiBotOrders::reconcile(book, report, settlements, now_ms, config, paper_cancel);
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

    // --- only then decide, against the book as it now stands ---------------
    book = KalshiBotOrders::replay(ledger);
    KalshiBotDecision::Exposure exposure;
    exposure.at_risk_usd = book.exposure_usd;
    exposure.session_opened_usd = session_opened_usd;
    exposure.resting = book.resting;
    exposure.requoted = KalshiBotOrders::requotable(lifecycle);

    // `book.settled`, not the settlement events alone: a quote that rested
    // through its market's resolution settles into no position and leaves no
    // settlement row, and without its CANCELED_MARKET_SETTLED the contract
    // would be quoted again on the next tick.
    const QJsonArray rows = KalshiBotDecision::decide(report, book.positions, book.settled,
                                                      now_ms, config, stop, exposure);
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
    result.signal_trusted = report.value(QStringLiteral("adds_value_over_market")).toBool();
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
    publish_funnel(ledger, now_ms);
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
                         double session_opened_usd) {
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

    const QJsonObject report = read_calibrator_report();
    // No book of any kind is replayed here. A live order's fills and lifecycle
    // live at the venue, and the paper model would invent them; and the bot's
    // own ledger is not the authority on what the venue holds. One bot order
    // per contract is enforced where the authority is — submit_order's
    // per-contract duplicate guard, against the immutable drafts (#141). A
    // re-bid therefore reaches submit_order and is refused there, before the
    // adapter, and the refusal is journaled like any other.
    const QJsonArray rows =
        KalshiBotDecision::decide(report, {}, {}, now_ms, config, stop);
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

    result.signal_trusted = report.value(QStringLiteral("adds_value_over_market")).toBool();
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
        {QStringLiteral("max_stake_usd"), config.max_stake_usd},
        {QStringLiteral("max_all_in_usd"), config.max_all_in_usd},
        {QStringLiteral("quote_ttl_seconds"), config.quote_ttl_seconds},
        {QStringLiteral("max_open_exposure_usd"), config.max_open_exposure_usd},
        {QStringLiteral("session_budget_usd"), config.session_budget_usd},
        {QStringLiteral("min_runway_seconds"), config.min_runway_seconds},
        {QStringLiteral("max_report_age_ms"), static_cast<double>(config.max_report_age_ms)},
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
    if (tick.settled > 0)
        std::printf("  settled %d · realized $%.2f\n", tick.settled, tick.settled_pnl);
    if (tick.state == QLatin1String("ok"))
        std::printf("  signal %s\n", tick.signal_trusted
                                         ? "TRUSTED (calibrator beats market baseline)"
                                         : "UNTRUSTED — every bid journaled SIGNAL_UNTRUSTED");
    std::printf("  ledger %s\n",
                qUtf8Printable(kalshi_evidence_path(QString::fromLatin1(kLedgerFile))));
}

void bot_usage() {
    std::fprintf(stderr,
                 "usage: kalshi bot once|run [--mode paper|live] [--edge-threshold X] [--max-stake X]\n"
                 "                          [--max-all-in X] [--min-runway-sec N]\n"
                 "                          [--max-report-age-sec N] [--quote-ttl-sec N]\n"
                 "                          [--max-exposure X] [--session-budget X]\n"
                 "       kalshi bot run [--interval N] [--iterations N]\n"
                 "       kalshi bot gate [--json]\n"
                 "       kalshi bot gate seal '{\"min_settled_bids\":300,\"max_drawdown_usd\":5}'\n"
                 "       kalshi bot stop [--reason \"why\"]   throw the kill switch\n"
                 "       kalshi bot resume                  clear it\n"
                 "       kalshi bot status                  what the GUI BOT chip shows\n"
                 "\n"
                 "PAPER is the default everywhere. A bid rests until it fills, its TTL\n"
                 "expires, or its edge goes; a resting remainder counts against\n"
                 "--max-exposure at its limit price.\n"
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

    const QString ledger_path = bot_ledger_path();
    // What the gate is allowed to know about the record's completeness, read
    // from disk rather than inferred from the rows: a truncated record is
    // indistinguishable from a shorter one once it is just an array of rows.
    KalshiBotGate::RecordIntegrity integrity;
    integrity.missing_generations = kalshi_bot_ledger_record(ledger_path).missing;
    integrity.oldest_row_ts_ms = kalshi_bot_oldest_row_ts_ms(ledger_path);
    integrity.published_first_settled_ts_ms = KalshiBotGate::published_anchor_ms(read_gate_verdict());

    const QJsonObject out = KalshiBotGate::evaluate(
        KalshiBotGate::load_params_file(
            kalshi_evidence_path(QString::fromLatin1(KalshiBotGate::kParamsFile))),
        read_ledger(ledger_path, is_event(kDecisionEvent)),
        read_ledger(ledger_path, is_event(kSettlementEvent)),
        QDateTime::currentMSecsSinceEpoch(), integrity);

    const QString evidence_path = kalshi_evidence_path(QString::fromLatin1(KalshiBotGate::kVerdictFile));
    if (!write_json_file(evidence_path, out))
        std::fprintf(stderr, "kalshi bot gate: cannot write %s\n", qUtf8Printable(evidence_path));
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

} // namespace

int kalshi_bot_command(const GlobalOpts& opts, QStringList args) {
    const QString sub = args.isEmpty() ? QString() : args.takeFirst().trimmed().toLower();
    static const QStringList kSubcommands{QStringLiteral("once"),   QStringLiteral("run"),
                                          QStringLiteral("gate"),   QStringLiteral("status"),
                                          QStringLiteral("stop"),   QStringLiteral("resume")};
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
    take_double(args, QStringLiteral("--max-stake"), config.max_stake_usd, bad);
    take_double(args, QStringLiteral("--max-all-in"), config.max_all_in_usd, bad);
    take_int(args, QStringLiteral("--min-runway-sec"), config.min_runway_seconds, bad);
    take_int(args, QStringLiteral("--quote-ttl-sec"), config.quote_ttl_seconds, bad, 1);
    take_double(args, QStringLiteral("--max-exposure"), config.max_open_exposure_usd, bad);
    take_double(args, QStringLiteral("--session-budget"), config.session_budget_usd, bad);
    int max_age_sec = static_cast<int>(config.max_report_age_ms / 1000);
    if (take_int(args, QStringLiteral("--max-report-age-sec"), max_age_sec, bad, 1))
        config.max_report_age_ms = static_cast<qint64>(max_age_sec) * 1000;

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
                                     kalshi_bot_read_stop_file(bot_stop_path()), permission, 0.0),
                       config);
            return 0;
        }
        print_tick(opts,
                   run_tick(config, now_ms, kalshi_bot_read_stop_file(bot_stop_path()), 0.0),
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
            tick = run_live_tick(opts, config, now_ms, stop, permission, session_opened_usd);
        } else {
            tick = run_tick(config, now_ms, stop, session_opened_usd);
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
