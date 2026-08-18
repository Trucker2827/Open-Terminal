#pragma once

#include "services/prediction/kalshi/KalshiBotDecision.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>

namespace openmarketterminal::services::prediction::kalshi_ns {

inline QJsonObject research_status_read(const QString& evidence_dir, const QString& name) {
    QFile file(QDir(evidence_dir).filePath(name));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

inline qint64 research_status_updated_ms(const QJsonObject& object) {
    for (const QString& key : {QStringLiteral("updated_at_ms"), QStringLiteral("generated_at_ms"),
                               QStringLiteral("frozen_at_ms")}) {
        const qint64 value = object.value(key).toVariant().toLongLong();
        if (value > 0) return value;
    }
    for (const QString& key : {QStringLiteral("updated_at"), QStringLiteral("generated_at")}) {
        const QDateTime value = QDateTime::fromString(object.value(key).toString(), Qt::ISODateWithMs);
        if (value.isValid()) return value.toMSecsSinceEpoch();
    }
    return 0;
}

inline QJsonObject research_paper_ledger(const QJsonObject& state, qint64 now_ms) {
    QJsonObject out;
    const QJsonObject embedded = state.value(QStringLiteral("summary")).toObject();
    const QJsonObject source = embedded.isEmpty() ? state : embedded;
    int completed = source.value(QStringLiteral("completed")).toInt();
    int open = source.value(QStringLiteral("open")).toInt();
    int wins = source.value(QStringLiteral("wins")).toInt();
    double pnl = source.value(QStringLiteral("net_pnl")).toDouble();
    double drawdown = source.value(QStringLiteral("max_drawdown")).toDouble();
    if (embedded.isEmpty()) {
        double equity = 0.0, peak = 0.0;
        const QJsonObject records = state.value(QStringLiteral("records")).toObject();
        for (auto it = records.constBegin(); it != records.constEnd(); ++it) {
            const QJsonObject row = it.value().toObject();
            if (row.value(QStringLiteral("status")).toString() == QLatin1String("completed")) {
                ++completed;
                const double trade = row.value(QStringLiteral("net_pnl")).toDouble();
                if (trade > 0) ++wins;
                pnl += trade; equity += trade; peak = qMax(peak, equity);
                drawdown = qMax(drawdown, peak - equity);
            } else if (row.value(QStringLiteral("status")).toString() == QLatin1String("open")) {
                ++open;
            }
        }
    }
    const qint64 updated_ms = research_status_updated_ms(state);
    QString status = QStringLiteral("PAPER TESTING");
    if (state.isEmpty() || updated_ms <= 0 || now_ms - updated_ms > 2 * 60 * 60 * 1000)
        status = QStringLiteral("STALE");
    if (state.value(QStringLiteral("reconciliation_failure")).toBool())
        status = QStringLiteral("RECONCILIATION FAILURE");
    const int next = completed < 10 ? 10 : (completed < 30 ? 30 : 100);
    out.insert(QStringLiteral("status"), status);
    out.insert(QStringLiteral("updated_at_ms"), updated_ms);
    out.insert(QStringLiteral("age_ms"), updated_ms > 0 ? qMax<qint64>(0, now_ms - updated_ms) : -1);
    out.insert(QStringLiteral("policy_sha256"), state.value(QStringLiteral("policy_sha256")));
    out.insert(QStringLiteral("completed"), completed);
    out.insert(QStringLiteral("open"), open);
    out.insert(QStringLiteral("net_pnl"), pnl);
    out.insert(QStringLiteral("max_drawdown"), drawdown);
    const QJsonValue supplied_win_rate = source.value(QStringLiteral("win_rate"));
    out.insert(QStringLiteral("win_rate"), supplied_win_rate.isDouble()
                                                  ? supplied_win_rate
                                                  : (completed > 0 ? QJsonValue(double(wins) / completed)
                                                                   : QJsonValue()));
    out.insert(QStringLiteral("next_milestone"), next);
    return out;
}

inline QJsonObject kalshi_research_status_snapshot(const QString& evidence_dir, qint64 now_ms) {
    struct Capability { const char* family; const char* horizon; const char* report; const char* ledger; const char* role; };
    static const Capability rows[] = {
        {"KXBTCD", "1h", "calibrator.json", "", "primary"},
        {"KXBTC15M", "15m", "kxbtc15m-calibrator.json", "", "diagnostic"},
        {"KXGOLDH", "1h", "commodities-hourly-calibrator.json", "kxgoldh-forward-paper.json", "paper"},
        {"KXSILVERH", "1h", "commodities-hourly-calibrator.json", "kxsilverh-forward-paper.json", "paper"},
        {"KXWTIH", "1h", "commodities-hourly-calibrator.json", "kxwtih-forward-paper.json", "paper"},
        {"KXGOLD15M", "15m", "commodities-15m-calibrator.json", "", "diagnostic"},
        {"KXSILVER15M", "15m", "commodities-15m-calibrator.json", "", "diagnostic"},
        {"KXWTI15M", "15m", "commodities-15m-calibrator.json", "", "diagnostic"},
        {"KXGOLDD", "1d", "commodities-daily-calibrator.json", "", "research"},
        {"KXSILVERD", "1d", "commodities-daily-calibrator.json", "", "research"},
        {"KXWTI", "1d", "commodities-daily-calibrator.json", "", "research"},
        {"KXBTCD", "1d", "kxbtc-daily-calibrator.json", "", "research"},
    };
    QHash<QString, QJsonObject> reports;
    QJsonArray capabilities;
    QJsonObject hourly_ledgers;
    for (const Capability& spec : rows) {
        const QString report_name = QString::fromLatin1(spec.report);
        if (!reports.contains(report_name)) reports.insert(report_name, research_status_read(evidence_dir, report_name));
        const QJsonObject report = reports.value(report_name);
        const QString family = QString::fromLatin1(spec.family);
        const QJsonObject block = report.value(QStringLiteral("by_family")).toObject().value(family).toObject();
        QJsonObject item{{QStringLiteral("family"), family},
                         {QStringLiteral("horizon"), QString::fromLatin1(spec.horizon)},
                         {QStringLiteral("role"), QString::fromLatin1(spec.role)},
                         {QStringLiteral("report_file"), report_name},
                         {QStringLiteral("report_present"), !report.isEmpty()},
                         {QStringLiteral("scored_contracts"), block.value(QStringLiteral("scored_contracts"))},
                         {QStringLiteral("eligible_scored_contracts"), block.value(QStringLiteral("eligible_scored_contracts"))}};
        const auto trust = KalshiBotDecision::family_trust(report, family);
        QString forecast = QStringLiteral("CALIBRATING");
        if (trust == KalshiBotDecision::FamilyTrust::Pass) forecast = QStringLiteral("FORECAST CANDIDATE");
        if (trust == KalshiBotDecision::FamilyTrust::Fail) forecast = QStringLiteral("NO EDGE");
        item.insert(QStringLiteral("forecast_status"), forecast);
        if (*spec.ledger) {
            const QJsonObject raw = research_status_read(evidence_dir, QString::fromLatin1(spec.ledger));
            const QJsonObject ledger = research_paper_ledger(raw, now_ms);
            item.insert(QStringLiteral("paper_ledger"), ledger);
            hourly_ledgers.insert(family, QJsonObject{{QStringLiteral("raw"), raw},
                                                       {QStringLiteral("normalized"), ledger}});
        }
        capabilities.append(item);
    }
    return QJsonObject{{QStringLiteral("schema"), QStringLiteral("kalshi-research-status/v1")},
                       {QStringLiteral("authority"), QStringLiteral("read_only_paper_research")},
                       {QStringLiteral("generated_at_ms"), now_ms},
                       {QStringLiteral("capabilities"), capabilities},
                       {QStringLiteral("hourly_ledgers"), hourly_ledgers}};
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
