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

int edge_journal_list_command(const GlobalOpts& opts, QStringList args) {
    QString symbol;
    QString horizon;
    QString limit_raw;
    if (!take_string_option(args, QStringLiteral("--symbol"), symbol) ||
        !take_string_option(args, QStringLiteral("--horizon"), horizon) ||
        !take_string_option(args, QStringLiteral("--limit"), limit_raw))
        return 2;
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: edge journal list [--symbol BTC] [--horizon 5m] [--limit N]\n");
        return 2;
    }
    int limit = 20;
    if (!limit_raw.isEmpty()) {
        bool ok = false;
        limit = limit_raw.toInt(&ok);
        if (!ok || limit < 1 || limit > 500) {
            std::fprintf(stderr, "--limit must be 1..500\n");
            return 2;
        }
    }
    QString sql = QStringLiteral("SELECT %1 FROM edge_decision_journal WHERE 1=1").arg(edge_journal_cols());
    QVariantList params;
    if (!symbol.trimmed().isEmpty()) {
        sql += QStringLiteral(" AND symbol=?");
        params << symbol.trimmed().toUpper();
    }
    if (!horizon.trimmed().isEmpty() && horizon.trimmed().toLower() != QStringLiteral("all")) {
        sql += QStringLiteral(" AND horizon=?");
        params << horizon.trimmed().toLower();
    }
    sql += QStringLiteral(" ORDER BY created_at DESC LIMIT ?");
    params << limit;
    auto r = Database::instance().execute(sql, params);
    if (r.is_err()) {
        std::fprintf(stderr, "%s\n", r.error().c_str());
        return 5;
    }
    auto& q = r.value();
    QJsonArray arr;
    if (opts.json) {
        while (q.next())
            arr.append(edge_journal_row_to_json(q));
        std::printf("%s\n", QJsonDocument(QJsonObject{{"decisions", arr}})
                                .toJson(QJsonDocument::Compact)
                                .constData());
        return 0;
    }
    std::printf("%-20s %-4s %-7s %-10s %-16s %-8s %-8s %-8s %-7s %s\n",
                "time", "sym", "horizon", "side", "call", "market", "model", "net", "outcome", "id");
    while (q.next()) {
        std::printf("%-20s %-4s %-7s %-10s %-16s %-8s %-8s %-8s %-7s %s\n",
                    qUtf8Printable(edge_time_text(q.value(1).toLongLong()).mid(0, 19)),
                    qUtf8Printable(q.value(4).toString()),
                    qUtf8Printable(q.value(5).toString()),
                    qUtf8Printable(elide_text(q.value(9).toString(), 10)),
                    qUtf8Printable(elide_text(q.value(10).toString(), 16)),
                    qUtf8Printable(edge_pct(q.value(12).toDouble())),
                    qUtf8Printable(edge_pct(q.value(13).toDouble())),
                    qUtf8Printable(edge_pct(q.value(15).toDouble())),
                    qUtf8Printable(edge_outcome_text(q.value(26).toInt())),
                    qUtf8Printable(q.value(0).toString()));
    }
    return 0;
}

int edge_journal_show_command(const GlobalOpts& opts, QStringList args) {
    if (args.size() != 1) {
        std::fprintf(stderr, "usage: edge journal show <id>\n");
        return 2;
    }
    auto r = Database::instance().execute(
        QStringLiteral("SELECT %1 FROM edge_decision_journal WHERE id=?").arg(edge_journal_cols()),
        {args.first()});
    if (r.is_err()) {
        std::fprintf(stderr, "%s\n", r.error().c_str());
        return 5;
    }
    auto& q = r.value();
    if (!q.next()) {
        std::fprintf(stderr, "decision not found\n");
        return 4;
    }
    const QJsonObject obj = edge_journal_row_to_json(q);
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(obj).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("id           %s\n", qUtf8Printable(obj.value("id").toString()));
    std::printf("time         %s\n", qUtf8Printable(obj.value("created_at").toString()));
    std::printf("market       %s %s %s\n",
                qUtf8Printable(obj.value("venue").toString()),
                qUtf8Printable(obj.value("symbol").toString()),
                qUtf8Printable(obj.value("horizon").toString()));
    std::printf("question     %s\n", qUtf8Printable(obj.value("question").toString()));
    std::printf("call         %s gate=%s outcome=%s\n",
                qUtf8Printable(obj.value("call").toString()),
                qUtf8Printable(obj.value("gate").toString()),
                qUtf8Printable(obj.value("outcome").toString()));
    std::printf("probability  market=%s model=%s confidence=%.2f\n",
                qUtf8Printable(edge_pct(obj.value("market_probability").toDouble())),
                qUtf8Printable(edge_pct(obj.value("model_probability").toDouble())),
                obj.value("confidence").toDouble());
    std::printf("edge         raw=%s net=%s gate=%s\n",
                qUtf8Printable(edge_pct(obj.value("raw_edge").toDouble())),
                qUtf8Printable(edge_pct(obj.value("edge_after_cost").toDouble())),
                qUtf8Printable(edge_pct(obj.value("gate_edge").toDouble())));
    std::printf("reason       %s\n", qUtf8Printable(obj.value("reasons").toString()));
    return 0;
}

int edge_journal_resolve_command(const GlobalOpts&, QStringList args) {
    QString outcome_raw;
    if (!take_string_option(args, QStringLiteral("--outcome"), outcome_raw))
        return 2;
    if (outcome_raw.isEmpty() && args.size() >= 2)
        outcome_raw = args.takeAt(1);
    if (args.size() != 1 || outcome_raw.isEmpty()) {
        std::fprintf(stderr, "usage: edge journal resolve <id> --outcome win|loss|pending\n");
        return 2;
    }
    const int outcome = edge_parse_outcome(outcome_raw);
    if (outcome < -1) {
        std::fprintf(stderr, "--outcome must be win, loss, or pending\n");
        return 2;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto r = Database::instance().execute(
        "UPDATE edge_decision_journal SET outcome=?, resolved_at=?, updated_at=? WHERE id=?",
        {outcome, outcome >= 0 ? now : 0, now, args.first()});
    if (r.is_err()) {
        std::fprintf(stderr, "%s\n", r.error().c_str());
        return 5;
    }
    std::printf("resolved     %s outcome=%s\n",
                qUtf8Printable(args.first()),
                qUtf8Printable(edge_outcome_text(outcome)));
    return 0;
}

int edge_journal_stats_command(const GlobalOpts& opts, QStringList args) {
    QString symbol;
    QString horizon;
    if (!take_string_option(args, QStringLiteral("--symbol"), symbol) ||
        !take_string_option(args, QStringLiteral("--horizon"), horizon))
        return 2;
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: edge journal stats [--symbol BTC] [--horizon 5m]\n");
        return 2;
    }
    QString where = QStringLiteral(" WHERE 1=1");
    QVariantList params;
    if (!symbol.trimmed().isEmpty()) {
        where += QStringLiteral(" AND symbol=?");
        params << symbol.trimmed().toUpper();
    }
    if (!horizon.trimmed().isEmpty() && horizon.trimmed().toLower() != QStringLiteral("all")) {
        where += QStringLiteral(" AND horizon=?");
        params << horizon.trimmed().toLower();
    }
    auto r = Database::instance().execute(
        QStringLiteral("SELECT COUNT(*), SUM(CASE WHEN outcome IN (0,1) THEN 1 ELSE 0 END),"
                       " SUM(CASE WHEN outcome=1 THEN 1 ELSE 0 END), AVG(edge_after_cost),"
                       " AVG(confidence), SUM(CASE WHEN data_status='trade_candidate' THEN 1 ELSE 0 END)"
                       " FROM edge_decision_journal%1").arg(where),
        params);
    if (r.is_err()) {
        std::fprintf(stderr, "%s\n", r.error().c_str());
        return 5;
    }
    auto& q = r.value();
    q.next();
    const int total = q.value(0).toInt();
    const int resolved = q.value(1).toInt();
    const int wins = q.value(2).toInt();
    const double win_rate = resolved > 0 ? static_cast<double>(wins) / resolved : 0.0;
    const double avg_edge = q.value(3).toDouble();
    const double avg_confidence = q.value(4).toDouble();
    const int candidates = q.value(5).toInt();
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(QJsonObject{{"total", total},
                                                       {"resolved", resolved},
                                                       {"wins", wins},
                                                       {"win_rate", win_rate},
                                                       {"average_edge_after_cost", avg_edge},
                                                       {"average_confidence", avg_confidence},
                                                       {"trade_candidates", candidates}})
                                .toJson(QJsonDocument::Compact)
                                .constData());
        return 0;
    }
    std::printf("decisions    %d\n", total);
    std::printf("resolved     %d wins=%d win_rate=%s\n", resolved, wins, qUtf8Printable(edge_pct(win_rate)));
    std::printf("candidates   %d\n", candidates);
    std::printf("averages     net_edge=%s confidence=%.2f\n", qUtf8Printable(edge_pct(avg_edge)), avg_confidence);
    return 0;
}

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

static QString edge_crypto_regime_from_features(const QJsonObject& features) {
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

static QJsonObject edge_crypto_cost_json(const QJsonObject& features) {
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

struct EdgeScalpGate {
    QString symbol;
    QString venue;
    QString horizon;
    QString verdict;
    QString action;
    QString journal_id;
    QString rationale;
    QStringList blockers;
    CryptoRecommendationDecision decision;
    services::edge_radar::CryptoMicrostructureSnapshot snapshot;
    EdgeCryptoTrust trust;
    double expected_capture_bps = 0.0;
    double observed_move_bps = 0.0;
    double round_trip_cost_bps = 0.0;
    double net_after_cost_bps = 0.0;
    double fee_bps = 0.0;
    double slippage_bps = 0.0;
    double safety_bps = 0.0;
    double capture_ratio = 0.0;
    double min_net_bps = 0.0;
    int horizon_sec = 0;
};

static services::edge_radar::CryptoMicrostructureWindow
edge_scalp_primary_window(const services::edge_radar::CryptoMicrostructureSnapshot& snapshot,
                          int horizon_sec) {
    services::edge_radar::CryptoMicrostructureWindow best;
    const int preferred = horizon_sec <= 10 ? 5 : (horizon_sec <= 30 ? 15 : 60);
    for (const auto& w : snapshot.windows) {
        if (w.available && w.seconds == preferred)
            return w;
    }
    for (const auto& w : snapshot.windows) {
        if (!w.available)
            continue;
        if (!best.available || std::abs(w.move_pct) > std::abs(best.move_pct))
            best = w;
    }
    return best;
}

static QJsonObject edge_scalp_gate_json(const EdgeScalpGate& gate) {
    return QJsonObject{{"symbol", gate.symbol},
                       {"venue", gate.venue},
                       {"horizon", gate.horizon},
                       {"verdict", gate.verdict},
                       {"action", gate.action},
                       {"journal_id", gate.journal_id},
                       {"direction", gate.decision.direction},
                       {"call", gate.decision.call},
                       {"reference_price", gate.decision.reference_price},
                       {"confidence", gate.decision.confidence},
                       {"expected_capture_bps", gate.expected_capture_bps},
                       {"observed_move_bps", gate.observed_move_bps},
                       {"round_trip_cost_bps", gate.round_trip_cost_bps},
                       {"net_after_cost_bps", gate.net_after_cost_bps},
                       {"fee_bps", gate.fee_bps},
                       {"slippage_bps", gate.slippage_bps},
                       {"safety_bps", gate.safety_bps},
                       {"capture_ratio", gate.capture_ratio},
                       {"min_net_bps", gate.min_net_bps},
                       {"trust", QJsonObject{{"score", gate.trust.trust},
                                             {"status", gate.trust.status},
                                             {"resolved", gate.trust.resolved},
                                             {"decisions", gate.trust.decisions},
                                             {"win_rate", edge_rate(gate.trust.wins, gate.trust.resolved)},
                                             {"profitable_buy_rate", edge_rate(gate.trust.buy_wins, gate.trust.buy_resolved)}}},
                       {"blockers", QJsonArray::fromStringList(gate.blockers)},
                       {"rationale", gate.rationale},
                       {"microstructure", services::edge_radar::CryptoMicrostructureRadar::to_json(gate.snapshot)}};
}

static int edge_emit_scalp_gate(const GlobalOpts& opts, const EdgeScalpGate& gate) {
    if (opts.json) {
        std::printf("%s\n", QJsonDocument(edge_scalp_gate_json(gate)).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    std::printf("SCALP / INTRADAY GATE\n");
    std::printf("verdict      %s  action=%s\n", qUtf8Printable(gate.verdict), qUtf8Printable(gate.action));
    std::printf("symbol       %s venue=%s horizon=%s journal=%s\n",
                qUtf8Printable(gate.symbol),
                qUtf8Printable(gate.venue),
                qUtf8Printable(gate.horizon),
                qUtf8Printable(gate.journal_id.isEmpty() ? QStringLiteral("-") : gate.journal_id));
    std::printf("price        %s direction=%s confidence=%s freshest=%s age=%lldms live_sources=%d\n",
                qUtf8Printable(edge_price_or_dash(gate.decision.reference_price)),
                qUtf8Printable(gate.decision.direction),
                qUtf8Printable(edge_pct(gate.decision.confidence)),
                qUtf8Printable(gate.snapshot.freshest_source.isEmpty() ? QStringLiteral("-") : gate.snapshot.freshest_source),
                static_cast<long long>(gate.snapshot.freshest_age_ms),
                gate.snapshot.live_sources);
    std::printf("breakeven    round_trip=%.2fbps fee=%.2fbps slippage=%.2fbps spread=%.2fbps safety=%.2fbps\n",
                gate.round_trip_cost_bps,
                gate.fee_bps * 2.0,
                gate.slippage_bps,
                gate.snapshot.cross_source_spread_bps,
                gate.safety_bps);
    std::printf("move         observed=%.2fbps capture=%.2fbps net=%.2fbps min_net=%.2fbps capture_ratio=%.2f\n",
                gate.observed_move_bps,
                gate.expected_capture_bps,
                gate.net_after_cost_bps,
                gate.min_net_bps,
                gate.capture_ratio);
    std::printf("trust        score=%.1f status=%s resolved=%d buy_profit_rate=%s\n",
                gate.trust.trust,
                qUtf8Printable(gate.trust.status.isEmpty() ? QStringLiteral("none") : gate.trust.status),
                gate.trust.resolved,
                qUtf8Printable(gate.trust.buy_resolved ? edge_pct(edge_rate(gate.trust.buy_wins, gate.trust.buy_resolved))
                                                        : QStringLiteral("-")));
    std::printf("flow         tick_direction=%+.2f top_book_imbalance=%+.2f book_sources=%d ticks=%d divergence=%.2fbps\n",
                gate.snapshot.tape_pressure,
                gate.snapshot.book_pressure,
                gate.snapshot.top_book_sources,
                gate.snapshot.tick_count,
                gate.snapshot.cross_source_spread_bps);
    if (!gate.blockers.isEmpty())
        std::printf("blockers     %s\n", qUtf8Printable(gate.blockers.join(QStringLiteral("; "))));
    std::printf("rationale    %s\n", qUtf8Printable(gate.rationale));
    std::printf("action       no order placed; use this as a pre-trade gate, then score the journal later\n");
    return 0;
}

int edge_scalp_gate_command(const GlobalOpts& opts, QStringList args) {
    QString duration_raw;
    QString sources_raw;
    QString venue_raw;
    QString horizon_raw;
    QString fee_raw;
    QString slippage_raw;
    QString safety_raw;
    QString min_net_raw;
    QString capture_raw;
    QString max_spread_raw;
    QString max_age_raw;
    QString min_sources_raw;
    QString min_samples_raw;
    QString min_trust_raw;
    QString interval_raw;
    QString iterations_raw;
    const bool maker = take_bool_flag(args, QStringLiteral("--maker")) ||
                       take_bool_flag(args, QStringLiteral("--post-only"));
    const bool allow_warmup = take_bool_flag(args, QStringLiteral("--allow-warmup"));
    const bool no_journal = take_bool_flag(args, QStringLiteral("--no-journal"));
    const bool watch = take_bool_flag(args, QStringLiteral("--watch"));
    if (!take_string_option(args, QStringLiteral("--duration-ms"), duration_raw) ||
        !take_string_option(args, QStringLiteral("--sources"), sources_raw) ||
        !take_string_option(args, QStringLiteral("--venue"), venue_raw) ||
        !take_string_option(args, QStringLiteral("--horizon-sec"), horizon_raw) ||
        !take_string_option(args, QStringLiteral("--fee-bps"), fee_raw) ||
        !take_string_option(args, QStringLiteral("--slippage-bps"), slippage_raw) ||
        !take_string_option(args, QStringLiteral("--safety-bps"), safety_raw) ||
        !take_string_option(args, QStringLiteral("--min-net-bps"), min_net_raw) ||
        !take_string_option(args, QStringLiteral("--capture-ratio"), capture_raw) ||
        !take_string_option(args, QStringLiteral("--max-spread-bps"), max_spread_raw) ||
        !take_string_option(args, QStringLiteral("--max-age-ms"), max_age_raw) ||
        !take_string_option(args, QStringLiteral("--min-live-sources"), min_sources_raw) ||
        !take_string_option(args, QStringLiteral("--min-samples"), min_samples_raw) ||
        !take_string_option(args, QStringLiteral("--min-trust"), min_trust_raw) ||
        !take_string_option(args, QStringLiteral("--interval-sec"), interval_raw) ||
        !take_string_option(args, QStringLiteral("--iterations"), iterations_raw))
        return 2;

    int duration_ms = 9000;
    if (!edge_parse_duration_ms(duration_raw, duration_ms))
        return 2;
    int horizon_sec = 15;
    if (!horizon_raw.isEmpty()) {
        bool ok = false;
        horizon_sec = horizon_raw.toInt(&ok);
        if (!ok || horizon_sec < 5 || horizon_sec > 3600) {
            std::fprintf(stderr, "--horizon-sec must be 5..3600 for scalp-gate\n");
            return 2;
        }
    }
    double capture_ratio = capture_raw.isEmpty() ? 0.35 : capture_raw.toDouble();
    if (capture_ratio > 1.0)
        capture_ratio /= 100.0;
    double max_spread_bps = max_spread_raw.isEmpty() ? 8.0 : max_spread_raw.toDouble();
    double max_age_ms = max_age_raw.isEmpty() ? 2500.0 : max_age_raw.toDouble();
    int min_live_sources = min_sources_raw.isEmpty() ? 2 : min_sources_raw.toInt();
    int min_samples = min_samples_raw.isEmpty() ? 30 : min_samples_raw.toInt();
    double min_trust = min_trust_raw.isEmpty() ? 45.0 : min_trust_raw.toDouble();
    double safety_bps = safety_raw.isEmpty() ? 5.0 : safety_raw.toDouble();
    double min_net_bps = min_net_raw.isEmpty() ? 5.0 : min_net_raw.toDouble();
    int interval_sec = interval_raw.isEmpty() ? 5 : interval_raw.toInt();
    int iterations = iterations_raw.isEmpty() ? (watch ? 0 : 1) : iterations_raw.toInt();
    if (capture_ratio <= 0.0 || capture_ratio > 1.0 || max_spread_bps < 0.0 || max_age_ms < 100.0 ||
        min_live_sources < 1 || min_live_sources > 10 || min_samples < 0 || min_samples > 10000 ||
        min_trust < 0.0 || min_trust > 100.0 || safety_bps < 0.0 || min_net_bps < 0.0 ||
        interval_sec < 1 || interval_sec > 3600 || iterations < 0 || iterations > 100000) {
        std::fprintf(stderr, "invalid scalp-gate threshold\n");
        return 2;
    }

    const QString symbol_raw = args.isEmpty() ? QStringLiteral("BTC-USD") : args.takeFirst();
    if (!args.isEmpty()) {
        std::fprintf(stderr, "unknown args: %s\n", qUtf8Printable(args.join(' ')));
        return 2;
    }

    const QString venue = venue_raw.trimmed().isEmpty() ? QStringLiteral("coinbase") : venue_raw.trimmed();
    const CryptoFeeProfile fee_profile = crypto_fee_profile_for_venue(venue);
    double fee_bps = maker ? fee_profile.maker_bps : fee_profile.taker_bps;
    double slippage_bps = fee_profile.slippage_bps;
    if (!edge_parse_bps_text(fee_raw, fee_bps, "--fee-bps") ||
        !edge_parse_bps_text(slippage_raw, slippage_bps, "--slippage-bps"))
        return 2;

    int code = 0;
    if (!init_headless_for_cli(opts, code))
        return code;

    const QString symbol = services::crypto_latency::CryptoLatencyService::normalize_symbol(symbol_raw);
    auto build_gate = [&]() -> Result<EdgeScalpGate> {
        EdgeScalpGate gate;
        gate.symbol = symbol;
        gate.venue = crypto_fee_venue_key(venue);
        gate.horizon_sec = horizon_sec;
        gate.horizon = QStringLiteral("%1s").arg(horizon_sec);
        gate.fee_bps = fee_bps;
        gate.slippage_bps = slippage_bps;
        gate.safety_bps = safety_bps;
        gate.capture_ratio = capture_ratio;
        gate.min_net_bps = min_net_bps;
        gate.snapshot = edge_capture_microstructure(gate.symbol,
                                                    edge_safe_latency_sources_for_symbol(sources_raw, gate.symbol),
                                                    duration_ms,
                                                    true);
        gate.decision = edge_score_crypto_recommendation(gate.symbol, venue, gate.snapshot,
                                                         horizon_sec, fee_bps * 2.0 + safety_bps,
                                                         slippage_bps, min_net_bps);
        gate.trust = edge_crypto_trust_for_symbol(gate.symbol, gate.horizon, 24 * 14);

        const auto primary = edge_scalp_primary_window(gate.snapshot, horizon_sec);
        gate.observed_move_bps = primary.available ? primary.move_pct * 100.0 : 0.0;
        const double directional_move_bps = gate.decision.direction == QLatin1String("up")
                                                ? std::max(0.0, gate.observed_move_bps)
                                                : 0.0;
        gate.expected_capture_bps = directional_move_bps * gate.capture_ratio * gate.decision.confidence;
        gate.round_trip_cost_bps = (fee_bps * 2.0) + slippage_bps +
                                   std::max(0.0, gate.snapshot.cross_source_spread_bps) + safety_bps;
        gate.net_after_cost_bps = gate.expected_capture_bps - gate.round_trip_cost_bps;

        if (gate.snapshot.freshest_age_ms < 0 || gate.snapshot.freshest_age_ms > static_cast<qint64>(max_age_ms))
            gate.blockers << QStringLiteral("freshest tick too stale");
        if (gate.snapshot.live_sources < min_live_sources)
            gate.blockers << QStringLiteral("not enough live sources");
        if (gate.snapshot.cross_source_spread_bps > max_spread_bps)
            gate.blockers << QStringLiteral("spread/divergence too wide");
        if (!primary.available)
            gate.blockers << QStringLiteral("not enough window history");
        if (gate.decision.direction != QLatin1String("up"))
            gate.blockers << QStringLiteral("spot scalp only supports long/up candidates");
        if (gate.net_after_cost_bps < min_net_bps)
            gate.blockers << QStringLiteral("estimated captured move does not clear round-trip cost");
        if (!allow_warmup && gate.trust.resolved < min_samples)
            gate.blockers << QStringLiteral("not enough resolved local samples");
        if (!allow_warmup && gate.trust.trust < min_trust)
            gate.blockers << QStringLiteral("local trust score below gate");

        if (gate.blockers.isEmpty()) {
            gate.verdict = QStringLiteral("TRADE CANDIDATE");
            gate.action = QStringLiteral("LIMIT_BUY_ONLY");
        } else if (gate.decision.direction == QLatin1String("up") && gate.snapshot.live_sources >= min_live_sources &&
                   gate.snapshot.freshest_age_ms >= 0 && gate.snapshot.freshest_age_ms <= static_cast<qint64>(max_age_ms)) {
            gate.verdict = QStringLiteral("WATCH");
            gate.action = QStringLiteral("NO_ORDER");
        } else {
            gate.verdict = QStringLiteral("NO TRADE");
            gate.action = QStringLiteral("NO_ORDER");
        }
        gate.rationale = QStringLiteral("scalp gate: expected capture %1bps vs round-trip %2bps; trust %3/%4 resolved")
                             .arg(gate.expected_capture_bps, 0, 'f', 2)
                             .arg(gate.round_trip_cost_bps, 0, 'f', 2)
                             .arg(gate.trust.trust, 0, 'f', 1)
                             .arg(gate.trust.resolved);

        if (!no_journal) {
            auto inserted = edge_journal_insert_crypto_recommendation(gate.decision, gate.snapshot,
                                                                      horizon_sec, fee_bps * 2.0 + safety_bps,
                                                                      slippage_bps, min_net_bps);
            if (inserted.is_err())
                return Result<EdgeScalpGate>::err(inserted.error());
            gate.journal_id = inserted.value();
        }
        return Result<EdgeScalpGate>::ok(gate);
    };

    if (opts.json && iterations != 1) {
        QJsonArray rows;
        int rendered = 0;
        while (iterations == 0 || rendered < iterations) {
            auto r = build_gate();
            if (r.is_err()) {
                std::fprintf(stderr, "failed to journal scalp gate: %s\n", r.error().c_str());
                return 5;
            }
            rows.append(edge_scalp_gate_json(r.value()));
            ++rendered;
            if (iterations > 0 && rendered >= iterations)
                break;
            QThread::sleep(static_cast<unsigned long>(interval_sec));
        }
        std::printf("%s\n", QJsonDocument(QJsonObject{{"rows", rows}}).toJson(QJsonDocument::Compact).constData());
        return 0;
    }

    int rendered = 0;
    while (iterations == 0 || rendered < iterations) {
        if (watch && !opts.json)
            std::printf("\033[2J\033[H");
        auto r = build_gate();
        if (r.is_err()) {
            std::fprintf(stderr, "failed to journal scalp gate: %s\n", r.error().c_str());
            return 5;
        }
        const int rc = edge_emit_scalp_gate(opts, r.value());
        std::fflush(stdout);
        if (rc != 0 || opts.json)
            return rc;
        ++rendered;
        if (iterations > 0 && rendered >= iterations)
            break;
        if (watch)
            std::printf("\nsession      refreshing every %ds; press Ctrl-C to stop\n", interval_sec);
        QThread::sleep(static_cast<unsigned long>(interval_sec));
    }
    return 0;
}

int edge_journal_regimes_command(const GlobalOpts& opts, QStringList args) {
    QString symbol;
    QString horizon;
    QString max_age_raw;
    if (!take_string_option(args, QStringLiteral("--symbol"), symbol) ||
        !take_string_option(args, QStringLiteral("--horizon"), horizon) ||
        !take_string_option(args, QStringLiteral("--max-age-hours"), max_age_raw))
        return 2;
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: edge journal regimes [--symbol BTC-USD] [--horizon 60s] [--max-age-hours N]\n");
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
        QStringLiteral("SELECT %1 FROM edge_decision_journal%2 ORDER BY created_at DESC")
            .arg(edge_journal_cols(), where),
        params);
    if (r.is_err()) {
        std::fprintf(stderr, "%s\n", r.error().c_str());
        return 5;
    }
    struct RegimeStats { int total = 0; int resolved = 0; int wins = 0; int buys = 0; double edge = 0.0; };
    QMap<QString, RegimeStats> stats;
    auto& q = r.value();
    while (q.next()) {
        const QJsonObject features = QJsonDocument::fromJson(q.value(24).toString().toUtf8()).object();
        const QString regime = edge_crypto_regime_from_features(features);
        auto s = stats.value(regime);
        ++s.total;
        s.edge += q.value(15).toDouble();
        if (q.value(26).toInt() >= 0) {
            ++s.resolved;
            if (q.value(26).toInt() == 1)
                ++s.wins;
        }
        if (edge_crypto_is_buy_call(q.value(10).toString(), q.value(9).toString()))
            ++s.buys;
        stats.insert(regime, s);
    }
    QJsonArray rows;
    if (!opts.json)
        std::printf("%-14s %9s %9s %8s %7s %9s\n", "REGIME", "DECISIONS", "RESOLVED", "WIN%", "BUYS", "AVG_NET");
    for (auto it = stats.cbegin(); it != stats.cend(); ++it) {
        const auto s = it.value();
        const double avg_edge = s.total > 0 ? s.edge / s.total : 0.0;
        if (opts.json) {
            rows.append(QJsonObject{{"regime", it.key()}, {"decisions", s.total}, {"resolved", s.resolved},
                                    {"wins", s.wins}, {"win_rate", edge_rate(s.wins, s.resolved)},
                                    {"buy_candidates", s.buys}, {"average_edge_after_cost", avg_edge}});
        } else {
            std::printf("%-14s %9d %9d %8s %7d %9s\n",
                        qUtf8Printable(it.key()), s.total, s.resolved,
                        qUtf8Printable(s.resolved ? edge_pct(edge_rate(s.wins, s.resolved)) : QStringLiteral("-")),
                        s.buys, qUtf8Printable(edge_pct(avg_edge)));
        }
    }
    if (opts.json)
        std::printf("%s\n", QJsonDocument(QJsonObject{{"rows", rows}}).toJson(QJsonDocument::Compact).constData());
    return 0;
}

int edge_journal_no_trade_command(const GlobalOpts& opts, QStringList args) {
    QString limit_raw;
    QString max_age_raw;
    if (!take_string_option(args, QStringLiteral("--limit"), limit_raw) ||
        !take_string_option(args, QStringLiteral("--max-age-hours"), max_age_raw))
        return 2;
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: edge journal no-trade [--limit N] [--max-age-hours N]\n");
        return 2;
    }
    int limit = limit_raw.isEmpty() ? 20 : limit_raw.toInt();
    int max_age_hours = max_age_raw.isEmpty() ? 24 : max_age_raw.toInt();
    if (limit < 1 || limit > 200 || max_age_hours < 1 || max_age_hours > 8760) {
        std::fprintf(stderr, "--limit must be 1..200 and --max-age-hours 1..8760\n");
        return 2;
    }
    auto r = Database::instance().execute(
        QStringLiteral("SELECT %1 FROM edge_decision_journal"
                       " WHERE source='edge crypto-recommend'"
                       " AND call!='BUY CANDIDATE' AND created_at>=?"
                       " ORDER BY created_at DESC LIMIT ?").arg(edge_journal_cols()),
        {QDateTime::currentMSecsSinceEpoch() - static_cast<qint64>(max_age_hours) * 3600000LL, limit});
    if (r.is_err()) {
        std::fprintf(stderr, "%s\n", r.error().c_str());
        return 5;
    }
    QJsonArray rows;
    auto& q = r.value();
    if (!opts.json)
        std::printf("%-20s %-9s %-14s %-8s %-8s %s\n", "TIME", "SYMBOL", "CALL", "CONF", "NET", "REASON");
    while (q.next()) {
        if (opts.json) {
            rows.append(edge_journal_row_to_json(q));
        } else {
            // Hoist qUtf8Printable temporaries into named QByteArrays: MSVC C1001
            // ICEs on this multi-arg printf-of-nested-temporaries in the release
            // (/O2 unity) build; naming the temporaries dodges the optimizer bug.
            const QByteArray c_time = edge_time_text(q.value(1).toLongLong()).left(19).toUtf8();
            const QByteArray c_sym = q.value(4).toString().toUtf8();
            const QByteArray c_call = elide_text(q.value(10).toString(), 14).toUtf8();
            const QByteArray c_conf = edge_pct(q.value(20).toDouble()).toUtf8();
            const QByteArray c_net = edge_pct(q.value(15).toDouble()).toUtf8();
            const QByteArray c_reason = elide_text(q.value(25).toString(), 110).toUtf8();
            std::printf("%-20s %-9s %-14s %-8s %-8s %s\n", c_time.constData(), c_sym.constData(),
                        c_call.constData(), c_conf.constData(), c_net.constData(), c_reason.constData());
        }
    }
    if (opts.json)
        std::printf("%s\n", QJsonDocument(QJsonObject{{"rows", rows}}).toJson(QJsonDocument::Compact).constData());
    return 0;
}

} // namespace openmarketterminal::cli
