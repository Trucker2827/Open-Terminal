#include "services/connectors/WorldBankConnector.h"

#include "services/connectors/Connector.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

namespace openmarketterminal::connectors {

WorldBankConnector& WorldBankConnector::instance() {
    static WorldBankConnector s;
    return s;
}

WorldBankConnector::WorldBankConnector(QObject* parent) : QObject(parent) {
    qRegisterMetaType<openmarketterminal::connectors::WorldBankPoint>();
    qRegisterMetaType<QVector<openmarketterminal::connectors::WorldBankPoint>>();
    qRegisterMetaType<openmarketterminal::connectors::Provenance>("openmarketterminal::connectors::Provenance");
}

void WorldBankConnector::fetch_series(const QString& country, const QString& indicator, int points) {
    const QString c = country.trimmed().isEmpty() ? QStringLiteral("USA") : country.trimmed();
    const QString ind = indicator.trimmed();
    if (ind.isEmpty()) {
        emit series_failed(QStringLiteral("missing indicator code"), Provenance{});
        return;
    }

    // https://api.worldbank.org/v2/country/{ISO3}/indicator/{CODE}?format=json&per_page=N
    const QString url = QStringLiteral("https://api.worldbank.org/v2/country/%1/indicator/%2?format=json&per_page=%3")
                            .arg(QString::fromUtf8(QUrl::toPercentEncoding(c)),
                                 QString::fromUtf8(QUrl::toPercentEncoding(ind)))
                            .arg(points);
    const QString cache_key = QStringLiteral("worldbank:%1:%2:%3").arg(c, ind).arg(points);

    QPointer<WorldBankConnector> self = this;
    ConnectorFetch::json(
        this, QStringLiteral("World Bank"), KeyRequirement::None, /*key_present=*/false, url, cache_key,
        /*ttl_seconds=*/86400, // macro series update infrequently — cache a day.
        [self, c, ind](Provenanced<QJsonDocument> r) {
            if (!self)
                return;
            if (!r.ok) {
                emit self->series_failed(r.error, r.provenance);
                return;
            }
            // Response is [ {pagination meta}, [ {country, indicator, date, value}, ... ] ].
            const QJsonArray top = r.data.array();
            if (top.size() < 2 || !top.at(1).isArray()) {
                emit self->series_failed(QStringLiteral("unexpected World Bank response shape"), r.provenance);
                return;
            }
            QString indicator_name;
            QVector<WorldBankPoint> series;
            const QJsonArray arr = top.at(1).toArray();
            series.reserve(arr.size());
            for (const auto& v : arr) {
                const QJsonObject o = v.toObject();
                if (indicator_name.isEmpty())
                    indicator_name = o.value(QStringLiteral("indicator")).toObject().value(QStringLiteral("value")).toString();
                WorldBankPoint p;
                p.date = o.value(QStringLiteral("date")).toString();
                const QJsonValue val = o.value(QStringLiteral("value"));
                p.has_value = val.isDouble();
                p.value = val.toDouble();
                series.append(p);
            }
            emit self->series_ready(c, ind, indicator_name, series, r.provenance);
        });
}

} // namespace openmarketterminal::connectors
