#include "services/connectors/HdxConnector.h"

#include "services/connectors/Connector.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QUrl>

namespace openmarketterminal::connectors {

HdxConnector& HdxConnector::instance() {
    static HdxConnector s;
    return s;
}

HdxConnector::HdxConnector(QObject* parent) : QObject(parent) {
    qRegisterMetaType<openmarketterminal::connectors::HdxDataset>();
    qRegisterMetaType<QVector<openmarketterminal::connectors::HdxDataset>>();
    qRegisterMetaType<openmarketterminal::connectors::Provenance>("openmarketterminal::connectors::Provenance");
}

void HdxConnector::search(const QString& query, int rows) {
    const QString q = query.trimmed();
    if (q.isEmpty()) {
        emit datasets_ready(query, {}, Provenance{});
        return;
    }
    const QString url = QStringLiteral("https://data.humdata.org/api/3/action/package_search?q=%1&rows=%2")
                            .arg(QString::fromUtf8(QUrl::toPercentEncoding(q)))
                            .arg(qMax(1, rows));
    const QString cache_key = QStringLiteral("hdx:search:%1:%2").arg(q.toLower()).arg(qMax(1, rows));

    QPointer<HdxConnector> self = this;
    ConnectorFetch::json(
        this, QStringLiteral("HDX (CKAN)"), KeyRequirement::None, /*key_present=*/false, url, cache_key,
        /*ttl_seconds=*/3600, [self, query](Provenanced<QJsonDocument> r) {
            if (!self)
                return;
            if (!r.ok) {
                emit self->datasets_failed(r.error, r.provenance);
                return;
            }
            const QJsonObject result = r.data.object().value(QStringLiteral("result")).toObject();
            const QJsonArray arr = result.value(QStringLiteral("results")).toArray();
            QVector<HdxDataset> out;
            out.reserve(arr.size());
            for (const auto& v : arr) {
                const QJsonObject o = v.toObject();
                HdxDataset ds;
                ds.title = o.value(QStringLiteral("title")).toString();
                ds.organization = o.value(QStringLiteral("organization")).toObject().value(QStringLiteral("title")).toString();
                ds.updated = o.value(QStringLiteral("metadata_modified")).toString();
                ds.name = o.value(QStringLiteral("name")).toString();
                ds.resources = o.value(QStringLiteral("num_resources")).toInt();
                out.append(ds);
            }
            emit self->datasets_ready(query, out, r.provenance);
        });
}

} // namespace openmarketterminal::connectors
