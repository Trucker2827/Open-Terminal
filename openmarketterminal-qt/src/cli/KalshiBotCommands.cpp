// `kalshi bot` command family — the PAPER Kalshi bot, ladder rungs 1 and 6.
//
// One tick is: settle whatever the exchange has actually resolved, manage the
// orders already working (fill / cancel on TTL / cancel on a vanished edge),
// then read calibrator.json and journal one decision row for every contract it
// offers. Every row lands in kalshi-bot-decisions.jsonl, passes included: a
// tick that bids nothing still has to say why.
//
// A bid is an ORDER, not a position (rung 6). It rests until something
// observable fills it, its TTL runs out, or its edge goes — and while it rests
// its remainder is counted as exposure at the limit price, the same PR #44
// rule the live execution ledger uses.
//
// This rung has NO live path. There is no order preparation, no submission,
// no live gate, and no `--mode live` — live mode is refused as unknown, not
// disabled behind a flag. All decision and lifecycle math lives in the pure,
// unit-tested KalshiBotDecision / KalshiBotOrders; this file only does I/O,
// flags, and printing.
//
// Own TU, excluded from unity builds like the other cli command families
// (MSVC front-end capacity; see CommandDispatch.cpp).

#include "cli/KalshiBotCommands.h"

#include "cli/ServeCommand.h"
#include "services/prediction/kalshi/KalshiBotDecision.h"
#include "services/prediction/kalshi/KalshiBotOrders.h"
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
using services::prediction::kalshi_ns::KalshiBotOrders;
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
};

/// The paper venue's canceller. The paper book IS the decision ledger, so a
/// cancel is effective exactly when its row lands — there is no separate venue
/// to answer, and inventing a refusal it never gave would be a lie. If the
/// append below fails, no cancel row exists, the order is still resting on the
/// next replay, and it is retried: unconfirmed cancels are handled by the same
/// path either way. A live rung injects a canceller that asks the exchange.
bool paper_cancel(const QJsonObject&, const QString&) { return true; }

TickResult run_tick(const KalshiBotDecision::Config& config, qint64 now_ms,
                    double session_opened_usd) {
    const QString ledger_path = kalshi_evidence_path(QString::fromLatin1(kLedgerFile));
    QJsonArray ledger = read_jsonl(ledger_path, [](const QJsonObject& row) {
        const QString event = row.value(QStringLiteral("event")).toString();
        return event == QLatin1String(kDecisionEvent) || event == QLatin1String(kSettlementEvent);
    });
    const auto journal = [&ledger, &ledger_path](const QJsonObject& row) {
        KalshiEvidenceEngine::append_jsonl(ledger_path, row);
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
                                                      now_ms, config, exposure);
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
    std::printf("KALSHI BOT · PAPER · %s\n", qUtf8Printable(tick.state));
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
                 "usage: kalshi bot once|run [--paper] [--edge-threshold X] [--max-stake X]\n"
                 "                          [--max-all-in X] [--min-runway-sec N]\n"
                 "                          [--max-report-age-sec N] [--quote-ttl-sec N]\n"
                 "                          [--max-exposure X] [--session-budget X]\n"
                 "       kalshi bot run [--interval N] [--iterations N]\n"
                 "\n"
                 "Paper only. This rung has no live mode: there is no order path here.\n"
                 "A bid rests until it fills, its TTL expires, or its edge goes; a resting\n"
                 "remainder counts against --max-exposure at its limit price.\n");
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
    take_int(args, QStringLiteral("--quote-ttl-sec"), config.quote_ttl_seconds, bad, 1);
    take_double(args, QStringLiteral("--max-exposure"), config.max_open_exposure_usd, bad);
    take_double(args, QStringLiteral("--session-budget"), config.session_budget_usd, bad);
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
        print_tick(opts, run_tick(config, QDateTime::currentMSecsSinceEpoch(), 0.0), config);
        return 0;
    }

    // The session budget accumulates across this run's ticks and is never
    // reset by one: a bounded run is bounded in money as well as in time.
    double session_opened_usd = 0.0;
    for (int i = 0; iterations == 0 || i < iterations; ++i) {
        if (i > 0) QThread::sleep(static_cast<unsigned long>(interval));
        const TickResult tick = run_tick(config, QDateTime::currentMSecsSinceEpoch(),
                                         session_opened_usd);
        session_opened_usd = tick.session_opened_usd;
        print_tick(opts, tick, config);
        std::fflush(stdout);
    }
    return 0;
}

} // namespace openmarketterminal::cli
