// edge journal command family — compiled as its own TU; see the note in
// CommandDispatch.cpp and EdgeJournalShared.h (MSVC front-end capacity).

#include "cli/EdgeJournalShared.h"

#include "storage/sqlite/Database.h"
#include "services/edge_radar/CryptoMicrostructureRadar.h"
#include <QSqlQuery>
#include "storage/repositories/TradeAuditRepository.h"
#include <QThread>
#include <QTimeZone>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace openmarketterminal::cli {
QString edge_normalize_stats_horizon(QString horizon) {
    horizon = horizon.trimmed().toLower();
    if (horizon.isEmpty() || horizon == QLatin1String("all"))
        return horizon;
    bool ok = false;
    horizon.toInt(&ok);
    if (ok)
        return QStringLiteral("%1s").arg(horizon);
    return horizon;
}

// CryptoRecommendationOutcome moved to EdgeJournalShared.h.

Result<CryptoRecommendationOutcome> edge_score_crypto_recommendation_outcome(QSqlQuery& q);

int edge_journal_crypto_stats_command(const GlobalOpts& opts, QStringList args) {
    QString symbol;
    QString horizon;
    QString max_age_raw;
    if (!take_string_option(args, QStringLiteral("--symbol"), symbol) ||
        !take_string_option(args, QStringLiteral("--horizon"), horizon) ||
        !take_string_option(args, QStringLiteral("--max-age-hours"), max_age_raw))
        return 2;
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: edge journal crypto-stats [--symbol BTC-USD] [--horizon 60s] [--max-age-hours N]\n");
        return 2;
    }

    int max_age_hours = 24 * 7;
    if (!max_age_raw.isEmpty()) {
        bool ok = false;
        max_age_hours = max_age_raw.toInt(&ok);
        if (!ok || max_age_hours < 1 || max_age_hours > 24 * 365) {
            std::fprintf(stderr, "--max-age-hours must be 1..8760\n");
            return 2;
        }
    }

    QString where = QStringLiteral(" WHERE source='edge crypto-recommend' AND created_at>=?");
    QVariantList params{QDateTime::currentMSecsSinceEpoch() -
                        static_cast<qint64>(max_age_hours) * 3600000LL};
    if (!symbol.trimmed().isEmpty()) {
        where += QStringLiteral(" AND symbol=?");
        params << services::crypto_latency::CryptoLatencyService::normalize_symbol(symbol);
    }
    horizon = edge_normalize_stats_horizon(horizon);
    if (!horizon.isEmpty() && horizon != QLatin1String("all")) {
        where += QStringLiteral(" AND horizon=?");
        params << horizon;
    }

    const QString sql = QStringLiteral(
        "SELECT symbol, horizon,"
        " COUNT(*),"
        " SUM(CASE WHEN outcome IN (0,1) THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN outcome=1 THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN call='BUY CANDIDATE' THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN call='BUY CANDIDATE' AND outcome IN (0,1) THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN call='BUY CANDIDATE' AND outcome=1 THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN call!='BUY CANDIDATE' THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN call!='BUY CANDIDATE' AND outcome IN (0,1) THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN call!='BUY CANDIDATE' AND outcome=1 THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN outcome=-1 THEN 1 ELSE 0 END),"
        " AVG(edge_after_cost),"
        " AVG(CASE WHEN outcome IN (0,1) THEN edge_after_cost ELSE NULL END),"
        " AVG(confidence),"
        " MAX(updated_at)"
        " FROM edge_decision_journal%1"
        " GROUP BY symbol, horizon"
        " ORDER BY symbol ASC, horizon ASC").arg(where);
    auto r = Database::instance().execute(sql, params);
    if (r.is_err()) {
        std::fprintf(stderr, "%s\n", r.error().c_str());
        return 5;
    }

    auto rate = [](int wins, int resolved) -> double {
        return resolved > 0 ? static_cast<double>(wins) / static_cast<double>(resolved) : 0.0;
    };
    auto rate_text = [&](int wins, int resolved) -> QString {
        return resolved > 0 ? edge_pct(rate(wins, resolved)) : QStringLiteral("-");
    };

    QJsonArray rows;
    auto& q = r.value();
    if (opts.json) {
        while (q.next()) {
            const int total = q.value(2).toInt();
            const int resolved = q.value(3).toInt();
            const int wins = q.value(4).toInt();
            const int buy_total = q.value(5).toInt();
            const int buy_resolved = q.value(6).toInt();
            const int buy_wins = q.value(7).toInt();
            const int no_buy_total = q.value(8).toInt();
            const int no_buy_resolved = q.value(9).toInt();
            const int no_buy_wins = q.value(10).toInt();
            rows.append(QJsonObject{{"symbol", q.value(0).toString()},
                                    {"horizon", q.value(1).toString()},
                                    {"decisions", total},
                                    {"resolved", resolved},
                                    {"wins", wins},
                                    {"win_rate", rate(wins, resolved)},
                                    {"buy_candidates", buy_total},
                                    {"buy_candidates_resolved", buy_resolved},
                                    {"profitable_after_cost", buy_wins},
                                    {"profitable_after_cost_rate", rate(buy_wins, buy_resolved)},
                                    {"no_buy_decisions", no_buy_total},
                                    {"no_buy_resolved", no_buy_resolved},
                                    {"avoided_unprofitable_after_cost", no_buy_wins},
                                    {"no_buy_success_rate", rate(no_buy_wins, no_buy_resolved)},
                                    {"pending", q.value(11).toInt()},
                                    {"average_edge_after_cost", q.value(12).toDouble()},
                                    {"resolved_average_edge_after_cost", q.value(13).toDouble()},
                                    {"average_confidence", q.value(14).toDouble()},
                                    {"updated_at", edge_time_text(q.value(15).toLongLong())}});
        }
        std::printf("%s\n", QJsonDocument(QJsonObject{{"rows", rows},
                                                       {"max_age_hours", max_age_hours}})
                                .toJson(QJsonDocument::Compact).constData());
        return 0;
    }

    std::printf("CRYPTO RECOMMENDATION SCORECARD\n");
    std::printf("Scores are post-cost: BUY wins only if later move beat fee+spread+slippage; NO BUY wins if it did not.\n\n");
    std::printf("%-9s %-7s %9s %9s %8s %7s %8s %8s %8s %10s %9s %s\n",
                "SYMBOL", "HZN", "DECISIONS", "RESOLVED", "WIN%", "BUY", "PFT%",
                "NO_BUY", "OK%", "PENDING", "AVG_NET", "UPDATED");
    bool any = false;
    while (q.next()) {
        any = true;
        const int total = q.value(2).toInt();
        const int resolved = q.value(3).toInt();
        const int wins = q.value(4).toInt();
        const int buy_total = q.value(5).toInt();
        const int buy_resolved = q.value(6).toInt();
        const int buy_wins = q.value(7).toInt();
        const int no_buy_total = q.value(8).toInt();
        const int no_buy_resolved = q.value(9).toInt();
        const int no_buy_wins = q.value(10).toInt();
        const int pending = q.value(11).toInt();
        std::printf("%-9s %-7s %9d %9d %8s %7d %8s %8d %8s %10d %9s %s\n",
                    qUtf8Printable(q.value(0).toString()),
                    qUtf8Printable(q.value(1).toString()),
                    total,
                    resolved,
                    qUtf8Printable(rate_text(wins, resolved)),
                    buy_total,
                    qUtf8Printable(rate_text(buy_wins, buy_resolved)),
                    no_buy_total,
                    qUtf8Printable(rate_text(no_buy_wins, no_buy_resolved)),
                    pending,
                    qUtf8Printable(edge_pct(q.value(12).toDouble())),
                    qUtf8Printable(edge_time_text(q.value(15).toLongLong())));
    }
    if (!any)
        std::printf("No crypto recommendation rows found yet. Run `edge crypto-universe` or enable daemon collectors.\n");
    return 0;
}

double edge_rate(int wins, int resolved) {
    return resolved > 0 ? static_cast<double>(wins) / static_cast<double>(resolved) : 0.0;
}

bool edge_crypto_is_buy_call(const QString& call, const QString& side) {
    Q_UNUSED(side);
    return call == QLatin1String("BUY CANDIDATE");
}

QString edge_crypto_regime_from_features(const QJsonObject& features) {
    const QJsonObject micro = features.value(QStringLiteral("microstructure")).toObject();
    const int live_sources = micro.value(QStringLiteral("live_sources")).toInt();
    const double spread_bps = micro.value(QStringLiteral("cross_source_spread_bps")).toDouble();
    const double tape = micro.value(QStringLiteral("tape_pressure")).toDouble();
    const double book = micro.value(QStringLiteral("book_pressure")).toDouble();
    const int ticks = micro.value(QStringLiteral("tick_count")).toInt();
    if (live_sources < 2 || ticks < 3)
        return QStringLiteral("thin-data");
    if (spread_bps > 20.0)
        return QStringLiteral("wide-spread");
    if (tape > 0.25 && book > -0.15)
        return QStringLiteral("trend-up");
    if (tape < -0.25 && book < 0.15)
        return QStringLiteral("trend-down");
    if (std::abs(tape) < 0.12 && std::abs(book) < 0.18)
        return QStringLiteral("chop");
    if ((tape > 0.20 && book < -0.20) || (tape < -0.20 && book > 0.20))
        return QStringLiteral("conflict");
    return QStringLiteral("mixed");
}

QJsonObject edge_crypto_cost_json(const QJsonObject& features) {
    return features.value(QStringLiteral("cost")).toObject();
}

EdgeCryptoTrust edge_crypto_trust_for_symbol(const QString& symbol,
                                             const QString& horizon,
                                             int max_age_hours) {
    EdgeCryptoTrust out;
    out.symbol = symbol;
    out.horizon = horizon;
    QString where = QStringLiteral(" WHERE source='edge crypto-recommend' AND created_at>=?");
    QVariantList params{QDateTime::currentMSecsSinceEpoch() -
                        static_cast<qint64>(max_age_hours) * 3600000LL};
    if (!symbol.trimmed().isEmpty()) {
        where += QStringLiteral(" AND symbol=?");
        params << services::crypto_latency::CryptoLatencyService::normalize_symbol(symbol);
    }
    const QString h = edge_normalize_stats_horizon(horizon);
    if (!h.isEmpty() && h != QLatin1String("all")) {
        where += QStringLiteral(" AND horizon=?");
        params << h;
    }
    auto r = Database::instance().execute(
        QStringLiteral("SELECT symbol, horizon, COUNT(*),"
                       " SUM(CASE WHEN outcome IN (0,1) THEN 1 ELSE 0 END),"
                       " SUM(CASE WHEN outcome=1 THEN 1 ELSE 0 END),"
                       " SUM(CASE WHEN call='BUY CANDIDATE' AND outcome IN (0,1) THEN 1 ELSE 0 END),"
                       " SUM(CASE WHEN call='BUY CANDIDATE' AND outcome=1 THEN 1 ELSE 0 END),"
                       " SUM(CASE WHEN call!='BUY CANDIDATE' AND outcome IN (0,1) THEN 1 ELSE 0 END),"
                       " SUM(CASE WHEN call!='BUY CANDIDATE' AND outcome=1 THEN 1 ELSE 0 END),"
                       " AVG(edge_after_cost), AVG(confidence)"
                       " FROM edge_decision_journal%1 GROUP BY symbol, horizon"
                       " ORDER BY COUNT(*) DESC LIMIT 1").arg(where),
        params);
    if (r.is_err())
        return out;
    auto& q = r.value();
    if (!q.next())
        return out;
    out.symbol = q.value(0).toString();
    out.horizon = q.value(1).toString();
    out.decisions = q.value(2).toInt();
    out.resolved = q.value(3).toInt();
    out.wins = q.value(4).toInt();
    out.buy_resolved = q.value(5).toInt();
    out.buy_wins = q.value(6).toInt();
    out.no_buy_resolved = q.value(7).toInt();
    out.no_buy_wins = q.value(8).toInt();
    out.avg_edge = q.value(9).toDouble();
    out.avg_confidence = q.value(10).toDouble();
    const double overall = edge_rate(out.wins, out.resolved);
    const double buy = out.buy_resolved > 0 ? edge_rate(out.buy_wins, out.buy_resolved) : overall;
    const double no_buy = out.no_buy_resolved > 0 ? edge_rate(out.no_buy_wins, out.no_buy_resolved) : overall;
    const double sample_weight = std::min(1.0, static_cast<double>(out.resolved) / 30.0);
    out.trust = 100.0 * sample_weight * (0.45 * overall + 0.35 * buy + 0.20 * no_buy);
    if (out.resolved < 5)
        out.status = QStringLiteral("warmup");
    else if (out.trust >= 70.0)
        out.status = QStringLiteral("trusted");
    else if (out.trust >= 45.0)
        out.status = QStringLiteral("watch");
    else
        out.status = QStringLiteral("weak");
    return out;
}

int edge_journal_evidence_command(const GlobalOpts& opts, QStringList args) {
    QString id = args.isEmpty() ? QString() : args.takeFirst();
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: edge journal evidence [id|latest]\n");
        return 2;
    }
    QString sql = QStringLiteral("SELECT %1 FROM edge_decision_journal").arg(edge_journal_cols());
    QVariantList params;
    if (id.isEmpty() || id == QLatin1String("latest")) {
        sql += QStringLiteral(" WHERE source='edge crypto-recommend' ORDER BY created_at DESC LIMIT 1");
    } else {
        sql += QStringLiteral(" WHERE id=?");
        params << id;
    }
    auto r = Database::instance().execute(sql, params);
    if (r.is_err()) {
        std::fprintf(stderr, "%s\n", r.error().c_str());
        return 5;
    }
    auto& q = r.value();
    if (!q.next()) {
        std::fprintf(stderr, "decision not found\n");
        return 4;
    }
    const QJsonObject row = edge_journal_row_to_json(q);
    const QJsonObject features = row.value(QStringLiteral("features")).toObject();
    const QJsonObject micro = features.value(QStringLiteral("microstructure")).toObject();
    const QJsonObject cost = edge_crypto_cost_json(features);
    auto scored = edge_score_crypto_recommendation_outcome(q);
    QJsonObject outcome;
    if (scored.is_ok()) {
        const auto o = scored.value();
        outcome = QJsonObject{{"status", "matured"},
                              {"reference_price", o.reference_price},
                              {"future_price", o.future_price},
                              {"future_tick_source", o.tick_source},
                              {"move", o.move},
                              {"breakeven", o.breakeven},
                              {"would_win", o.outcome == 1}};
    } else {
        outcome = QJsonObject{{"status", "waiting"}, {"reason", QString::fromStdString(scored.error())}};
    }
    const QString regime = edge_crypto_regime_from_features(features);
    if (opts.json) {
        QJsonObject out = row;
        out["regime"] = regime;
        out["outcome_evidence"] = outcome;
        std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("EVIDENCE\n");
    std::printf("id           %s\n", qUtf8Printable(row.value("id").toString()));
    std::printf("decision     %s %s %s call=%s outcome=%s\n",
                qUtf8Printable(row.value("created_at").toString()),
                qUtf8Printable(row.value("symbol").toString()),
                qUtf8Printable(row.value("horizon").toString()),
                qUtf8Printable(row.value("call").toString()),
                qUtf8Printable(row.value("outcome").toString()));
    std::printf("price        ref=%s model=%s confidence=%s regime=%s\n",
                qUtf8Printable(edge_price_or_dash(features.value("reference_price").toDouble())),
                qUtf8Printable(edge_pct(row.value("model_probability").toDouble())),
                qUtf8Printable(edge_pct(row.value("confidence").toDouble())),
                qUtf8Printable(regime));
    std::printf("cost         fee=%.2fbps slippage=%.2fbps spread=%.2fbps min_edge=%.2fbps\n",
                cost.value("fee_bps").toDouble(),
                cost.value("slippage_bps").toDouble(),
                cost.value("cross_source_spread_bps").toDouble(),
                cost.value("min_edge_bps").toDouble());
    std::printf("micro        live=%d freshest=%s age=%.0fms tick_direction=%+.2f top_book_imbalance=%+.2f ticks=%d\n",
                micro.value("live_sources").toInt(),
                qUtf8Printable(micro.value("freshest_source").toString()),
                micro.value("freshest_age_ms").toDouble(),
                micro.value("tape_pressure").toDouble(),
                micro.value("book_pressure").toDouble(),
                micro.value("tick_count").toInt());
    std::printf("reason       %s\n", qUtf8Printable(row.value("reasons").toString()));
    if (outcome.value("status").toString() == QLatin1String("matured")) {
        std::printf("outcome      future=%s move=%s breakeven=%s win=%s source=%s\n",
                    qUtf8Printable(edge_price_or_dash(outcome.value("future_price").toDouble())),
                    qUtf8Printable(edge_pct(outcome.value("move").toDouble())),
                    qUtf8Printable(edge_pct(outcome.value("breakeven").toDouble())),
                    outcome.value("would_win").toBool() ? "yes" : "no",
                    qUtf8Printable(outcome.value("future_tick_source").toString()));
    } else {
        std::printf("outcome      waiting: %s\n", qUtf8Printable(outcome.value("reason").toString()));
    }
    return 0;
}

int edge_journal_paper_sim_command(const GlobalOpts& opts, QStringList args) {
    QString symbol;
    QString horizon;
    QString max_age_raw;
    QString amount_raw;
    if (!take_string_option(args, QStringLiteral("--symbol"), symbol) ||
        !take_string_option(args, QStringLiteral("--horizon"), horizon) ||
        !take_string_option(args, QStringLiteral("--max-age-hours"), max_age_raw) ||
        !take_string_option(args, QStringLiteral("--amount-usd"), amount_raw))
        return 2;
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: edge journal paper-sim [--symbol BTC-USD] [--horizon 60s] [--amount-usd N] [--max-age-hours N]\n");
        return 2;
    }
    int max_age_hours = max_age_raw.isEmpty() ? 48 : max_age_raw.toInt();
    double amount_usd = amount_raw.isEmpty() ? 100.0 : amount_raw.toDouble();
    if (max_age_hours < 1 || max_age_hours > 24 * 365 || amount_usd <= 0.0) {
        std::fprintf(stderr, "--max-age-hours must be 1..8760 and --amount-usd must be positive\n");
        return 2;
    }
    QString where = QStringLiteral(" WHERE source='edge crypto-recommend' AND created_at>=?");
    QVariantList params{QDateTime::currentMSecsSinceEpoch() -
                        static_cast<qint64>(max_age_hours) * 3600000LL};
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
    int scanned = 0, buy_trades = 0, resolved = 0, winners = 0, waiting = 0;
    double pnl = 0.0;
    QJsonArray rows;
    auto& q = r.value();
    while (q.next()) {
        ++scanned;
        const QString call = q.value(10).toString();
        const QString side = q.value(9).toString();
        if (!edge_crypto_is_buy_call(call, side))
            continue;
        auto scored = edge_score_crypto_recommendation_outcome(q);
        if (scored.is_err()) {
            ++waiting;
            continue;
        }
        const auto o = scored.value();
        ++buy_trades;
        ++resolved;
        const double trade_pnl = amount_usd * (o.move - o.breakeven);
        pnl += trade_pnl;
        if (trade_pnl > 0.0)
            ++winners;
        if (opts.json)
            rows.append(QJsonObject{{"id", o.id}, {"symbol", o.symbol}, {"move", o.move},
                                    {"breakeven", o.breakeven}, {"pnl", trade_pnl}});
    }
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(QJsonObject{{"scanned", scanned},
                                                       {"buy_trades", buy_trades},
                                                       {"resolved", resolved},
                                                       {"waiting", waiting},
                                                       {"winners", winners},
                                                       {"win_rate", edge_rate(winners, resolved)},
                                                       {"amount_usd", amount_usd},
                                                       {"pnl", pnl},
                                                       {"rows", rows}})
                                .toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("PAPER SIMULATION\n");
    std::printf("amount       $%.2f per BUY CANDIDATE\n", amount_usd);
    std::printf("scanned      %d buy_trades=%d resolved=%d waiting=%d\n", scanned, buy_trades, resolved, waiting);
    std::printf("wins         %d win_rate=%s\n", winners, qUtf8Printable(edge_pct(edge_rate(winners, resolved))));
    std::printf("pnl          $%.4f after estimated cost\n", pnl);
    return 0;
}

int edge_journal_trust_command(const GlobalOpts& opts, QStringList args) {
    QString symbol;
    QString horizon;
    QString max_age_raw;
    if (!take_string_option(args, QStringLiteral("--symbol"), symbol) ||
        !take_string_option(args, QStringLiteral("--horizon"), horizon) ||
        !take_string_option(args, QStringLiteral("--max-age-hours"), max_age_raw))
        return 2;
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: edge journal trust [--symbol BTC-USD] [--horizon 60s] [--max-age-hours N]\n");
        return 2;
    }
    const int max_age_hours = max_age_raw.isEmpty() ? 24 * 7 : max_age_raw.toInt();
    QString where = QStringLiteral(" WHERE source='edge crypto-recommend' AND created_at>=?");
    QVariantList params{QDateTime::currentMSecsSinceEpoch() -
                        static_cast<qint64>(max_age_hours) * 3600000LL};
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
        QStringLiteral("SELECT DISTINCT symbol, horizon FROM edge_decision_journal%1 ORDER BY symbol, horizon").arg(where),
        params);
    if (r.is_err()) {
        std::fprintf(stderr, "%s\n", r.error().c_str());
        return 5;
    }
    QJsonArray rows;
    auto& q = r.value();
    if (!opts.json)
        std::printf("%-9s %-7s %9s %9s %8s %8s %8s %8s %s\n",
                    "SYMBOL", "HZN", "DECISIONS", "RESOLVED", "WIN%", "PFT%", "AVOID%", "TRUST", "STATUS");
    while (q.next()) {
        const auto t = edge_crypto_trust_for_symbol(q.value(0).toString(), q.value(1).toString(), max_age_hours);
        if (opts.json) {
            rows.append(QJsonObject{{"symbol", t.symbol}, {"horizon", t.horizon}, {"decisions", t.decisions},
                                    {"resolved", t.resolved}, {"win_rate", edge_rate(t.wins, t.resolved)},
                                    {"profitable_after_cost_rate", edge_rate(t.buy_wins, t.buy_resolved)},
                                    {"avoid_rate", edge_rate(t.no_buy_wins, t.no_buy_resolved)},
                                    {"trust", t.trust}, {"status", t.status}});
        } else {
            std::printf("%-9s %-7s %9d %9d %8s %8s %8s %7.1f %s\n",
                        qUtf8Printable(t.symbol),
                        qUtf8Printable(t.horizon),
                        t.decisions,
                        t.resolved,
                        qUtf8Printable(t.resolved ? edge_pct(edge_rate(t.wins, t.resolved)) : QStringLiteral("-")),
                        qUtf8Printable(t.buy_resolved ? edge_pct(edge_rate(t.buy_wins, t.buy_resolved)) : QStringLiteral("-")),
                        qUtf8Printable(t.no_buy_resolved ? edge_pct(edge_rate(t.no_buy_wins, t.no_buy_resolved)) : QStringLiteral("-")),
                        t.trust,
                        qUtf8Printable(t.status));
        }
    }
    if (opts.json)
        std::printf("%s\n", QJsonDocument(QJsonObject{{"rows", rows}}).toJson(QJsonDocument::Compact).constData());
    return 0;
}

// EdgeScalpGate lives in EdgeJournalShared.h.

} // namespace openmarketterminal::cli
