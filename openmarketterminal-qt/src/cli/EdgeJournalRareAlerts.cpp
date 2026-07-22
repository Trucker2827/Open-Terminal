// edge journal rare-alerts — deliberately compiled as its OWN translation
// unit. Inside CommandDispatch.cpp this function deterministically crashes
// the MSVC front-end (C1001, msc1.cpp:1589) at its position in that ~27k-line
// TU regardless of body contents; see EdgeJournalShared.h. Do not merge it
// back without a green Windows release build proving the compiler recovered.

#include "cli/EdgeJournalShared.h"

#include "storage/sqlite/Database.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

namespace openmarketterminal::cli {

int edge_journal_rare_alerts_command(const GlobalOpts& opts, QStringList args) {
    QString min_edge_raw;
    QString min_conf_raw;
    QString min_trust_raw;
    QString max_age_raw;
    QString provider_raw;
    const bool notify = take_bool_flag(args, QStringLiteral("--notify"));
    if (!take_string_option(args, QStringLiteral("--min-edge-bps"), min_edge_raw) ||
        !take_string_option(args, QStringLiteral("--min-confidence"), min_conf_raw) ||
        !take_string_option(args, QStringLiteral("--min-trust"), min_trust_raw) ||
        !take_string_option(args, QStringLiteral("--max-age-min"), max_age_raw) ||
        !take_string_option(args, QStringLiteral("--provider"), provider_raw))
        return 2;
    if (notify && !require_yes(args, "usage: edge journal rare-alerts --notify --provider P --yes"))
        return 2;
    if (!args.isEmpty()) {
        std::fprintf(stderr, "usage: edge journal rare-alerts [--min-edge-bps N] [--min-confidence P] [--min-trust N] [--max-age-min N] [--notify --provider P --yes]\n");
        return 2;
    }
    double min_edge_bps = min_edge_raw.isEmpty() ? 50.0 : min_edge_raw.toDouble();
    double min_conf = min_conf_raw.isEmpty() ? 0.45 : min_conf_raw.toDouble();
    if (min_conf > 1.0)
        min_conf /= 100.0;
    double min_trust = min_trust_raw.isEmpty() ? 0.0 : min_trust_raw.toDouble();
    int max_age_min = max_age_raw.isEmpty() ? 15 : max_age_raw.toInt();
    if (min_edge_bps < 0.0 || min_conf < 0.0 || min_conf > 1.0 || min_trust < 0.0 || max_age_min < 1 || max_age_min > 1440) {
        std::fprintf(stderr, "invalid rare-alert threshold\n");
        return 2;
    }
    auto r = Database::instance().execute(
        QStringLiteral("SELECT %1 FROM edge_decision_journal"
                       " WHERE source='edge crypto-recommend' AND call='BUY CANDIDATE'"
                       " AND outcome=-1 AND created_at>=? ORDER BY created_at DESC LIMIT 50").arg(edge_journal_cols()),
        {QDateTime::currentMSecsSinceEpoch() - static_cast<qint64>(max_age_min) * 60000LL});
    if (r.is_err()) {
        std::fprintf(stderr, "%s\n", r.error().c_str());
        return 5;
    }
    QJsonArray alerts;
    auto& q = r.value();
    while (q.next()) {
        const double edge_bps = q.value(15).toDouble() * 10000.0;
        const double conf = q.value(20).toDouble();
        const auto trust = edge_crypto_trust_for_symbol(q.value(4).toString(), q.value(5).toString(), 24 * 7);
        if (edge_bps < min_edge_bps || conf < min_conf || trust.trust < min_trust)
            continue;
        QJsonObject alert;
        alert.insert(QStringLiteral("id"), q.value(0).toString());
        alert.insert(QStringLiteral("symbol"), q.value(4).toString());
        alert.insert(QStringLiteral("horizon"), q.value(5).toString());
        alert.insert(QStringLiteral("edge_bps"), edge_bps);
        alert.insert(QStringLiteral("confidence"), conf);
        alert.insert(QStringLiteral("trust"), trust.trust);
        alert.insert(QStringLiteral("time"), edge_time_text(q.value(1).toLongLong()));
        alert.insert(QStringLiteral("reason"), q.value(25).toString());
        alerts.append(alert);
    }
    if (notify && !alerts.isEmpty()) {
        int code = 0;
        if (!init_headless_for_cli(opts, code))
            return code;
        auto& svc = notifications::NotificationService::instance();
        auto* provider = provider_raw.trimmed().isEmpty() ? nullptr : svc.provider(provider_raw.trimmed().toLower());
        notifications::NotificationRequest req;
        req.title = QStringLiteral("OpenTerminal rare crypto edge");
        req.message = QStringLiteral("%1 fresh BUY candidate(s) passed edge/trust gates").arg(alerts.size());
        req.level = notifications::NotifLevel::Alert;
        req.trigger = notifications::NotifTrigger::Manual;
        bool ok = false;
        QString error;
        if (provider)
            notify_wait_send(provider, req, 15000, ok, error);
        else
            error = QStringLiteral("provider required");
        if (!ok)
            std::fprintf(stderr, "notification failed: %s\n", qUtf8Printable(error));
    }
    if (opts.json) {
        QJsonObject out;
        out.insert(QStringLiteral("alerts"), alerts);
        std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    if (alerts.isEmpty()) {
        std::printf("rare alerts  none\n");
        return 0;
    }
    std::printf("%-20s %-9s %-7s %-9s %-8s %-7s %s\n", "TIME", "SYMBOL", "HZN", "EDGE", "CONF", "TRUST", "ID");
    for (const auto& v : alerts) {
        const auto a = v.toObject();
        const QByteArray c_time = a.value("time").toString().left(19).toUtf8();
        const QByteArray c_sym = a.value("symbol").toString().toUtf8();
        const QByteArray c_hzn = a.value("horizon").toString().toUtf8();
        const QByteArray c_conf = edge_pct(a.value("confidence").toDouble()).toUtf8();
        const QByteArray c_id = a.value("id").toString().toUtf8();
        const double edge_bps = a.value("edge_bps").toDouble();
        const double trust = a.value("trust").toDouble();
        std::printf("%-20s %-9s %-7s %8.1fbps %-8s %7.1f %s\n", c_time.constData(), c_sym.constData(),
                    c_hzn.constData(), edge_bps, c_conf.constData(), trust, c_id.constData());
    }
    return 0;
}

} // namespace openmarketterminal::cli
