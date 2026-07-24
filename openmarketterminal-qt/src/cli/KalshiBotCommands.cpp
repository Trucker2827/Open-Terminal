// `kalshi bot` command family — the PAPER Kalshi bot, ladder rungs 1 and 4.
//
// One tick is: check the kill switch, settle whatever the exchange has
// actually resolved, then read calibrator.json and journal one decision row
// for every contract it offers. Every row lands in
// kalshi-bot-decisions.jsonl, passes included: a tick that bids nothing still
// has to say why.
//
// Rung 4 (the observable loop) adds three subcommands and one file:
//   `kalshi bot stop`   — throws the kill switch (writes kalshi-bot-stop.json)
//   `kalshi bot resume` — clears it (removes that file)
//   `kalshi bot status` — prints exactly what the GUI BOT chip shows, from the
//                         same classifier and the same staleness constant
//                         (KalshiBotRuntime.h). Two renderers, one truth.
// The loop re-reads the stop file at the top of EVERY tick, before any bid;
// `once` refuses that tick, `run` journals the refusal and exits, so the
// switch halts bidding within one tick either way.
//
// This rung has NO live path. There is no order preparation, no submission,
// no live gate, and no `--mode live` — live mode is refused as unknown, not
// disabled behind a flag. All decision math lives in the pure, unit-tested
// KalshiBotDecision; this file only does I/O, flags, and printing.
//
// Own TU, excluded from unity builds like the other cli command families
// (MSVC front-end capacity; see CommandDispatch.cpp).

#include "cli/KalshiBotCommands.h"

#include "cli/ServeCommand.h"
#include "services/prediction/kalshi/KalshiBotDecision.h"
#include "services/prediction/kalshi/KalshiBotRuntime.h"
#include "services/prediction/kalshi/KalshiEvidenceEngine.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QThread>

#include <cstdio>
#include <functional>

namespace openmarketterminal::cli {

// Shared CLI flag helper (defined in CommandDispatch.cpp; redeclared here to
// avoid pulling EdgeJournalShared.h's heavier includes into this small TU).
bool take_string_option(QStringList& args, const QString& flag, QString& out);
bool take_bool_flag(QStringList& args, const QString& flag);

namespace {

using namespace services::prediction::kalshi_ns;

constexpr auto kLedgerFile = kKalshiBotDecisionLedgerFile;
constexpr auto kDecisionEvent = "kalshi_bot_decision";
constexpr auto kSettlementEvent = "kalshi_bot_paper_settlement";

QString bot_ledger_path() { return kalshi_evidence_path(QString::fromLatin1(kLedgerFile)); }
QString bot_stop_path() { return kalshi_evidence_path(QString::fromLatin1(kKalshiBotStopFileName)); }

/// Reads a jsonl ledger. `keep` filters rows as they are parsed so a large
/// evidence file never has to be held in memory whole.
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

/// The calibrator's live report. An unreadable or unparseable file returns an
/// empty object, which KalshiBotDecision::decide() journals as REPORT_MISSING
/// — the bot never substitutes a previous report for a missing one.
QJsonObject read_calibrator_report() {
    QFile file(kalshi_evidence_path(QStringLiteral("calibrator.json")));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject();
}

struct TickResult {
    int bids = 0;
    int passes = 0;
    int settled = 0;
    int still_open = 0;
    double settled_pnl = 0.0;
    QString state;      ///< "ok", or the refusal reason code
    bool signal_trusted = false;
    bool stopped = false;
};

/// One tick. `stop` was read from disk immediately before this call, so the
/// kill switch is re-read every tick rather than latched at startup.
TickResult run_tick(const KalshiBotDecision::Config& config, qint64 now_ms,
                    const KalshiBotStopFile& stop) {
    const QString ledger_path = bot_ledger_path();

    // The switch short-circuits the whole tick: no settlement pass, no report
    // read, no bid — only the journaled refusal. KalshiBotDecision::decide()
    // checks it again (it is the layer that owns the single path to a bid);
    // this early return is what makes the refusal cheap and total.
    if (stop.engaged) {
        TickResult stopped;
        stopped.stopped = true;
        stopped.state = QString::fromLatin1(KalshiBotDecision::kBotStopped);
        const QJsonArray rows = KalshiBotDecision::decide({}, {}, {}, now_ms, config, stop);
        for (const auto& value : rows) {
            KalshiEvidenceEngine::append_jsonl(ledger_path, value.toObject());
            ++stopped.passes;
        }
        return stopped;
    }

    const auto is_event = [](const char* name) {
        return [name](const QJsonObject& row) {
            return row.value(QStringLiteral("event")).toString() == QLatin1String(name);
        };
    };
    const QJsonArray decisions = read_jsonl(ledger_path, is_event(kDecisionEvent));
    QJsonArray settlements = read_jsonl(ledger_path, is_event(kSettlementEvent));
    QJsonArray open = KalshiBotDecision::open_positions_from_ledger(decisions, settlements);

    TickResult result;

    // --- settle first, against real exchange results only ------------------
    if (!open.isEmpty()) {
        QSet<QString> wanted;
        for (const auto& value : open)
            wanted.insert(value.toObject().value(QStringLiteral("ticker")).toString());
        const auto keep_wanted = [&wanted](const QString& key) {
            return [&wanted, key](const QJsonObject& row) {
                return wanted.contains(row.value(key).toString());
            };
        };
        const QJsonArray fresh = KalshiBotDecision::settle_paper(
            open,
            KalshiBotDecision::normalize_settlements(
                read_jsonl(kalshi_evidence_path(QStringLiteral("kalshi-account-settlements.jsonl")),
                           keep_wanted(QStringLiteral("market_id"))),
                read_jsonl(kalshi_evidence_path(QStringLiteral("kalshi-settlements.jsonl")),
                           keep_wanted(QStringLiteral("kalshi_market_id")))),
            now_ms);
        for (const auto& value : fresh) {
            KalshiEvidenceEngine::append_jsonl(ledger_path, value.toObject());
            settlements.append(value);
            result.settled_pnl += value.toObject().value(QStringLiteral("realized_pnl")).toDouble();
        }
        result.settled = static_cast<int>(fresh.size());
        open = KalshiBotDecision::open_positions_from_ledger(decisions, settlements);
    }

    // --- then decide ------------------------------------------------------
    const QJsonObject report = read_calibrator_report();
    const QJsonArray rows =
        KalshiBotDecision::decide(report, open, settlements, now_ms, config, stop);
    for (const auto& value : rows) {
        const QJsonObject row = value.toObject();
        KalshiEvidenceEngine::append_jsonl(ledger_path, row);
        if (row.value(QStringLiteral("action")).toString() == QStringLiteral("bid"))
            ++result.bids;
        else
            ++result.passes;
    }
    result.signal_trusted = report.value(QStringLiteral("adds_value_over_market")).toBool();
    result.state = QStringLiteral("ok");
    if (rows.size() == 1) {
        const QString reason = rows.first().toObject().value(QStringLiteral("reason_code")).toString();
        if (reason == QLatin1String(KalshiBotDecision::kReportMissing) ||
            reason == QLatin1String(KalshiBotDecision::kReportStale))
            result.state = reason;
    }
    result.still_open = static_cast<int>(open.size()) + result.bids;
    return result;
}

QJsonObject tick_summary(const TickResult& tick, const KalshiBotDecision::Config& config) {
    return QJsonObject{
        {QStringLiteral("mode"), QStringLiteral("paper")},
        {QStringLiteral("live_eligible"), false},
        {QStringLiteral("state"), tick.state},
        {QStringLiteral("stopped"), tick.stopped},
        {QStringLiteral("bids"), tick.bids},
        {QStringLiteral("passes"), tick.passes},
        {QStringLiteral("settled"), tick.settled},
        {QStringLiteral("settled_pnl"), tick.settled_pnl},
        {QStringLiteral("open_positions"), tick.still_open},
        {QStringLiteral("signal_trusted"), tick.signal_trusted},
        {QStringLiteral("edge_threshold"), config.edge_threshold},
        {QStringLiteral("max_stake_usd"), config.max_stake_usd},
        {QStringLiteral("max_all_in_usd"), config.max_all_in_usd},
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
    std::printf("KALSHI BOT · PAPER · %s\n", qUtf8Printable(tick.state));
    if (tick.stopped)
        std::printf("  kill switch engaged — this tick placed no bid; refusal journaled as %s\n",
                    KalshiBotDecision::kBotStopped);
    std::printf("  bids %d · passes %d · open %d\n", tick.bids, tick.passes, tick.still_open);
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
                 "usage: kalshi bot once|run [--paper] [--edge-threshold X] [--max-stake X]\n"
                 "                          [--max-all-in X] [--min-runway-sec N]\n"
                 "                          [--max-report-age-sec N]\n"
                 "       kalshi bot run [--interval N] [--iterations N]\n"
                 "       kalshi bot stop [--reason \"why\"]   throw the kill switch\n"
                 "       kalshi bot resume                  clear it\n"
                 "       kalshi bot status                  what the GUI BOT chip shows\n"
                 "\n"
                 "Paper only. This rung has no live mode: there is no order path here.\n");
}

// --- the kill switch and the status the GUI chip mirrors --------------------

KalshiBotLoopStatus current_loop_status(qint64 now_ms) {
    return kalshi_bot_loop_status(kalshi_bot_newest_ts_ms(kalshi_bot_read_ledger_tail(bot_ledger_path())),
                                  kalshi_bot_read_stop_file(bot_stop_path()), now_ms);
}

QJsonObject status_summary(const KalshiBotLoopStatus& status, qint64 now_ms) {
    QJsonObject out{
        {QStringLiteral("mode"), QStringLiteral("paper")},
        {QStringLiteral("state"), status.state},
        {QStringLiteral("color_role"), kalshi_bot_state_color_role(status.state)},
        {QStringLiteral("headline"), status.headline},
        {QStringLiteral("stopped"), status.stop.engaged},
        {QStringLiteral("stale_after_ms"), static_cast<double>(kKalshiBotStaleMs)},
        {QStringLiteral("interval_seconds"), kKalshiBotIntervalSeconds},
        {QStringLiteral("now_ms"), static_cast<double>(now_ms)},
        {QStringLiteral("ledger"), bot_ledger_path()},
        {QStringLiteral("stop_file"), bot_stop_path()}};
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
    const KalshiBotLoopStatus status = current_loop_status(now_ms);
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(status_summary(status, now_ms))
                                .toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    // Same headline, same state, same colour role the chip paints — this text
    // is produced by kalshi_bot_loop_status(), not re-derived here.
    std::printf("%s\n", qUtf8Printable(status.headline));
    std::printf("  chip %s (%s) · stale after %llds\n", qUtf8Printable(status.state.toUpper()),
                qUtf8Printable(kalshi_bot_state_color_role(status.state)),
                static_cast<long long>(kKalshiBotStaleMs / 1000));
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
    static const QStringList kSubcommands{QStringLiteral("once"), QStringLiteral("run"),
                                          QStringLiteral("status"), QStringLiteral("stop"),
                                          QStringLiteral("resume")};
    if (opts.help || sub.isEmpty() || !kSubcommands.contains(sub)) {
        bot_usage();
        return opts.help ? 0 : 2;
    }

    // The kill switch and the status readout take no trading configuration, so
    // they are answered before any of it is parsed.
    if (sub == QStringLiteral("status")) return bot_status_command(opts, args);
    if (sub == QStringLiteral("stop")) return bot_stop_command(opts, args);
    if (sub == QStringLiteral("resume")) return bot_resume_command(opts, args);

    // Paper is the only mode that exists in this rung. `--mode live` is an
    // unknown mode, not a disabled one: refusing it here is the whole point.
    QString mode = QStringLiteral("paper");
    take_string_option(args, QStringLiteral("--mode"), mode);
    take_bool_flag(args, QStringLiteral("--paper"));
    if (mode.trimmed().toLower() != QStringLiteral("paper")) {
        std::fprintf(stderr,
                     "kalshi bot: unknown mode '%s' — this rung is paper-only and has no live "
                     "order path at all\n",
                     qUtf8Printable(mode));
        return 2;
    }

    KalshiBotDecision::Config config;
    bool bad = false;
    take_double(args, QStringLiteral("--edge-threshold"), config.edge_threshold, bad);
    take_double(args, QStringLiteral("--max-stake"), config.max_stake_usd, bad);
    take_double(args, QStringLiteral("--max-all-in"), config.max_all_in_usd, bad);
    take_int(args, QStringLiteral("--min-runway-sec"), config.min_runway_seconds, bad);
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

    if (sub == QStringLiteral("once")) {
        print_tick(opts,
                   run_tick(config, QDateTime::currentMSecsSinceEpoch(),
                            kalshi_bot_read_stop_file(bot_stop_path())),
                   config);
        return 0;
    }

    for (int i = 0; iterations == 0 || i < iterations; ++i) {
        if (i > 0) QThread::sleep(static_cast<unsigned long>(interval));
        // Re-read every tick: a switch thrown while this loop slept must be
        // seen by the very next tick, before that tick can bid.
        const KalshiBotStopFile stop = kalshi_bot_read_stop_file(bot_stop_path());
        const TickResult tick = run_tick(config, QDateTime::currentMSecsSinceEpoch(), stop);
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
