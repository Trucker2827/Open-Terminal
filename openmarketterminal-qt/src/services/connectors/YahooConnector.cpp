#include "services/connectors/YahooConnector.h"

#include "services/connectors/Connector.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

namespace openmarketterminal::connectors {

YahooConnector& YahooConnector::instance() {
    static YahooConnector s;
    return s;
}

YahooConnector::YahooConnector(QObject* parent) : QObject(parent) {
    qRegisterMetaType<openmarketterminal::connectors::YahooSearchHit>();
    qRegisterMetaType<QVector<openmarketterminal::connectors::YahooSearchHit>>();
    qRegisterMetaType<openmarketterminal::connectors::Provenance>("openmarketterminal::connectors::Provenance");
}

void YahooConnector::search(const QString& query, int limit) {
    const QString q = query.trimmed();
    if (q.isEmpty()) {
        emit search_ready({}, Provenance{});
        return;
    }

    const QString url =
        QStringLiteral("https://query2.finance.yahoo.com/v1/finance/search?q=%1&quotesCount=%2&newsCount=0")
            .arg(QString::fromUtf8(QUrl::toPercentEncoding(q)))
            .arg(limit);
    const QString cache_key = QStringLiteral("yahoo:search:%1:%2").arg(q.toLower()).arg(limit);

    ConnectorFetch::json(
        this, QStringLiteral("Yahoo Finance"), KeyRequirement::None, /*key_present=*/false, url, cache_key,
        /*ttl_seconds=*/120, [this](Provenanced<QJsonDocument> r) {
            if (!r.ok) {
                emit search_failed(r.error, r.provenance);
                return;
            }
            QVector<YahooSearchHit> hits;
            const QJsonArray quotes = r.data.object().value(QStringLiteral("quotes")).toArray();
            for (const auto& v : quotes) {
                const QJsonObject o = v.toObject();
                YahooSearchHit h;
                h.symbol = o.value(QStringLiteral("symbol")).toString();
                if (h.symbol.isEmpty())
                    continue;
                h.name = o.value(QStringLiteral("shortname")).toString();
                if (h.name.isEmpty())
                    h.name = o.value(QStringLiteral("longname")).toString();
                h.exchange = o.value(QStringLiteral("exchDisp")).toString();
                h.type = o.value(QStringLiteral("quoteType")).toString();
                hits.append(h);
            }
            emit search_ready(hits, r.provenance);
        });
}

} // namespace openmarketterminal::connectors
