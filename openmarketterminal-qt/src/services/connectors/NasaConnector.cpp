#include "services/connectors/NasaConnector.h"

#include "services/connectors/Connector.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>

namespace openmarketterminal::connectors {

NasaConnector& NasaConnector::instance() {
    static NasaConnector s;
    return s;
}

NasaConnector::NasaConnector(QObject* parent) : QObject(parent) {
    qRegisterMetaType<openmarketterminal::connectors::NaturalEvent>();
    qRegisterMetaType<QVector<openmarketterminal::connectors::NaturalEvent>>();
    qRegisterMetaType<openmarketterminal::connectors::Provenance>("openmarketterminal::connectors::Provenance");
}

void NasaConnector::fetch_events(int limit) {
    const QString url =
        QStringLiteral("https://eonet.gsfc.nasa.gov/api/v3/events?status=open&limit=%1").arg(qMax(1, limit));
    const QString cache_key = QStringLiteral("nasa:eonet:open:%1").arg(qMax(1, limit));

    QPointer<NasaConnector> self = this;
    ConnectorFetch::json(
        this, QStringLiteral("NASA EONET"), KeyRequirement::None, /*key_present=*/false, url, cache_key,
        /*ttl_seconds=*/1800, [self](Provenanced<QJsonDocument> r) {
            if (!self)
                return;
            if (!r.ok) {
                emit self->events_failed(r.error, r.provenance);
                return;
            }
            QVector<NaturalEvent> out;
            const QJsonArray events = r.data.object().value(QStringLiteral("events")).toArray();
            out.reserve(events.size());
            for (const auto& v : events) {
                const QJsonObject o = v.toObject();
                NaturalEvent e;
                e.id = o.value(QStringLiteral("id")).toString();
                e.title = o.value(QStringLiteral("title")).toString();
                const QJsonArray cats = o.value(QStringLiteral("categories")).toArray();
                if (!cats.isEmpty())
                    e.category = cats.first().toObject().value(QStringLiteral("title")).toString();
                const QJsonArray geo = o.value(QStringLiteral("geometry")).toArray();
                if (!geo.isEmpty()) {
                    const QJsonObject g = geo.last().toObject();
                    e.date = g.value(QStringLiteral("date")).toString();
                    const QJsonArray coord = g.value(QStringLiteral("coordinates")).toArray();
                    if (coord.size() >= 2) {
                        e.lon = coord.at(0).toDouble();
                        e.lat = coord.at(1).toDouble();
                        e.has_coord = true;
                    }
                }
                out.append(e);
            }
            emit self->events_ready(out, r.provenance);
        });
}

} // namespace openmarketterminal::connectors
