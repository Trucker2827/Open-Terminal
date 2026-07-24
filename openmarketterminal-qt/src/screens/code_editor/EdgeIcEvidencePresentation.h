#pragma once
// EdgeIcEvidencePresentation.h — row descriptor for the published edge IC
// report (edge-ic.json, written by scripts/edge_ic_report.py --publish) in the
// Research Lab evidence library. A missing or unparseable file yields an
// absent row — the library never fabricates an entry for evidence that does
// not exist.

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTimeZone>

namespace openmarketterminal::screens {

struct EdgeIcEvidenceRow {
    bool present = false;
    QString title;       // "edge-ic"
    QString type;        // "EDGE IC REPORT"
    QString generated;   // report generation time (UTC) or "--"
    QString detail;      // resolved decisions + venues, for the tooltip
    QString path;
};

inline EdgeIcEvidenceRow edge_ic_evidence_row(const QString& path) {
    EdgeIcEvidenceRow row;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return row;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value(QStringLiteral("event")).toString() != QStringLiteral("edge_ic_report"))
        return row;
    row.present = true;
    row.title = QStringLiteral("edge-ic");
    row.type = QStringLiteral("EDGE IC REPORT");
    row.path = path;
    const qint64 generated_ms =
        static_cast<qint64>(root.value(QStringLiteral("generated_at_ms")).toDouble(0));
    row.generated = generated_ms > 0
        ? QDateTime::fromMSecsSinceEpoch(generated_ms, QTimeZone::UTC)
              .toString(QStringLiteral("yyyy-MM-dd HH:mm 'UTC'"))
        : QStringLiteral("--");
    const QJsonObject overall = root.value(QStringLiteral("overall")).toObject();
    const QStringList venues = root.value(QStringLiteral("venues")).toObject().keys();
    row.detail = QStringLiteral("%1 resolved decisions · venues: %2")
                     .arg(overall.value(QStringLiteral("resolved_decisions")).toInt())
                     .arg(venues.isEmpty() ? QStringLiteral("none") : venues.join(QStringLiteral(", ")));
    return row;
}

} // namespace openmarketterminal::screens
