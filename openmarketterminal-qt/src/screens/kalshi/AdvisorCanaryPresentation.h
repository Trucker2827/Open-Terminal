#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace openmarketterminal::screens::kalshi {

struct AdvisorCanaryView {
    QString legacy_badge;
    QString canary_badge;
    QString system;
    QString qualification;
    QString safety;
    QString activity;
    bool legacy_live = false;
    bool canary_live = false;
    bool critical = true;
};

inline qint64 advisor_i64(const QJsonValue& value) {
    return value.isString() ? value.toString().toLongLong()
                            : static_cast<qint64>(value.toDouble());
}

inline QString advisor_join_blockers(const QJsonArray& values) {
    QStringList out;
    for (const auto& value : values) out << value.toString();
    return out.isEmpty() ? QStringLiteral("none") : out.join(QStringLiteral(", "));
}

inline AdvisorCanaryView present_advisor_canary(const QJsonObject& loop,
                                                 const QJsonObject& qualification_snapshot,
                                                 const QJsonObject& promotion,
                                                 const QJsonObject& safety,
                                                 const QJsonObject& canary,
                                                 const QJsonObject& latest,
                                                 const QJsonObject& legacy,
                                                 qint64 now_ms) {
    AdvisorCanaryView view;
    view.legacy_live = legacy.value(QStringLiteral("session_active")).toBool();
    const bool legacy_known = !legacy.isEmpty();
    view.legacy_badge = legacy_known
        ? QStringLiteral("LEGACY LIVE SESSION: %1").arg(view.legacy_live ? QStringLiteral("ARMED")
                                                                        : QStringLiteral("DISARMED"))
        : QStringLiteral("LEGACY LIVE SESSION: UNKNOWN / FAIL CLOSED");

    const QString promotion_state = promotion.value(QStringLiteral("state")).toString();
    view.canary_live = canary.value(QStringLiteral("enabled")).toBool() &&
                       promotion_state == QStringLiteral("CANARY_ENABLED");
    view.canary_badge = QStringLiteral("CODEX CANARY: %1")
        .arg(promotion_state.isEmpty() ? QStringLiteral("UNKNOWN / FAIL CLOSED")
                                      : view.canary_live ? QStringLiteral("ENABLED") : promotion_state);

    const qint64 heartbeat = advisor_i64(loop.value(QStringLiteral("heartbeat_at_ms")));
    const bool heartbeat_fresh = heartbeat > 0 && heartbeat <= now_ms && now_ms - heartbeat <= 180'000;
    const bool journal_valid = loop.value(QStringLiteral("journal_valid")).toBool(false);
    const QString loop_version = loop.value(QStringLiteral("loop_version")).toString(QStringLiteral("unknown"));
    const QJsonObject score = qualification_snapshot.value(QStringLiteral("score")).toObject();
    const QJsonObject filter = score.value(QStringLiteral("filter")).toObject();
    const QString epoch = filter.value(QStringLiteral("forecaster_id")).toString(QStringLiteral("unknown"));
    view.system = QStringLiteral("SUPERVISOR %1 · %2 · PID %3\nFORECASTER %4\nCAPABILITY LOCK %5 · JOURNAL %6 · %7 opportunities")
        .arg(heartbeat_fresh ? QStringLiteral("LIVE") : QStringLiteral("STALE / UNKNOWN"), loop_version)
        .arg(loop.value(QStringLiteral("pid")).toInt())
        .arg(epoch)
        .arg(epoch.contains(QStringLiteral("zero-capability")) ? QStringLiteral("PINNED")
                                                               : QStringLiteral("UNKNOWN / FAIL CLOSED"))
        .arg(journal_valid ? QStringLiteral("VALID") : QStringLiteral("INVALID / UNKNOWN"))
        .arg(loop.value(QStringLiteral("opportunities")).toInt());

    const QJsonObject qual = qualification_snapshot.value(QStringLiteral("qualification")).toObject();
    const QJsonObject metrics = qual.value(QStringLiteral("metrics")).toObject();
    const QJsonObject checks = qual.value(QStringLiteral("checks")).toObject();
    const QJsonObject policy = qual.value(QStringLiteral("policy")).toObject();
    QStringList check_text;
    const QStringList check_names{QStringLiteral("minimum_resolved"), QStringLiteral("daemon_coverage"),
        QStringLiteral("positive_daemon_improvement"), QStringLiteral("positive_market_improvement"),
        QStringLiteral("confidence_interval"), QStringLiteral("net_value_after_fees")};
    for (const QString& name : check_names)
        check_text << QStringLiteral("%1 %2").arg(checks.value(name).toBool() ? QStringLiteral("✓")
                                                                            : QStringLiteral("✗"), name);
    view.qualification = QStringLiteral("QUALIFICATION %1 · %2 / %3 resolved · daemon coverage %4%\n%5\nQualification never enables trading by itself.")
        .arg(qual.value(QStringLiteral("qualified")).toBool() ? QStringLiteral("PASSED") : QStringLiteral("NOT PASSED"))
        .arg(metrics.value(QStringLiteral("resolved")).toInt())
        .arg(policy.value(QStringLiteral("minimum_resolved")).toInt(200))
        .arg(metrics.value(QStringLiteral("daemon_coverage")).toDouble() * 100.0, 0, 'f', 1)
        .arg(check_text.join(QStringLiteral(" · ")));

    const QJsonArray blockers = safety.value(QStringLiteral("blockers")).toArray();
    const QString scope = safety.value(QStringLiteral("drawdown_scope")).toString(QStringLiteral("unknown"));
    view.safety = QStringLiteral("PROMOTION %1 · SAFETY %2\nBLOCKERS: %3\nP&L pending %4 · reconciliation age %5 · daily P&L $%6 · epoch drawdown $%7 · losses %8 · exposure $%9\nCANARY enabled=%10 · epoch=%11 · limits $%12/order, $%13 exposure, $%14 daily loss")
        .arg(promotion_state.isEmpty() ? QStringLiteral("UNKNOWN") : promotion_state,
             safety.value(QStringLiteral("safe")).toBool() ? QStringLiteral("CLEAR") : QStringLiteral("BLOCKED"),
             advisor_join_blockers(blockers))
        .arg(safety.value(QStringLiteral("pnl_reconciliation_pending")).toInt())
        .arg(advisor_i64(safety.value(QStringLiteral("last_reconciled_at_ms"))) > 0
                 ? QStringLiteral("%1s").arg(qMax<qint64>(0, now_ms-advisor_i64(safety.value(QStringLiteral("last_reconciled_at_ms"))))/1000)
                 : QStringLiteral("unknown"))
        .arg(safety.value(QStringLiteral("daily_realized_pnl")).toDouble(),0,'f',2)
        .arg(safety.value(QStringLiteral("maximum_drawdown")).toDouble(),0,'f',2)
        .arg(safety.value(QStringLiteral("consecutive_losses")).toInt())
        .arg(safety.value(QStringLiteral("open_exposure")).toDouble(),0,'f',2)
        .arg(canary.value(QStringLiteral("enabled")).toBool() ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(scope == QStringLiteral("canary_epoch") ? QString::number(advisor_i64(canary.value(QStringLiteral("epoch_started_at_ms"))))
                                                     : QStringLiteral("unknown"))
        .arg(canary.value(QStringLiteral("max_order_dollars")).toDouble(),0,'f',2)
        .arg(canary.value(QStringLiteral("max_open_exposure")).toDouble(),0,'f',2)
        .arg(canary.value(QStringLiteral("daily_loss_limit")).toDouble(),0,'f',2);

    const QJsonObject forecast = latest.value(QStringLiteral("forecast")).toObject();
    const QJsonObject proposal = latest.value(QStringLiteral("proposal")).toObject();
    QString reason = latest.value(QStringLiteral("reason_code")).toString();
    if (reason.isEmpty()) reason = forecast.value(QStringLiteral("reason_code")).toString();
    view.activity = QStringLiteral("LATEST %1 · %2%3\nPROPOSAL %4 %5 @ %6 · NET EDGE %7 · GATE %8")
        .arg(latest.value(QStringLiteral("status")).toString(QStringLiteral("UNKNOWN")),
             latest.value(QStringLiteral("ticker")).toString(QStringLiteral("—")),
             reason.isEmpty() ? QString() : QStringLiteral(" · %1").arg(reason),
             proposal.value(QStringLiteral("side")).toString(QStringLiteral("—")).toUpper(),
             QString::number(proposal.value(QStringLiteral("contracts")).toInt()),
             QString::number(proposal.value(QStringLiteral("limit_price")).toDouble(), 'f', 2),
             QString::number(proposal.value(QStringLiteral("cost_net_edge")).toDouble(), 'f', 4),
             proposal.value(QStringLiteral("gate")).toString(QStringLiteral("—")).toUpper());

    view.critical = !heartbeat_fresh || !journal_valid || promotion_state.isEmpty() ||
                    !safety.value(QStringLiteral("safe")).toBool() || view.canary_live;
    return view;
}

} // namespace openmarketterminal::screens::kalshi
