// `kalshi bot` command family — the PAPER Kalshi bot, ladder rung 1.
//
// One tick is: settle whatever the exchange has actually resolved, then read
// calibrator.json and journal one decision row for every contract it offers.
// Every row lands in kalshi-bot-decisions.jsonl, passes included: a tick that
// bids nothing still has to say why.
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

using services::prediction::kalshi_ns::KalshiBotDecision;
using services::prediction::kalshi_ns::KalshiEvidenceEngine;

constexpr auto kLedgerFile = "kalshi-bot-decisions.jsonl";
constexpr auto kDecisionEvent = "kalshi_bot_decision";
constexpr auto kSettlementEvent = "kalshi_bot_paper_settlement";

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
};

TickResult run_tick(const KalshiBotDecision::Config& config, qint64 now_ms) {
    const QString ledger_path = kalshi_evidence_path(QString::fromLatin1(kLedgerFile));
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
    const QJsonArray rows = KalshiBotDecision::decide(report, open, settlements, now_ms, config);
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
                 "\n"
                 "Paper only. This rung has no live mode: there is no order path here.\n");
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
    if (opts.help || sub.isEmpty() || (sub != QStringLiteral("once") && sub != QStringLiteral("run"))) {
        bot_usage();
        return opts.help ? 0 : 2;
    }

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

    int interval = 60;
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
        print_tick(opts, run_tick(config, QDateTime::currentMSecsSinceEpoch()), config);
        return 0;
    }

    for (int i = 0; iterations == 0 || i < iterations; ++i) {
        if (i > 0) QThread::sleep(static_cast<unsigned long>(interval));
        print_tick(opts, run_tick(config, QDateTime::currentMSecsSinceEpoch()), config);
        std::fflush(stdout);
    }
    return 0;
}

} // namespace openmarketterminal::cli
