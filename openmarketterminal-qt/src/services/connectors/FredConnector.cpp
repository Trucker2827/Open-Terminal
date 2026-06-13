#include "services/connectors/FredConnector.h"

#include "services/connectors/Connector.h"
#include "storage/repositories/SettingsRepository.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QUrl>

namespace openmarketterminal::connectors {

namespace {
QString fred_key() {
    auto r = openmarketterminal::SettingsRepository::instance().get(QStringLiteral("connectors.fred_api_key"));
    return r.is_ok() ? r.value().trimmed() : QString();
}
// Display URL never contains the key — the badge tooltip must not leak it.
QString fred_display_url(const QString& sid) {
    return QStringLiteral("https://api.stlouisfed.org/fred/series/observations?series_id=%1&api_key=***").arg(sid);
}
} // namespace

FredConnector& FredConnector::instance() {
    static FredConnector s;
    return s;
}

FredConnector::FredConnector(QObject* parent) : QObject(parent) {
    qRegisterMetaType<openmarketterminal::connectors::FredPoint>();
    qRegisterMetaType<QVector<openmarketterminal::connectors::FredPoint>>();
    qRegisterMetaType<openmarketterminal::connectors::Provenance>("openmarketterminal::connectors::Provenance");
}

bool FredConnector::has_key() {
    return !fred_key().isEmpty();
}

void FredConnector::fetch_series(const QString& series_id, int points) {
    const QString sid = series_id.trimmed();
    const QString key = fred_key();

    Provenance prov;
    prov.source = QStringLiteral("FRED (St. Louis Fed)");
    prov.source_url = fred_display_url(sid);
    prov.key_req = KeyRequirement::Required;
    prov.key_present = !key.isEmpty();
    prov.fetched_at_ms = QDateTime::currentMSecsSinceEpoch();

    if (sid.isEmpty()) {
        emit series_failed(QStringLiteral("missing series id"), prov);
        return;
    }
    if (key.isEmpty()) {
        // Make the key requirement explicit instead of silently failing.
        emit series_failed(QStringLiteral("FRED API key required — set connectors.fred_api_key in Settings"), prov);
        return;
    }

    const QString url = QStringLiteral("https://api.stlouisfed.org/fred/series/observations?series_id=%1&api_key=%2"
                                       "&file_type=json&sort_order=desc&limit=%3")
                            .arg(QString::fromUtf8(QUrl::toPercentEncoding(sid)), key)
                            .arg(points);
    const QString cache_key = QStringLiteral("fred:%1:%2").arg(sid).arg(points);

    QPointer<FredConnector> self = this;
    ConnectorFetch::json(
        this, QStringLiteral("FRED (St. Louis Fed)"), KeyRequirement::Required, /*key_present=*/true, url, cache_key,
        /*ttl_seconds=*/3600, [self, sid](Provenanced<QJsonDocument> r) {
            if (!self)
                return;
            // Redact the key from the provenance the UI will display.
            r.provenance.source_url = fred_display_url(sid);
            if (!r.ok) {
                emit self->series_failed(r.error, r.provenance);
                return;
            }
            QVector<FredPoint> series;
            const QJsonArray obs = r.data.object().value(QStringLiteral("observations")).toArray();
            series.reserve(obs.size());
            for (const auto& v : obs) {
                const QJsonObject o = v.toObject();
                FredPoint p;
                p.date = o.value(QStringLiteral("date")).toString();
                const QString val = o.value(QStringLiteral("value")).toString();
                bool ok = false;
                const double d = val.toDouble(&ok);
                p.has_value = ok;
                p.value = ok ? d : 0.0;
                series.append(p);
            }
            emit self->series_ready(sid, series, r.provenance);
        });
}

} // namespace openmarketterminal::connectors
