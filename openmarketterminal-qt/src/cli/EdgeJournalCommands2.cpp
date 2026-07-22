// edge journal command family — part B (replay/proof-loop/score-crypto +
// dispatcher). Split from part A at the exact MSVC front-end detonation
// boundary: the parser state poisoned somewhere in part A crashes (C1001
// msc1.cpp:1589) at the first function signature AFTER
// edge_journal_no_trade_command, even though that function is byte-identical
// to the last green Windows build. A TU boundary fully resets the front-end.

#include "cli/EdgeJournalShared.h"

#include "services/edge_radar/CryptoMicrostructureRadar.h"
#include "storage/repositories/TradeAuditRepository.h"
#include "storage/sqlite/Database.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QThread>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace openmarketterminal::cli {


// edge_journal_rare_alerts_command lives in EdgeJournalRareAlerts.cpp —
// see EdgeJournalShared.h for why it must NOT move back into this TU.

int edge_journal_replay_command(const GlobalOpts& opts, QStringList args) {
    QString symbol;
    QString horizon;
    QString amount_raw;
    QString max_age_raw;
    if (!take_string_option(args, QStringLiteral("--symbol"), symbol) ||
        !take_string_option(args, QStringLiteral("--horizon"), horizon) ||
        !take_string_option(args, QStringLiteral("--amount-usd"), amount_raw) ||
        !take_string_option(args, QStringLiteral("--max-age-hours"), max_age_raw))
        return 2;
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: edge journal replay [--symbol BTC-USD] [--horizon 60s] [--amount-usd N] [--max-age-hours N]\n");
        return 2;
    }
    const double amount = amount_raw.isEmpty() ? 100.0 : amount_raw.toDouble();
    const int max_age_hours = max_age_raw.isEmpty() ? 48 : max_age_raw.toInt();
    QString where = QStringLiteral(" WHERE source='edge crypto-recommend' AND created_at>=?");
    QVariantList params{QDateTime::currentMSecsSinceEpoch() - static_cast<qint64>(max_age_hours) * 3600000LL};
    if (!symbol.trimmed().isEmpty()) {
        where += QStringLiteral(" AND symbol=?");
        params << services::crypto_latency::CryptoLatencyService::normalize_symbol(symbol);
    }
    horizon = edge_normalize_stats_horizon(horizon);
    if (!horizon.isEmpty() && horizon != QLatin1String("all")) {
        where += QStringLiteral(" AND horizon=?");
        params << horizon;
    }
    auto r = Database::instance().execute(
        QStringLiteral("SELECT %1 FROM edge_decision_journal%2 ORDER BY created_at ASC")
            .arg(edge_journal_cols(), where),
        params);
    if (r.is_err()) {
        std::fprintf(stderr, "%s\n", r.error().c_str());
        return 5;
    }
    int decisions = 0, resolved = 0, wins = 0, buys = 0, waiting = 0;
    double pnl = 0.0;
    QJsonArray steps;
    auto& q = r.value();
    if (!opts.json)
        std::printf("%-20s %-9s %-14s %-8s %-8s %-10s %-10s\n", "TIME", "SYMBOL", "CALL", "OUTCOME", "MOVE", "PNL", "CUM_PNL");
    while (q.next()) {
        ++decisions;
        auto scored = edge_score_crypto_recommendation_outcome(q);
        if (scored.is_err()) {
            ++waiting;
            continue;
        }
        const auto o = scored.value();
        ++resolved;
        if (o.outcome == 1)
            ++wins;
        double step_pnl = 0.0;
        if (edge_crypto_is_buy_call(q.value(10).toString(), q.value(9).toString())) {
            ++buys;
            step_pnl = amount * (o.move - o.breakeven);
            pnl += step_pnl;
        }
        if (opts.json) {
            steps.append(QJsonObject{{"time", edge_time_text(q.value(1).toLongLong())},
                                     {"symbol", o.symbol}, {"call", o.call}, {"outcome", edge_outcome_text(o.outcome)},
                                     {"move", o.move}, {"pnl", step_pnl}, {"cumulative_pnl", pnl}});
        } else {
            // Hoist qUtf8Printable temporaries into named QByteArrays (MSVC C1001
            // ICE workaround — same as the other edge_journal row printers).
            const QByteArray c_time = edge_time_text(q.value(1).toLongLong()).left(19).toUtf8();
            const QByteArray c_sym = o.symbol.toUtf8();
            const QByteArray c_call = elide_text(o.call, 14).toUtf8();
            const QByteArray c_out = edge_outcome_text(o.outcome).toUtf8();
            const QByteArray c_move = edge_pct(o.move).toUtf8();
            std::printf("%-20s %-9s %-14s %-8s %-8s %10.4f %10.4f\n", c_time.constData(), c_sym.constData(),
                        c_call.constData(), c_out.constData(), c_move.constData(), step_pnl, pnl);
        }
    }
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(QJsonObject{{"decisions", decisions}, {"resolved", resolved},
                                                       {"wins", wins}, {"buy_trades", buys}, {"waiting", waiting},
                                                       {"pnl", pnl}, {"steps", steps}})
                                .toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("summary      decisions=%d resolved=%d wins=%d win_rate=%s buys=%d waiting=%d pnl=$%.4f\n",
                decisions, resolved, wins, qUtf8Printable(edge_pct(edge_rate(wins, resolved))), buys, waiting, pnl);
    return 0;
}

struct EdgeProofBrokerEvent {
    QString time;
    qint64 ts_ms = 0;
    QString phase;
    QString tool;
    QString account;
    QString mode;
    QString decision;
    QString symbol;
    QString side;
    double quantity = 0.0;
};

static qint64 edge_parse_event_time_ms(const QString& raw) {
    QDateTime dt = QDateTime::fromString(raw, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(raw, Qt::ISODate);
    if (!dt.isValid())
        return 0;
    if (dt.timeSpec() == Qt::LocalTime)
        dt.setTimeZone(QTimeZone::UTC);
    return dt.toMSecsSinceEpoch();
}

static QString edge_symbol_base_for_match(QString symbol) {
    symbol = services::crypto_latency::CryptoLatencyService::normalize_symbol(symbol).toUpper();
    const int dash = symbol.indexOf('-');
    if (dash > 0)
        return symbol.left(dash);
    const int slash = symbol.indexOf('/');
    if (slash > 0)
        return symbol.left(slash);
    return symbol;
}

static bool edge_symbols_match_for_proof(const QString& a, const QString& b) {
    const QString aa = edge_symbol_base_for_match(a);
    const QString bb = edge_symbol_base_for_match(b);
    return !aa.isEmpty() && aa == bb;
}

static QVector<EdgeProofBrokerEvent> edge_recent_broker_events_for_proof(const QString& symbol,
                                                                         qint64 min_ts,
                                                                         qint64 max_ts) {
    QVector<EdgeProofBrokerEvent> events;
    auto r = TradeAuditRepository::instance().recent(2000);
    if (r.is_err())
        return events;
    for (const auto& row : r.value()) {
        const QJsonObject json = trade_audit_row_to_json(row);
        const QString event_symbol = json.value(QStringLiteral("symbol")).toString();
        if (!symbol.trimmed().isEmpty() && !edge_symbols_match_for_proof(symbol, event_symbol))
            continue;
        const qint64 ts = edge_parse_event_time_ms(json.value(QStringLiteral("event_ts")).toString());
        if (ts > 0 && min_ts > 0 && ts < min_ts)
            continue;
        if (ts > 0 && max_ts > 0 && ts > max_ts)
            continue;
        events.push_back(EdgeProofBrokerEvent{
            json.value(QStringLiteral("event_ts")).toString(),
            ts,
            json.value(QStringLiteral("phase")).toString(),
            json.value(QStringLiteral("tool")).toString(),
            json.value(QStringLiteral("account")).toString(),
            json.value(QStringLiteral("mode")).toString(),
            json.value(QStringLiteral("decision")).toString(),
            event_symbol,
            json.value(QStringLiteral("side")).toString(),
            json.value(QStringLiteral("quantity")).toDouble()});
    }
    return events;
}

static std::optional<EdgeProofBrokerEvent> edge_matching_broker_event(const QVector<EdgeProofBrokerEvent>& events,
                                                                      const QString& symbol,
                                                                      qint64 decision_ts,
                                                                      qint64 target_ts) {
    const qint64 start = decision_ts > 0 ? decision_ts - 60000LL : 0;
    const qint64 end = target_ts > 0 ? target_ts + 60000LL : 0;
    for (const auto& event : events) {
        if (!edge_symbols_match_for_proof(symbol, event.symbol))
            continue;
        if (event.ts_ms > 0 && start > 0 && event.ts_ms < start)
            continue;
        if (event.ts_ms > 0 && end > 0 && event.ts_ms > end)
            continue;
        return event;
    }
    return std::nullopt;
}

static bool edge_update_crypto_recommendation_outcome(const CryptoRecommendationOutcome& outcome,
                                                      QString* error = nullptr) {
    const qint64 updated_at = QDateTime::currentMSecsSinceEpoch();
    auto update = Database::instance().execute(
        "UPDATE edge_decision_journal SET outcome=?, resolved_at=?, updated_at=?,"
        " reasons = reasons || ' | scored: future=' || ? || ' move=' || ? || ' breakeven=' || ?"
        " WHERE id=?",
        {outcome.outcome, outcome.future_ts, updated_at,
         QString::number(outcome.future_price, 'f', 8),
         QString::number(outcome.move, 'f', 8),
         QString::number(outcome.breakeven, 'f', 8),
         outcome.id});
    if (update.is_err()) {
        if (error)
            *error = QString::fromStdString(update.error());
        return false;
    }
    QString lake_error;
    edge_append_decision_journal_to_lake(outcome.id, &lake_error);
    return true;
}

struct EdgeProofSymbolStats {
    int signal_count = 0;
    int resolved = 0;
    int waiting = 0;
    int wins = 0;
    int buy_signals = 0;
    int buy_resolved = 0;
    int buy_wins = 0;
    int no_trade_signals = 0;
    int no_trade_resolved = 0;
    int no_trade_correct = 0;
    int matched_orders = 0;
    double paper_pnl = 0.0;
    double avoided_value = 0.0;
};

static QString edge_proof_sample_status(int resolved) {
    if (resolved >= 1000)
        return QStringLiteral("institutional-sample");
    if (resolved >= 500)
        return QStringLiteral("strong-sample");
    if (resolved >= 100)
        return QStringLiteral("useful-sample");
    if (resolved >= 30)
        return QStringLiteral("early-sample");
    return QStringLiteral("warmup");
}

static int edge_proof_next_milestone(int resolved) {
    if (resolved < 30)
        return 30;
    if (resolved < 100)
        return 100;
    if (resolved < 500)
        return 500;
    if (resolved < 1000)
        return 1000;
    return 0;
}

static QString edge_proof_verdict(const EdgeProofSymbolStats& s) {
    const double buy_rate = edge_rate(s.buy_wins, s.buy_resolved);
    const double no_trade_rate = edge_rate(s.no_trade_correct, s.no_trade_resolved);
    if (s.resolved < 30)
        return QStringLiteral("WARMUP");
    if (s.buy_resolved >= 10 && s.paper_pnl > 0.0 && buy_rate >= 0.55)
        return QStringLiteral("BUY EDGE PROVING");
    if (s.buy_resolved >= 10 && (s.paper_pnl <= 0.0 || buy_rate < 0.50))
        return QStringLiteral("BUY WEAK");
    if (s.no_trade_resolved >= 20 && no_trade_rate >= 0.65 && s.buy_resolved < 5)
        return QStringLiteral("AVOID WEAK TRADES");
    if (s.no_trade_resolved >= 20 && no_trade_rate >= 0.65)
        return QStringLiteral("NO-TRADE EDGE");
    return QStringLiteral("MIXED");
}

static QJsonObject edge_proof_stats_json(const QString& symbol, const EdgeProofSymbolStats& s) {
    return QJsonObject{{"symbol", symbol},
                       {"signals", s.signal_count},
                       {"resolved", s.resolved},
                       {"waiting", s.waiting},
                       {"wins", s.wins},
                       {"win_rate", edge_rate(s.wins, s.resolved)},
                       {"buy_signals", s.buy_signals},
                       {"buy_resolved", s.buy_resolved},
                       {"profitable_buy_trades", s.buy_wins},
                       {"profitable_buy_rate", edge_rate(s.buy_wins, s.buy_resolved)},
                       {"no_trade_signals", s.no_trade_signals},
                       {"no_trade_resolved", s.no_trade_resolved},
                       {"correct_no_trade", s.no_trade_correct},
                       {"no_trade_success_rate", edge_rate(s.no_trade_correct, s.no_trade_resolved)},
                       {"paper_pnl_after_cost", s.paper_pnl},
                       {"avoided_value_estimate", s.avoided_value},
                       {"matched_orders", s.matched_orders},
                       {"sample_status", edge_proof_sample_status(s.resolved)},
                       {"next_milestone", edge_proof_next_milestone(s.resolved)},
                       {"verdict", edge_proof_verdict(s)}};
}

int edge_journal_proof_loop_command(const GlobalOpts& opts, QStringList args) {
    QString symbol;
    QString horizon;
    QString max_age_raw;
    QString amount_raw;
    QString limit_raw;
    if (!take_string_option(args, QStringLiteral("--symbol"), symbol) ||
        !take_string_option(args, QStringLiteral("--horizon"), horizon) ||
        !take_string_option(args, QStringLiteral("--max-age-hours"), max_age_raw) ||
        !take_string_option(args, QStringLiteral("--amount-usd"), amount_raw) ||
        !take_string_option(args, QStringLiteral("--limit"), limit_raw))
        return 2;
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: edge journal proof-loop [--symbol BTC-USD] [--horizon 60s] [--amount-usd N] [--max-age-hours N] [--limit N]\n");
        return 2;
    }
    int max_age_hours = max_age_raw.isEmpty() ? 48 : max_age_raw.toInt();
    double amount_usd = amount_raw.isEmpty() ? 100.0 : amount_raw.toDouble();
    int limit = limit_raw.isEmpty() ? 200 : limit_raw.toInt();
    if (max_age_hours < 1 || max_age_hours > 24 * 365 || amount_usd <= 0.0 || limit < 1 || limit > 5000) {
        std::fprintf(stderr, "--max-age-hours must be 1..8760, --amount-usd positive, --limit 1..5000\n");
        return 2;
    }

    QString where = QStringLiteral(" WHERE source='edge crypto-recommend' AND created_at>=?");
    const qint64 min_ts = QDateTime::currentMSecsSinceEpoch() -
                          static_cast<qint64>(max_age_hours) * 3600000LL;
    QVariantList params{min_ts};
    if (!symbol.trimmed().isEmpty()) {
        where += QStringLiteral(" AND symbol=?");
        symbol = services::crypto_latency::CryptoLatencyService::normalize_symbol(symbol);
        params << symbol;
    }
    horizon = edge_normalize_stats_horizon(horizon);
    if (!horizon.isEmpty() && horizon != QLatin1String("all")) {
        where += QStringLiteral(" AND horizon=?");
        params << horizon;
    }
    params << limit;
    auto r = Database::instance().execute(
        QStringLiteral("SELECT %1 FROM edge_decision_journal%2 ORDER BY created_at DESC LIMIT ?")
            .arg(edge_journal_cols(), where),
        params);
    if (r.is_err()) {
        std::fprintf(stderr, "%s\n", r.error().c_str());
        return 5;
    }

    const auto broker_events = edge_recent_broker_events_for_proof(symbol, min_ts, 0);
    int signal_count = 0;
    int resolved = 0;
    int waiting = 0;
    int wins = 0;
    int buy_signals = 0;
    int buy_resolved = 0;
    int buy_wins = 0;
    int no_trade_signals = 0;
    int no_trade_resolved = 0;
    int no_trade_correct = 0;
    int matched_orders = 0;
    int allowed_orders = 0;
    int denied_orders = 0;
    double paper_pnl = 0.0;
    double avoided_value = 0.0;
    QJsonArray rows;
    QMap<QString, EdgeProofSymbolStats> by_symbol;

    auto& q = r.value();
    if (!opts.json) {
        std::printf("PROOF LOOP\n");
        std::printf("signal -> decision -> paper/live order -> post-cost result\n\n");
        std::printf("%-20s %-9s %-14s %-8s %-8s %-10s %-11s %s\n",
                    "TIME", "SYMBOL", "CALL", "RESULT", "MOVE", "PAPER_PNL", "ORDER", "ID");
    }
    while (q.next()) {
        ++signal_count;
        const QString id = q.value(0).toString();
        const qint64 decision_ts = q.value(1).toLongLong();
        const QString row_symbol = q.value(4).toString();
        const QString call = q.value(10).toString();
        const QString side = q.value(9).toString();
        const bool buy_call = edge_crypto_is_buy_call(call, side);
        auto sym_stats = by_symbol.value(row_symbol);
        ++sym_stats.signal_count;
        if (buy_call)
            ++buy_signals;
        else
            ++no_trade_signals;
        if (buy_call)
            ++sym_stats.buy_signals;
        else
            ++sym_stats.no_trade_signals;

        auto scored = edge_score_crypto_recommendation_outcome(q);
        QString result = QStringLiteral("waiting");
        double move = 0.0;
        double pnl = 0.0;
        qint64 target_ts = decision_ts + std::max(5, q.value(21).toInt()) * 1000LL;
        if (scored.is_ok()) {
            const auto outcome = scored.value();
            target_ts = outcome.target_ts;
            QString error;
            edge_update_crypto_recommendation_outcome(outcome, &error);
            result = edge_outcome_text(outcome.outcome);
            move = outcome.move;
            ++resolved;
            ++sym_stats.resolved;
            if (outcome.outcome == 1)
                ++wins;
            if (outcome.outcome == 1)
                ++sym_stats.wins;
            if (buy_call) {
                ++buy_resolved;
                ++sym_stats.buy_resolved;
                pnl = amount_usd * (outcome.move - outcome.breakeven);
                paper_pnl += pnl;
                sym_stats.paper_pnl += pnl;
                if (pnl > 0.0) {
                    ++buy_wins;
                    ++sym_stats.buy_wins;
                }
            } else {
                ++no_trade_resolved;
                ++sym_stats.no_trade_resolved;
                if (outcome.outcome == 1) {
                    ++no_trade_correct;
                    ++sym_stats.no_trade_correct;
                    const double avoided = amount_usd * std::max(0.0, outcome.breakeven - outcome.move);
                    avoided_value += avoided;
                    sym_stats.avoided_value += avoided;
                }
            }
        } else {
            ++waiting;
            ++sym_stats.waiting;
        }

        QString order_label = QStringLiteral("none");
        const auto order = edge_matching_broker_event(broker_events, row_symbol, decision_ts, target_ts);
        if (order) {
            ++matched_orders;
            ++sym_stats.matched_orders;
            order_label = order->decision.trimmed().isEmpty() ? order->phase : order->decision;
            const QString d = order->decision.trimmed().toLower();
            if (d.contains(QStringLiteral("allow")) || d.contains(QStringLiteral("accept")) ||
                d.contains(QStringLiteral("approved")) || d.contains(QStringLiteral("submit")))
                ++allowed_orders;
            if (d.contains(QStringLiteral("deny")) || d.contains(QStringLiteral("reject")) ||
                d.contains(QStringLiteral("block")))
                ++denied_orders;
        }
        by_symbol.insert(row_symbol, sym_stats);

        if (opts.json) {
            rows.append(QJsonObject{{"id", id},
                                    {"decision_time", edge_time_text(decision_ts)},
                                    {"symbol", row_symbol},
                                    {"horizon", q.value(5).toString()},
                                    {"call", call},
                                    {"result", result},
                                    {"move", move},
                                    {"paper_pnl_after_cost", pnl},
                                    {"matched_order", order.has_value()},
                                    {"order_decision", order_label}});
        } else {
            // Hoist qUtf8Printable temporaries into named QByteArrays (MSVC C1001
            // ICE workaround — same as the other edge_journal row printers).
            const QByteArray c_time = edge_time_text(decision_ts).left(19).toUtf8();
            const QByteArray c_sym = row_symbol.toUtf8();
            const QByteArray c_call = elide_text(call, 14).toUtf8();
            const QByteArray c_result = result.toUtf8();
            const QByteArray c_move = (scored.is_ok() ? edge_pct(move) : QStringLiteral("-")).toUtf8();
            const QByteArray c_order = elide_text(order_label, 11).toUtf8();
            const QByteArray c_id = id.left(8).toUtf8();
            std::printf("%-20s %-9s %-14s %-8s %-8s %10.4f %-11s %s\n", c_time.constData(), c_sym.constData(),
                        c_call.constData(), c_result.constData(), c_move.constData(), pnl, c_order.constData(),
                        c_id.constData());
        }
    }

    QJsonArray symbol_rows;
    for (auto it = by_symbol.cbegin(); it != by_symbol.cend(); ++it)
        symbol_rows.append(edge_proof_stats_json(it.key(), it.value()));

    EdgeProofSymbolStats aggregate;
    aggregate.signal_count = signal_count;
    aggregate.resolved = resolved;
    aggregate.waiting = waiting;
    aggregate.wins = wins;
    aggregate.buy_signals = buy_signals;
    aggregate.buy_resolved = buy_resolved;
    aggregate.buy_wins = buy_wins;
    aggregate.no_trade_signals = no_trade_signals;
    aggregate.no_trade_resolved = no_trade_resolved;
    aggregate.no_trade_correct = no_trade_correct;
    aggregate.matched_orders = matched_orders;
    aggregate.paper_pnl = paper_pnl;
    aggregate.avoided_value = avoided_value;

    QJsonObject summary{{"signals", signal_count},
                        {"resolved", resolved},
                        {"waiting", waiting},
                        {"wins", wins},
                        {"win_rate", edge_rate(wins, resolved)},
                        {"buy_signals", buy_signals},
                        {"buy_resolved", buy_resolved},
                        {"profitable_buy_trades", buy_wins},
                        {"profitable_buy_rate", edge_rate(buy_wins, buy_resolved)},
                        {"no_trade_signals", no_trade_signals},
                        {"no_trade_resolved", no_trade_resolved},
                        {"correct_no_trade", no_trade_correct},
                        {"no_trade_success_rate", edge_rate(no_trade_correct, no_trade_resolved)},
                        {"paper_amount_usd", amount_usd},
                        {"paper_pnl_after_cost", paper_pnl},
                        {"avoided_value_estimate", avoided_value},
                        {"broker_events_seen", broker_events.size()},
                        {"matched_orders", matched_orders},
                        {"allowed_orders", allowed_orders},
                        {"denied_orders", denied_orders},
                        {"sample_status", edge_proof_sample_status(resolved)},
                        {"next_milestone", edge_proof_next_milestone(resolved)},
                        {"verdict", edge_proof_verdict(aggregate)}};
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(QJsonObject{{"summary", summary},
                                                       {"by_symbol", symbol_rows},
                                                       {"rows", rows}})
                                .toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("\nSUMMARY\n");
    std::printf("signals      %d resolved=%d waiting=%d win_rate=%s\n",
                signal_count, resolved, waiting, qUtf8Printable(edge_pct(edge_rate(wins, resolved))));
    std::printf("buy          %d resolved=%d profitable=%d rate=%s paper_pnl=$%.4f\n",
                buy_signals, buy_resolved, buy_wins,
                qUtf8Printable(edge_pct(edge_rate(buy_wins, buy_resolved))), paper_pnl);
    std::printf("no trade     %d resolved=%d correct=%d rate=%s avoided=$%.4f\n",
                no_trade_signals, no_trade_resolved, no_trade_correct,
                qUtf8Printable(edge_pct(edge_rate(no_trade_correct, no_trade_resolved))), avoided_value);
    std::printf("orders       broker_events=%d matched=%d allowed=%d denied=%d\n",
                static_cast<int>(broker_events.size()), matched_orders, allowed_orders, denied_orders);
    std::printf("verdict      %s sample=%s next=%d\n",
                qUtf8Printable(edge_proof_verdict(aggregate)),
                qUtf8Printable(edge_proof_sample_status(resolved)),
                edge_proof_next_milestone(resolved));
    std::printf("\nBY SYMBOL\n");
    std::printf("%-9s %7s %7s %8s %5s %8s %7s %8s %10s %s\n",
                "SYMBOL", "SIG", "RES", "WIN%", "BUY", "BUY%", "NO_TR", "NT_OK%", "PNL", "VERDICT");
    for (auto it = by_symbol.cbegin(); it != by_symbol.cend(); ++it) {
        const auto& s = it.value();
        std::printf("%-9s %7d %7d %8s %5d %8s %7d %8s %10.4f %s\n",
                    qUtf8Printable(it.key()),
                    s.signal_count,
                    s.resolved,
                    qUtf8Printable(s.resolved ? edge_pct(edge_rate(s.wins, s.resolved)) : QStringLiteral("-")),
                    s.buy_signals,
                    qUtf8Printable(s.buy_resolved ? edge_pct(edge_rate(s.buy_wins, s.buy_resolved)) : QStringLiteral("-")),
                    s.no_trade_signals,
                    qUtf8Printable(s.no_trade_resolved ? edge_pct(edge_rate(s.no_trade_correct, s.no_trade_resolved)) : QStringLiteral("-")),
                    s.paper_pnl,
                    qUtf8Printable(edge_proof_verdict(s)));
    }
    std::printf("note         direct signal/order IDs are not mandatory yet; matching is by symbol and decision window.\n");
    return 0;
}

QString edge_context_symbol_base(QString symbol) {
    symbol = symbol.trimmed().toUpper();
    symbol.replace('/', '-');
    const int dash = symbol.indexOf('-');
    if (dash > 0)
        return symbol.left(dash);
    return symbol;
}

static bool edge_context_known_crypto_base(const QString& base) {
    static const QSet<QString> bases = {
        QStringLiteral("BTC"),  QStringLiteral("ETH"),  QStringLiteral("SOL"),  QStringLiteral("BNB"),
        QStringLiteral("XRP"),  QStringLiteral("DOGE"), QStringLiteral("ADA"),  QStringLiteral("AVAX"),
        QStringLiteral("TON"),  QStringLiteral("LINK"), QStringLiteral("DOT"),  QStringLiteral("MATIC"),
        QStringLiteral("UNI"),  QStringLiteral("ATOM"), QStringLiteral("LTC"),  QStringLiteral("BCH"),
        QStringLiteral("APT"),  QStringLiteral("ARB"),  QStringLiteral("OP"),   QStringLiteral("SUI"),
        QStringLiteral("TRX"),  QStringLiteral("INJ"),  QStringLiteral("NEAR"), QStringLiteral("WIF"),
        QStringLiteral("PEPE"), QStringLiteral("ZEC"),  QStringLiteral("HYPE"), QStringLiteral("XLM"),
        QStringLiteral("VVV"),  QStringLiteral("DIN")
    };
    return bases.contains(base.trimmed().toUpper());
}

static bool edge_context_symbol_looks_crypto(const QString& raw) {
    const QString s = raw.trimmed().toUpper().replace('/', '-');
    const QString base = edge_context_symbol_base(s);
    if (edge_context_known_crypto_base(base))
        return true;
    return s.endsWith(QStringLiteral("-USD")) || s.endsWith(QStringLiteral("-USDT"));
}

QString edge_context_normalized_symbol(const QString& raw, bool crypto) {
    if (crypto)
        return services::crypto_latency::CryptoLatencyService::normalize_symbol(raw);
    return raw.trimmed().toUpper();
}

QStringList edge_context_news_keywords(const QString& symbol, bool crypto) {
    const QString base = edge_context_symbol_base(symbol);
    QStringList keys{base};
    if (!crypto)
        keys << symbol;
    if (base == QLatin1String("BTC"))
        keys << QStringLiteral("BITCOIN");
    else if (base == QLatin1String("ETH"))
        keys << QStringLiteral("ETHEREUM");
    else if (base == QLatin1String("SOL"))
        keys << QStringLiteral("SOLANA");
    else if (base == QLatin1String("DOGE"))
        keys << QStringLiteral("DOGECOIN");
    else if (base == QLatin1String("XRP"))
        keys << QStringLiteral("RIPPLE");
    if (crypto)
        keys << QStringLiteral("CRYPTO");
    keys.removeDuplicates();
    return keys;
}

static QString edge_context_news_where(const QStringList& keywords, QVariantList& params) {
    QStringList parts;
    for (const auto& raw : keywords) {
        const QString key = raw.trimmed().toUpper();
        if (key.isEmpty())
            continue;
        const QString pattern = QStringLiteral("%%1%").arg(key);
        parts << QStringLiteral("(UPPER(headline) LIKE ? OR UPPER(summary) LIKE ? OR UPPER(tickers) LIKE ?)");
        params << pattern << pattern << pattern;
    }
    return parts.isEmpty() ? QStringLiteral("1=0") : parts.join(QStringLiteral(" OR "));
}

QJsonObject edge_context_news_summary(const QStringList& keywords, int days, int limit) {
    const qint64 since_s = QDateTime::currentSecsSinceEpoch() - static_cast<qint64>(days) * 86400LL;
    QVariantList count_params{since_s};
    const QString where = edge_context_news_where(keywords, count_params);
    QJsonObject out{{"lookback_days", days}, {"keywords", QJsonArray::fromStringList(keywords)}};
    auto count = Database::instance().execute(
        QStringLiteral("SELECT COUNT(*), MAX(sort_ts),"
                       " SUM(CASE WHEN sentiment='BULLISH' THEN 1 ELSE 0 END),"
                       " SUM(CASE WHEN sentiment='BEARISH' THEN 1 ELSE 0 END),"
                       " SUM(CASE WHEN priority IN ('FLASH','URGENT','BREAKING') THEN 1 ELSE 0 END)"
                       " FROM news_articles WHERE sort_ts>=? AND (%1)").arg(where),
        count_params);
    if (count.is_err()) {
        out.insert(QStringLiteral("available"), false);
        out.insert(QStringLiteral("error"), QString::fromStdString(count.error()));
        return out;
    }
    auto& cq = count.value();
    if (cq.next()) {
        out.insert(QStringLiteral("available"), true);
        out.insert(QStringLiteral("articles"), cq.value(0).toInt());
        out.insert(QStringLiteral("latest_at"), edge_time_text(cq.value(1).toLongLong() * 1000LL));
        out.insert(QStringLiteral("bullish"), cq.value(2).toInt());
        out.insert(QStringLiteral("bearish"), cq.value(3).toInt());
        out.insert(QStringLiteral("urgent"), cq.value(4).toInt());
    }

    QVariantList recent_params{since_s};
    const QString recent_where = edge_context_news_where(keywords, recent_params);
    recent_params << limit;
    auto recent = Database::instance().execute(
        QStringLiteral("SELECT headline, source, sentiment, impact, priority, sort_ts, link"
                       " FROM news_articles WHERE sort_ts>=? AND (%1)"
                       " ORDER BY sort_ts DESC LIMIT ?").arg(recent_where),
        recent_params);
    QJsonArray rows;
    if (recent.is_ok()) {
        auto& rq = recent.value();
        while (rq.next()) {
            rows.append(QJsonObject{{"headline", rq.value(0).toString()},
                                    {"source", rq.value(1).toString()},
                                    {"sentiment", rq.value(2).toString()},
                                    {"impact", rq.value(3).toString()},
                                    {"priority", rq.value(4).toString()},
                                    {"time", edge_time_text(rq.value(5).toLongLong() * 1000LL)},
                                    {"link", rq.value(6).toString()}});
        }
    }
    out.insert(QStringLiteral("recent"), rows);
    return out;
}

QJsonObject edge_context_decision_summary(const QString& symbol, int days) {
    const qint64 since_ms = QDateTime::currentMSecsSinceEpoch() - static_cast<qint64>(days) * 86400000LL;
    auto r = Database::instance().execute(
        QStringLiteral("SELECT COUNT(*),"
                       " SUM(CASE WHEN outcome IN (0,1) THEN 1 ELSE 0 END),"
                       " SUM(CASE WHEN outcome=1 THEN 1 ELSE 0 END),"
                       " SUM(CASE WHEN LOWER(call) LIKE '%buy%' OR LOWER(side)='buy' THEN 1 ELSE 0 END),"
                       " AVG(edge_after_cost), AVG(confidence), MAX(created_at)"
                       " FROM edge_decision_journal WHERE symbol=? AND created_at>=?"),
        {symbol, since_ms});
    QJsonObject out{{"lookback_days", days}};
    if (r.is_err()) {
        out.insert(QStringLiteral("available"), false);
        out.insert(QStringLiteral("error"), QString::fromStdString(r.error()));
        return out;
    }
    auto& q = r.value();
    q.next();
    const int total = q.value(0).toInt();
    const int resolved = q.value(1).toInt();
    const int wins = q.value(2).toInt();
    out.insert(QStringLiteral("available"), true);
    out.insert(QStringLiteral("decisions"), total);
    out.insert(QStringLiteral("resolved"), resolved);
    out.insert(QStringLiteral("wins"), wins);
    out.insert(QStringLiteral("win_rate"), edge_rate(wins, resolved));
    out.insert(QStringLiteral("buy_calls"), q.value(3).toInt());
    out.insert(QStringLiteral("average_edge_after_cost"), q.value(4).toDouble());
    out.insert(QStringLiteral("average_confidence"), q.value(5).toDouble());
    out.insert(QStringLiteral("latest_at"), edge_time_text(q.value(6).toLongLong()));
    return out;
}

QJsonObject edge_context_model_summary(const QString& symbol, int limit) {
    auto r = Database::instance().execute(
        "SELECT horizon, direction, readiness, probability, confidence, calibration_score,"
        " sample_count, as_of, trained_at, source"
        " FROM edge_prediction_model_outputs WHERE symbol=? ORDER BY as_of DESC LIMIT ?",
        {symbol, limit});
    QJsonObject out;
    if (r.is_err()) {
        out.insert(QStringLiteral("available"), false);
        out.insert(QStringLiteral("error"), QString::fromStdString(r.error()));
        return out;
    }
    QJsonArray rows;
    auto& q = r.value();
    while (q.next()) {
        rows.append(QJsonObject{{"horizon", q.value(0).toString()},
                                {"direction", q.value(1).toString()},
                                {"readiness", q.value(2).toString()},
                                {"probability", q.value(3).toDouble()},
                                {"confidence", q.value(4).toDouble()},
                                {"calibration_score", q.value(5).toDouble()},
                                {"sample_count", q.value(6).toInt()},
                                {"as_of", edge_time_text(q.value(7).toLongLong())},
                                {"trained_at", edge_time_text(q.value(8).toLongLong())},
                                {"source", q.value(9).toString()}});
    }
    out.insert(QStringLiteral("available"), true);
    out.insert(QStringLiteral("outputs"), rows.size());
    out.insert(QStringLiteral("recent"), rows);
    return out;
}

QJsonObject edge_context_broker_summary(const QString& symbol, int days) {
    QJsonObject out{{"lookback_days", days}};
    auto r = TradeAuditRepository::instance().recent(2000);
    if (r.is_err()) {
        out.insert(QStringLiteral("available"), false);
        out.insert(QStringLiteral("error"), QString::fromStdString(r.error()));
        return out;
    }
    const qint64 since = QDateTime::currentMSecsSinceEpoch() - static_cast<qint64>(days) * 86400000LL;
    int matched = 0;
    int allowed = 0;
    int denied = 0;
    int paper = 0;
    int live = 0;
    QString latest;
    QJsonArray recent;
    for (const auto& row : r.value()) {
        const QJsonObject json = trade_audit_row_to_json(row);
        const QString event_symbol = json.value(QStringLiteral("symbol")).toString();
        if (!edge_symbols_match_for_proof(symbol, event_symbol))
            continue;
        const qint64 ts = edge_parse_event_time_ms(json.value(QStringLiteral("event_ts")).toString());
        if (ts > 0 && ts < since)
            continue;
        ++matched;
        const QString decision = json.value(QStringLiteral("decision")).toString().toLower();
        if (decision.contains(QStringLiteral("allow")) || decision.contains(QStringLiteral("prepared")) ||
            decision.contains(QStringLiteral("filled")) || decision.contains(QStringLiteral("submitted")))
            ++allowed;
        if (decision.contains(QStringLiteral("deny")) || decision.contains(QStringLiteral("reject")) ||
            decision.contains(QStringLiteral("block")))
            ++denied;
        const QString mode = json.value(QStringLiteral("mode")).toString().toLower();
        if (mode == QLatin1String("paper"))
            ++paper;
        if (mode == QLatin1String("live"))
            ++live;
        if (latest.isEmpty())
            latest = json.value(QStringLiteral("event_ts")).toString();
        if (recent.size() < 5) {
            recent.append(QJsonObject{{"time", json.value(QStringLiteral("event_ts")).toString()},
                                      {"tool", json.value(QStringLiteral("tool")).toString()},
                                      {"mode", json.value(QStringLiteral("mode")).toString()},
                                      {"decision", json.value(QStringLiteral("decision")).toString()},
                                      {"side", json.value(QStringLiteral("side")).toString()},
                                      {"quantity", json.value(QStringLiteral("quantity")).toDouble()}});
        }
    }
    out.insert(QStringLiteral("available"), true);
    out.insert(QStringLiteral("events"), matched);
    out.insert(QStringLiteral("allowed"), allowed);
    out.insert(QStringLiteral("denied"), denied);
    out.insert(QStringLiteral("paper"), paper);
    out.insert(QStringLiteral("live"), live);
    out.insert(QStringLiteral("latest_at"), latest.isEmpty() ? QStringLiteral("-") : latest);
    out.insert(QStringLiteral("recent"), recent);
    return out;
}

QJsonArray edge_context_public_sources(bool crypto, const QString& symbol) {
    QJsonArray rows;
    rows.append(QJsonObject{{"name", "local news"},
                            {"status", "available when news cache has matches"},
                            {"command", QStringLiteral("news search %1").arg(edge_context_symbol_base(symbol))}});
    if (crypto) {
        rows.append(QJsonObject{{"name", "exchange tape / microstructure"},
                                {"status", "local live collector"},
                                {"command", QStringLiteral("edge microstructure %1 --watch").arg(symbol)}});
        rows.append(QJsonObject{{"name", "public on-chain whale flows"},
                                {"status", "planned connector"},
                                {"command", "not wired yet; Etherscan-style public endpoints can feed this"}});
        return rows;
    }
    rows.append(QJsonObject{{"name", "SEC Form 4 insider transactions"},
                            {"status", "CLI available"},
                            {"command", QStringLiteral("research insiders %1").arg(symbol)}});
    rows.append(QJsonObject{{"name", "SEC 13F institutional holdings"},
                            {"status", "CLI available"},
                            {"command", QStringLiteral("research 13f-top %1").arg(symbol)}});
    rows.append(QJsonObject{{"name", "politician disclosures"},
                            {"status", "available when local AINVEST_API_KEY is configured"},
                            {"command", QStringLiteral("research politicians %1").arg(symbol)}});
    rows.append(QJsonObject{{"name", "company filings / fundamentals"},
                            {"status", "CLI available"},
                            {"command", QStringLiteral("research filings %1").arg(symbol)}});
    return rows;
}

int edge_context_command(const GlobalOpts& opts, QStringList args) {
    QString asset_class;
    QString days_raw;
    QString limit_raw;
    if (!take_string_option(args, QStringLiteral("--asset-class"), asset_class) ||
        !take_string_option(args, QStringLiteral("--days"), days_raw) ||
        !take_string_option(args, QStringLiteral("--limit"), limit_raw))
        return 2;
    if (args.size() != 1) {
        std::fprintf(stderr, "usage: edge context <symbol> [--asset-class equity|crypto] [--days N] [--limit N]\n");
        return 2;
    }
    int days = days_raw.isEmpty() ? 14 : days_raw.toInt();
    int limit = limit_raw.isEmpty() ? 5 : limit_raw.toInt();
    if (days < 1 || days > 3650 || limit < 1 || limit > 50) {
        std::fprintf(stderr, "--days must be 1..3650 and --limit must be 1..50\n");
        return 2;
    }
    asset_class = asset_class.trimmed().toLower();
    if (!asset_class.isEmpty() && asset_class != QLatin1String("equity") && asset_class != QLatin1String("crypto")) {
        std::fprintf(stderr, "--asset-class must be equity or crypto\n");
        return 2;
    }
    const QString raw = args.first();
    const bool crypto = asset_class == QLatin1String("crypto") ||
                        (asset_class.isEmpty() && edge_context_symbol_looks_crypto(raw));
    const QString symbol = edge_context_normalized_symbol(raw, crypto);
    const QStringList keywords = edge_context_news_keywords(symbol, crypto);

    const QJsonObject news = edge_context_news_summary(keywords, days, limit);
    const QJsonObject decisions = edge_context_decision_summary(symbol, days);
    const QJsonObject models = edge_context_model_summary(symbol, limit);
    const QJsonObject broker = edge_context_broker_summary(symbol, days);
    const QJsonArray sources = edge_context_public_sources(crypto, symbol);
    const QJsonObject out{{"symbol", symbol},
                          {"input", raw},
                          {"asset_class", crypto ? QStringLiteral("crypto") : QStringLiteral("equity")},
                          {"lookback_days", days},
                          {"news", news},
                          {"decision_history", decisions},
                          {"model_outputs", models},
                          {"broker_events", broker},
                          {"public_sources", sources},
                          {"rule", "public/legal data only; no nonpublic inside information enters the probability path"}};
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        return 0;
    }

    std::printf("PUBLIC CONTEXT\n");
    std::printf("symbol       %s (%s)\n", qUtf8Printable(symbol), crypto ? "crypto" : "equity");
    std::printf("lookback     %d days\n", days);
    std::printf("rule         public/legal only; context can explain, veto, or boost, but cannot use future/nonpublic data\n\n");

    std::printf("LOCAL HISTORY\n");
    std::printf("news         %d articles  bullish=%d bearish=%d urgent=%d latest=%s\n",
                news.value("articles").toInt(),
                news.value("bullish").toInt(),
                news.value("bearish").toInt(),
                news.value("urgent").toInt(),
                qUtf8Printable(news.value("latest_at").toString("-")));
    std::printf("decisions    %d resolved=%d win_rate=%s buy_calls=%d avg_edge=%s latest=%s\n",
                decisions.value("decisions").toInt(),
                decisions.value("resolved").toInt(),
                qUtf8Printable(edge_pct(decisions.value("win_rate").toDouble())),
                decisions.value("buy_calls").toInt(),
                qUtf8Printable(edge_pct(decisions.value("average_edge_after_cost").toDouble())),
                qUtf8Printable(decisions.value("latest_at").toString("-")));
    std::printf("models       %d recent outputs\n", models.value("outputs").toInt());
    std::printf("orders       %d events allowed=%d denied=%d paper=%d live=%d latest=%s\n\n",
                broker.value("events").toInt(),
                broker.value("allowed").toInt(),
                broker.value("denied").toInt(),
                broker.value("paper").toInt(),
                broker.value("live").toInt(),
                qUtf8Printable(broker.value("latest_at").toString("-")));

    const QJsonArray recent_news = news.value("recent").toArray();
    if (!recent_news.isEmpty()) {
        std::printf("RECENT NEWS\n");
        for (const auto& v : recent_news) {
            const QJsonObject a = v.toObject();
            std::printf("%-19s %-14s %-7s %-7s %s\n",
                        qUtf8Printable(a.value("time").toString().left(19)),
                        qUtf8Printable(elide_text(a.value("source").toString(), 14)),
                        qUtf8Printable(a.value("sentiment").toString()),
                        qUtf8Printable(a.value("impact").toString()),
                        qUtf8Printable(elide_text(a.value("headline").toString(), 100)));
        }
        std::printf("\n");
    }

    std::printf("PUBLIC EVIDENCE COMMANDS\n");
    for (const auto& v : sources) {
        const QJsonObject s = v.toObject();
        std::printf("%-28s %-48s %s\n",
                    qUtf8Printable(s.value("name").toString()),
                    qUtf8Printable(elide_text(s.value("status").toString(), 48)),
                    qUtf8Printable(s.value("command").toString()));
    }
    return 0;
}

struct EdgeDecisionLane {
    bool found = false;
    QString id;
    QString venue;
    QString source;
    QString symbol;
    QString horizon;
    QString market_id;
    QString question;
    QString direction;
    QString side;
    QString call;
    QString gate;
    QString reasons;
    double market_probability = 0.0;
    double model_probability = 0.0;
    double edge_after_cost = 0.0;
    double confidence = 0.0;
    qint64 created_at = 0;
};

static EdgeDecisionLane edge_decision_lane_from_query(QSqlQuery& q) {
    EdgeDecisionLane lane;
    lane.found = true;
    lane.id = q.value(0).toString();
    lane.created_at = q.value(1).toLongLong();
    lane.venue = q.value(3).toString();
    lane.symbol = q.value(4).toString();
    lane.horizon = q.value(5).toString();
    lane.market_id = q.value(6).toString();
    lane.question = q.value(7).toString();
    lane.direction = q.value(8).toString();
    lane.side = q.value(9).toString();
    lane.call = q.value(10).toString();
    lane.gate = q.value(11).toString();
    lane.market_probability = q.value(12).toDouble();
    lane.model_probability = q.value(13).toDouble();
    lane.edge_after_cost = q.value(15).toDouble();
    lane.confidence = q.value(20).toDouble();
    lane.reasons = q.value(25).toString();
    lane.source = q.value(28).toString();
    return lane;
}

static std::optional<EdgeDecisionLane> edge_latest_lane(const QString& where, const QVariantList& params) {
    auto r = Database::instance().execute(
        QStringLiteral("SELECT %1 FROM edge_decision_journal WHERE %2 ORDER BY created_at DESC LIMIT 1")
            .arg(edge_journal_cols(), where),
        params);
    if (r.is_err())
        return std::nullopt;
    auto& q = r.value();
    if (!q.next())
        return std::nullopt;
    return edge_decision_lane_from_query(q);
}

static QString edge_cockpit_coin_name(const QString& base) {
    static const QHash<QString, QString> names{{"BTC", "Bitcoin"}, {"ETH", "Ethereum"},
                                               {"SOL", "Solana"}, {"XRP", "XRP"},
                                               {"DOGE", "Dogecoin"}, {"ADA", "Cardano"},
                                               {"AVAX", "Avalanche"}, {"LINK", "Chainlink"},
                                               {"LTC", "Litecoin"}, {"BCH", "Bitcoin Cash"}};
    return names.value(base.toUpper(), base.toUpper());
}

static QString edge_cockpit_base_symbol(const QString& symbol) {
    const QString normalized = services::crypto_latency::CryptoLatencyService::normalize_symbol(symbol);
    const int dash = normalized.indexOf('-');
    if (dash > 0)
        return normalized.left(dash).toUpper();
    const int slash = normalized.indexOf('/');
    if (slash > 0)
        return normalized.left(slash).toUpper();
    return normalized.toUpper();
}

static bool edge_call_is_positive(const EdgeDecisionLane& lane) {
    const QString call = lane.call.trimmed().toLower();
    const bool positive_word = call.contains(QStringLiteral("buy")) ||
                               call.contains(QStringLiteral("long candidate")) ||
                               call.contains(QStringLiteral("short candidate"));
    return positive_word && !call.contains(QStringLiteral("no buy")) &&
           !call.contains(QStringLiteral("no trade")) && !call.contains(QStringLiteral("reject"));
}

static bool edge_call_is_watch(const EdgeDecisionLane& lane) {
    const QString call = lane.call.trimmed().toLower();
    return call.contains(QStringLiteral("watch")) || call.contains(QStringLiteral("research"));
}

static bool edge_call_is_stand_down(const EdgeDecisionLane& lane) {
    const QString call = lane.call.trimmed().toLower();
    return call.contains(QStringLiteral("no buy")) || call.contains(QStringLiteral("no trade")) ||
           call.contains(QStringLiteral("reject"));
}

static QString edge_lane_label(const EdgeDecisionLane& lane) {
    if (!lane.found)
        return QStringLiteral("-");
    QString label = lane.call.trimmed().isEmpty() ? lane.gate : lane.call.trimmed();
    if (label.isEmpty())
        label = QStringLiteral("seen");
    return label;
}

static std::optional<EdgeDecisionLane> edge_latest_spot_lane(const QString& symbol, qint64 min_ts) {
    return edge_latest_lane(QStringLiteral("source='edge crypto-recommend' AND symbol=? AND created_at>=?"),
                            {services::crypto_latency::CryptoLatencyService::normalize_symbol(symbol), min_ts});
}

static std::optional<EdgeDecisionLane> edge_latest_perp_lane(const QString& symbol, qint64 min_ts) {
    return edge_latest_lane(QStringLiteral("source='edge long-short-strategy' AND symbol=? AND created_at>=?"),
                            {services::crypto_latency::CryptoLatencyService::normalize_symbol(symbol), min_ts});
}

static std::optional<EdgeDecisionLane> edge_latest_btc5m_lane(qint64 min_ts) {
    return edge_latest_lane(QStringLiteral("source='edge journal-evaluate-btc5m-live' AND symbol='BTC' AND created_at>=?"),
                            {min_ts});
}

static std::optional<EdgeDecisionLane> edge_latest_kalshi_lane(const QString& base, qint64 min_ts) {
    const QString b = base.toUpper();
    const QString coin = edge_cockpit_coin_name(b);
    const QString pattern1 = QStringLiteral("%%1%").arg(b);
    const QString pattern2 = QStringLiteral("%%1%").arg(coin);
    return edge_latest_lane(QStringLiteral("source='edge journal-kalshi-scan' AND created_at>=?"
                                           " AND (question LIKE ? OR market_id LIKE ? OR reasons LIKE ?"
                                           " OR features_json LIKE ? OR question LIKE ? OR features_json LIKE ?)"),
                            {min_ts, pattern1, pattern1, pattern1, pattern1, pattern2, pattern2});
}

static std::optional<EdgeDecisionLane> edge_latest_chronos_lane(const QString& symbol, qint64 min_ts) {
    return edge_latest_lane(QStringLiteral("source='chronos2-forecast' AND symbol=? AND created_at>=?"),
                            {services::crypto_latency::CryptoLatencyService::normalize_symbol(symbol), min_ts});
}

static QString edge_decision_cockpit_action(const EdgeDecisionLane& spot,
                                            const EdgeDecisionLane& perp,
                                            const EdgeDecisionLane& btc5m,
                                            const EdgeDecisionLane& kalshi,
                                            const EdgeDecisionLane& chronos) {
    const bool spot_buy = spot.found && edge_call_is_positive(spot);
    const bool perp_trade = perp.found && edge_call_is_positive(perp);
    const bool pm_buy = (btc5m.found && edge_call_is_positive(btc5m)) ||
                        (kalshi.found && edge_call_is_positive(kalshi));
    const bool pm_watch = (btc5m.found && edge_call_is_watch(btc5m)) ||
                          (kalshi.found && edge_call_is_watch(kalshi));
    const bool chronos_buy = chronos.found && chronos.side == QLatin1String("buy") && edge_call_is_positive(chronos);
    const bool chronos_down = chronos.found && chronos.side == QLatin1String("sell") &&
                              chronos.gate == QLatin1String("pass");
    const bool chronos_conflict = chronos.found && edge_call_is_stand_down(chronos) &&
                                  (spot_buy || perp_trade || pm_buy);
    const bool any_prediction = btc5m.found || kalshi.found;
    if ((spot_buy || perp_trade || pm_buy) && chronos_buy)
        return QStringLiteral("CONFLUENCE");
    if (chronos_conflict)
        return QStringLiteral("WATCH CONFLICT");
    if ((spot_buy || perp_trade) && pm_buy)
        return QStringLiteral("CONFLUENCE");
    if (perp_trade && !spot_buy && !pm_buy)
        return perp.side == QLatin1String("short") ? QStringLiteral("PERP-SHORT") : QStringLiteral("PERP-LONG");
    if (!spot_buy && pm_buy)
        return QStringLiteral("PREDICTION-ONLY");
    if (!spot_buy && !pm_buy && chronos_buy)
        return QStringLiteral("MODEL-ONLY");
    if (!spot_buy && !pm_buy && chronos_down)
        return QStringLiteral("MODEL-DOWN");
    if (spot.found && edge_call_is_stand_down(spot) && !any_prediction)
        return QStringLiteral("STAND DOWN");
    if (spot_buy && !pm_buy)
        return QStringLiteral("SPOT-ONLY");
    if (!spot_buy && pm_watch)
        return QStringLiteral("WATCH PREDICTION");
    if (!spot_buy && !pm_buy && chronos.found)
        return QStringLiteral("WATCH MODEL");
    if (spot.found && edge_call_is_stand_down(spot) && any_prediction)
        return QStringLiteral("STAND DOWN");
    if (spot.found)
        return QStringLiteral("SPOT ONLY DATA");
    return QStringLiteral("NEED DATA");
}

static QJsonObject edge_decision_lane_json(const EdgeDecisionLane& lane) {
    if (!lane.found)
        return QJsonObject{{"found", false}};
    return QJsonObject{{"found", true},
                       {"id", lane.id},
                       {"venue", lane.venue},
                       {"source", lane.source},
                       {"symbol", lane.symbol},
                       {"horizon", lane.horizon},
                       {"market_id", lane.market_id},
                       {"question", lane.question},
                       {"direction", lane.direction},
                       {"side", lane.side},
                       {"call", lane.call},
                       {"gate", lane.gate},
                       {"market_probability", lane.market_probability},
                       {"model_probability", lane.model_probability},
                       {"edge_after_cost", lane.edge_after_cost},
                       {"confidence", lane.confidence},
                       {"created_at", edge_time_text(lane.created_at)},
                       {"reasons", lane.reasons}};
}

int edge_decision_cockpit_command(const GlobalOpts& opts, QStringList args) {
    QString symbols_raw;
    QString max_age_raw;
    if (!take_string_option(args, QStringLiteral("--symbols"), symbols_raw) ||
        !take_string_option(args, QStringLiteral("--max-age-hours"), max_age_raw))
        return 2;
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: edge decision-cockpit [--symbols BTC,ETH,SOL] [--max-age-hours N]\n");
        return 2;
    }
    int max_age_hours = max_age_raw.isEmpty() ? 24 : max_age_raw.toInt();
    if (max_age_hours < 1 || max_age_hours > 24 * 30) {
        std::fprintf(stderr, "--max-age-hours must be 1..720\n");
        return 2;
    }
    const QStringList symbols = edge_parse_crypto_universe_symbols(symbols_raw);
    const qint64 min_ts = QDateTime::currentMSecsSinceEpoch() -
                          static_cast<qint64>(max_age_hours) * 3600000LL;

    const auto latest_btc5m = edge_latest_btc5m_lane(min_ts);
    QJsonArray rows;
    if (!opts.json) {
        std::printf("COMBINED DECISION COCKPIT\n");
        std::printf("Spot, perp long/short, prediction-market, and Chronos lanes are separate; action marks where edge may exist.\n\n");
        std::printf("%-9s %-13s %-9s %-13s %-9s %-13s %-9s %-13s %-9s %-13s %-9s %s\n",
                    "SYMBOL", "SPOT", "S_NET", "PERP", "P_NET", "BTC5M", "B_NET", "KALSHI", "K_NET",
                    "CHRONOS", "C_NET", "ACTION");
    }
    for (const QString& symbol : symbols) {
        const QString base = edge_cockpit_base_symbol(symbol);
        const auto spot = edge_latest_spot_lane(symbol, min_ts).value_or(EdgeDecisionLane{});
        const auto perp = edge_latest_perp_lane(symbol, min_ts).value_or(EdgeDecisionLane{});
        const EdgeDecisionLane btc5m = base == QLatin1String("BTC") ? latest_btc5m.value_or(EdgeDecisionLane{})
                                                                    : EdgeDecisionLane{};
        const auto kalshi = edge_latest_kalshi_lane(base, min_ts).value_or(EdgeDecisionLane{});
        const auto chronos = edge_latest_chronos_lane(symbol, min_ts).value_or(EdgeDecisionLane{});
        const QString action = edge_decision_cockpit_action(spot, perp, btc5m, kalshi, chronos);
        if (opts.json) {
            rows.append(QJsonObject{{"symbol", symbol},
                                    {"base", base},
                                    {"spot", edge_decision_lane_json(spot)},
                                    {"perp_long_short", edge_decision_lane_json(perp)},
                                    {"btc5m_prediction", edge_decision_lane_json(btc5m)},
                                    {"kalshi_prediction", edge_decision_lane_json(kalshi)},
                                    {"chronos_forecast", edge_decision_lane_json(chronos)},
                                    {"action", action}});
        } else {
            std::printf("%-9s %-13s %-9s %-13s %-9s %-13s %-9s %-13s %-9s %-13s %-9s %s\n",
                        qUtf8Printable(symbol),
                        qUtf8Printable(elide_text(edge_lane_label(spot), 13)),
                        qUtf8Printable(spot.found ? edge_pct(spot.edge_after_cost) : QStringLiteral("-")),
                        qUtf8Printable(elide_text(edge_lane_label(perp), 13)),
                        qUtf8Printable(perp.found ? edge_pct(perp.edge_after_cost) : QStringLiteral("-")),
                        qUtf8Printable(elide_text(edge_lane_label(btc5m), 13)),
                        qUtf8Printable(btc5m.found ? edge_pct(btc5m.edge_after_cost) : QStringLiteral("-")),
                        qUtf8Printable(elide_text(edge_lane_label(kalshi), 13)),
                        qUtf8Printable(kalshi.found ? edge_pct(kalshi.edge_after_cost) : QStringLiteral("-")),
                        qUtf8Printable(elide_text(edge_lane_label(chronos), 13)),
                        qUtf8Printable(chronos.found ? edge_pct(chronos.edge_after_cost) : QStringLiteral("-")),
                        qUtf8Printable(action));
        }
    }
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(QJsonObject{{"rows", rows},
                                                       {"max_age_hours", max_age_hours}})
                                .toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("\nLegend       CONFLUENCE=independent lanes agree; WATCH CONFLICT=Chronos conflicts with an actionable lane; MODEL-ONLY/MODEL-DOWN=forecast context only; STAND DOWN=no actionable lane.\n");
    return 0;
}

static QStringList edge_parse_equity_symbols(QString symbols_raw) {
    if (symbols_raw.trimmed().isEmpty())
        symbols_raw = QStringLiteral("AAPL,NVDA,MSFT,SPY,QQQ");
    QStringList out;
    for (const QString& part : symbols_raw.split(',', Qt::SkipEmptyParts)) {
        const QString sym = part.trimmed().toUpper();
        if (!sym.isEmpty() && !out.contains(sym))
            out << sym;
    }
    return out.isEmpty() ? QStringList{QStringLiteral("AAPL")} : out;
}

static std::optional<EdgeDecisionLane> edge_latest_chronos_equity_lane(const QString& symbol, qint64 min_ts) {
    return edge_latest_lane(QStringLiteral("source='chronos2-equity-forecast' AND symbol=? AND created_at>=?"),
                            {symbol.trimmed().toUpper(), min_ts});
}

static QString edge_equity_cockpit_action(const EdgeDecisionLane& chronos) {
    if (!chronos.found)
        return QStringLiteral("NEED DATA");
    if (chronos.gate == QLatin1String("pass") && chronos.side == QLatin1String("buy"))
        return QStringLiteral("MODEL-UP");
    if (chronos.gate == QLatin1String("pass") && chronos.side == QLatin1String("sell"))
        return QStringLiteral("MODEL-DOWN");
    if (edge_call_is_watch(chronos) || edge_call_is_stand_down(chronos))
        return QStringLiteral("WATCH");
    return QStringLiteral("REVIEW");
}

int edge_equity_cockpit_command(const GlobalOpts& opts, QStringList args) {
    QString symbols_raw;
    QString max_age_raw;
    if (!take_string_option(args, QStringLiteral("--symbols"), symbols_raw) ||
        !take_string_option(args, QStringLiteral("--max-age-hours"), max_age_raw))
        return 2;
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: edge equity-cockpit [--symbols AAPL,NVDA,SPY] [--max-age-hours N]\n");
        return 2;
    }
    int max_age_hours = max_age_raw.isEmpty() ? 24 * 7 : max_age_raw.toInt();
    if (max_age_hours < 1 || max_age_hours > 24 * 90) {
        std::fprintf(stderr, "--max-age-hours must be 1..2160\n");
        return 2;
    }
    const qint64 min_ts = QDateTime::currentMSecsSinceEpoch() -
                          static_cast<qint64>(max_age_hours) * 3600000LL;
    const QStringList symbols = edge_parse_equity_symbols(symbols_raw);
    QJsonArray rows;
    if (!opts.json) {
        std::printf("EQUITY DECISION COCKPIT\n");
        std::printf("Chronos equity is a paper forecast lane; trade execution still requires separate fee/risk approval.\n\n");
        std::printf("%-8s %-18s %-9s %-9s %-9s %-19s %s\n",
                    "SYMBOL", "CHRONOS", "NET", "CONF", "HORIZON", "AGE", "ACTION");
    }
    for (const QString& symbol : symbols) {
        const EdgeDecisionLane chronos = edge_latest_chronos_equity_lane(symbol, min_ts).value_or(EdgeDecisionLane{});
        const QString action = edge_equity_cockpit_action(chronos);
        if (opts.json) {
            rows.append(QJsonObject{{"symbol", symbol},
                                    {"chronos_forecast", edge_decision_lane_json(chronos)},
                                    {"action", action}});
        } else {
            const QString age = chronos.found ? edge_time_text(chronos.created_at) : QStringLiteral("-");
            std::printf("%-8s %-18s %-9s %-9s %-9s %-19s %s\n",
                        qUtf8Printable(symbol),
                        qUtf8Printable(elide_text(edge_lane_label(chronos), 18)),
                        qUtf8Printable(chronos.found ? edge_pct(chronos.edge_after_cost) : QStringLiteral("-")),
                        qUtf8Printable(chronos.found ? edge_pct(chronos.confidence) : QStringLiteral("-")),
                        qUtf8Printable(chronos.found ? chronos.horizon : QStringLiteral("-")),
                        qUtf8Printable(age.left(19)),
                        qUtf8Printable(action));
        }
    }
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(QJsonObject{{"rows", rows},
                                                       {"max_age_hours", max_age_hours}})
                                .toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("\nLegend       MODEL-UP/DOWN means the forecast cleared its journal threshold only; use sandbox scorekeeping before live equity orders.\n");
    return 0;
}

QString edge_crypto_raw_tick_symbol(const QString& symbol) {
    return edge_base_crypto_symbol(symbol);
}

Result<CryptoRecommendationOutcome> edge_score_crypto_recommendation_outcome(QSqlQuery& q) {
    CryptoRecommendationOutcome out;
    out.id = q.value(0).toString();
    out.decision_ts = q.value(1).toLongLong();
    out.symbol = q.value(4).toString();
    out.side = q.value(9).toString();
    out.call = q.value(10).toString();
    out.breakeven = std::max(0.0, q.value(17).toDouble() + q.value(18).toDouble());
    const int seconds_left = std::max(5, q.value(21).toInt());
    out.target_ts = out.decision_ts + static_cast<qint64>(seconds_left) * 1000LL;
    const QJsonObject features = QJsonDocument::fromJson(q.value(24).toString().toUtf8()).object();
    out.reference_price = features.value(QStringLiteral("reference_price")).toDouble();
    if (out.reference_price <= 0.0) {
        const QJsonObject micro = features.value(QStringLiteral("microstructure")).toObject();
        out.reference_price = micro.value(QStringLiteral("reference_price")).toDouble();
    }
    if (out.reference_price <= 0.0)
        return Result<CryptoRecommendationOutcome>::err("missing reference price");

    out.raw_tick_symbol = edge_crypto_raw_tick_symbol(out.symbol);
    auto tick_result = Database::instance().execute(
        "SELECT source, price, received_ts FROM edge_prediction_raw_ticks"
        " WHERE symbol=? AND received_ts>=? ORDER BY received_ts ASC LIMIT 1",
        {out.raw_tick_symbol, out.target_ts});
    if (tick_result.is_err())
        return Result<CryptoRecommendationOutcome>::err(tick_result.error());
    auto& tick_query = tick_result.value();
    if (!tick_query.next())
        return Result<CryptoRecommendationOutcome>::err("no future tick yet");
    out.tick_source = tick_query.value(0).toString();
    out.future_price = tick_query.value(1).toDouble();
    out.future_ts = tick_query.value(2).toLongLong();
    if (out.future_price <= 0.0)
        return Result<CryptoRecommendationOutcome>::err("future tick has no price");
    out.move = (out.future_price - out.reference_price) / out.reference_price;

    const bool buy_call = edge_crypto_is_buy_call(out.call, out.side);
    if (buy_call)
        out.outcome = out.move > out.breakeven ? 1 : 0;
    else
        out.outcome = out.move <= out.breakeven ? 1 : 0;
    return Result<CryptoRecommendationOutcome>::ok(out);
}

int edge_journal_score_crypto_command(const GlobalOpts& opts, QStringList args) {
    QString symbol;
    QString limit_raw;
    QString max_age_raw;
    const bool rescore = take_bool_flag(args, QStringLiteral("--rescore"));
    if (!take_string_option(args, QStringLiteral("--symbol"), symbol) ||
        !take_string_option(args, QStringLiteral("--limit"), limit_raw) ||
        !take_string_option(args, QStringLiteral("--max-age-hours"), max_age_raw))
        return 2;
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: edge journal score-crypto [--symbol BTC-USD] [--limit N] [--max-age-hours N] [--rescore]\n");
        return 2;
    }
    int limit = 100;
    if (!limit_raw.isEmpty()) {
        bool ok = false;
        limit = limit_raw.toInt(&ok);
        if (!ok || limit < 1 || limit > 5000) {
            std::fprintf(stderr, "--limit must be 1..5000\n");
            return 2;
        }
    }
    int max_age_hours = 24;
    if (!max_age_raw.isEmpty()) {
        bool ok = false;
        max_age_hours = max_age_raw.toInt(&ok);
        if (!ok || max_age_hours < 1 || max_age_hours > 24 * 30) {
            std::fprintf(stderr, "--max-age-hours must be 1..720\n");
            return 2;
        }
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString sql = QStringLiteral("SELECT %1 FROM edge_decision_journal"
                                 " WHERE source='edge crypto-recommend'"
                                 " AND created_at>=?").arg(edge_journal_cols());
    QVariantList params{now - static_cast<qint64>(max_age_hours) * 3600000LL};
    if (!rescore)
        sql += QStringLiteral(" AND outcome=-1");
    if (!symbol.trimmed().isEmpty()) {
        sql += QStringLiteral(" AND symbol=?");
        params << services::crypto_latency::CryptoLatencyService::normalize_symbol(symbol);
    }
    sql += QStringLiteral(" ORDER BY created_at ASC LIMIT ?");
    params << limit;
    auto r = Database::instance().execute(sql, params);
    if (r.is_err()) {
        std::fprintf(stderr, "%s\n", r.error().c_str());
        return 5;
    }

    QJsonArray rows;
    int scanned = 0;
    int resolved = 0;
    int waiting = 0;
    int wins = 0;
    auto& q = r.value();
    while (q.next()) {
        ++scanned;
        auto scored = edge_score_crypto_recommendation_outcome(q);
        if (scored.is_err()) {
            ++waiting;
            if (opts.json)
                rows.append(QJsonObject{{"id", q.value(0).toString()},
                                        {"status", "waiting"},
                                        {"reason", QString::fromStdString(scored.error())}});
            continue;
        }
        const auto outcome = scored.value();
        const qint64 updated_at = QDateTime::currentMSecsSinceEpoch();
        auto update = Database::instance().execute(
            "UPDATE edge_decision_journal SET outcome=?, resolved_at=?, updated_at=?,"
            " reasons = reasons || ' | scored: future=' || ? || ' move=' || ? || ' breakeven=' || ?"
            " WHERE id=?",
            {outcome.outcome, outcome.future_ts, updated_at,
             QString::number(outcome.future_price, 'f', 8),
             QString::number(outcome.move, 'f', 8),
             QString::number(outcome.breakeven, 'f', 8),
             outcome.id});
        if (update.is_err()) {
            std::fprintf(stderr, "%s\n", update.error().c_str());
            return 5;
        }
        QString lake_error;
        edge_append_decision_journal_to_lake(outcome.id, &lake_error);
        ++resolved;
        if (outcome.outcome == 1)
            ++wins;
        if (opts.json) {
            rows.append(QJsonObject{{"id", outcome.id},
                                    {"status", "resolved"},
                                    {"outcome", edge_outcome_text(outcome.outcome)},
                                    {"call", outcome.call},
                                    {"reference_price", outcome.reference_price},
                                    {"future_price", outcome.future_price},
                                    {"move", outcome.move},
                                    {"breakeven", outcome.breakeven},
                                    {"future_tick_source", outcome.tick_source},
                                    {"future_tick_ts", edge_time_text(outcome.future_ts)}});
        }
    }

    if (opts.json) {
        std::printf("%s\n", QJsonDocument(QJsonObject{{"scanned", scanned},
                                                       {"resolved", resolved},
                                                       {"waiting", waiting},
                                                       {"wins", wins},
                                                       {"rows", rows}})
                                .toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("scanned      %d\n", scanned);
    std::printf("resolved     %d wins=%d win_rate=%s\n",
                resolved, wins, qUtf8Printable(edge_pct(resolved > 0 ? double(wins) / resolved : 0.0)));
    std::printf("waiting      %d\n", waiting);
    return 0;
}

int edge_journal_command(const GlobalOpts& opts, QStringList args) {
    const QString sub = args.isEmpty() ? QStringLiteral("list") : args.takeFirst().trimmed().toLower();
    if (sub == "list" || sub == "ls")
        return edge_journal_list_command(opts, args);
    if (sub == "show" || sub == "get")
        return edge_journal_show_command(opts, args);
    if (sub == "resolve")
        return edge_journal_resolve_command(opts, args);
    if (sub == "stats" || sub == "summary")
        return edge_journal_stats_command(opts, args);
    if (sub == "crypto-stats" || sub == "crypto-accuracy" || sub == "crypto-table" ||
        sub == "scorecard-crypto")
        return edge_journal_crypto_stats_command(opts, args);
    if (sub == "evidence" || sub == "explain")
        return edge_journal_evidence_command(opts, args);
    if (sub == "paper-sim" || sub == "simulate" || sub == "sim")
        return edge_journal_paper_sim_command(opts, args);
    if (sub == "trust" || sub == "trust-score" || sub == "trust-scores")
        return edge_journal_trust_command(opts, args);
    if (sub == "regimes" || sub == "regime")
        return edge_journal_regimes_command(opts, args);
    if (sub == "no-trade" || sub == "do-nothing")
        return edge_journal_no_trade_command(opts, args);
    if (sub == "rare-alerts" || sub == "alerts")
        return edge_journal_rare_alerts_command(opts, args);
    if (sub == "replay")
        return edge_journal_replay_command(opts, args);
    if (sub == "proof-loop" || sub == "proof" || sub == "scorekeeping" || sub == "scoreboard")
        return edge_journal_proof_loop_command(opts, args);
    if (sub == "score-crypto" || sub == "score-crypto-recommendations" ||
        sub == "resolve-crypto" || sub == "crypto-score")
        return edge_journal_score_crypto_command(opts, args);
    if (sub == "evaluate-btc5m-live" || sub == "btc5m-live" || sub == "write-btc5m-live")
        return edge_journal_evaluate_btc5m_live_command(opts, args);
    std::fprintf(stderr, "usage: edge journal list|show|resolve|stats|crypto-stats|evidence|paper-sim|trust|regimes|no-trade|rare-alerts|replay|proof-loop|score-crypto|evaluate-btc5m-live\n");
    return 2;
}

} // namespace openmarketterminal::cli
