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

} // namespace openmarketterminal::cli
