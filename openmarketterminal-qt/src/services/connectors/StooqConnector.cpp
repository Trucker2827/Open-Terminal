#include "services/connectors/StooqConnector.h"

#include "services/connectors/Connector.h"

#include <QPointer>
#include <QStringList>
#include <QUrl>

namespace openmarketterminal::connectors {

StooqConnector& StooqConnector::instance() {
    static StooqConnector s;
    return s;
}

StooqConnector::StooqConnector(QObject* parent) : QObject(parent) {
    qRegisterMetaType<openmarketterminal::connectors::StooqQuote>();
    qRegisterMetaType<openmarketterminal::connectors::Provenance>("openmarketterminal::connectors::Provenance");
}

void StooqConnector::fetch_quote(const QString& symbol) {
    QString s = symbol.trimmed().toLower();
    if (s.isEmpty()) {
        emit quote_failed(QStringLiteral("missing symbol"), Provenance{});
        return;
    }
    if (!s.contains(QLatin1Char('.')))
        s += QStringLiteral(".us"); // default to the US market

    // f=sd2t2ohlcv -> Symbol,Date,Time,Open,High,Low,Close,Volume ; &h adds a header row.
    const QString url = QStringLiteral("https://stooq.com/q/l/?s=%1&f=sd2t2ohlcv&h&e=csv")
                            .arg(QString::fromUtf8(QUrl::toPercentEncoding(s)));
    const QString cache_key = QStringLiteral("stooq:quote:%1").arg(s);

    QPointer<StooqConnector> self = this;
    ConnectorFetch::text(
        this, QStringLiteral("Stooq"), KeyRequirement::None, /*key_present=*/false, url, cache_key,
        /*ttl_seconds=*/60, [self, s](Provenanced<QString> r) {
            if (!self)
                return;
            if (!r.ok) {
                emit self->quote_failed(r.error, r.provenance);
                return;
            }
            // CSV: header line + one data line.
            const QStringList lines = r.data.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            if (lines.size() < 2 || r.data.contains(QLatin1String("<html"), Qt::CaseInsensitive)
                || r.data.contains(QLatin1String("does not exist"))) {
                emit self->quote_failed(QStringLiteral("Stooq returned no quote (symbol unknown or IP blocked)"),
                                        r.provenance);
                return;
            }
            const QStringList f = lines.at(1).split(QLatin1Char(','));
            if (f.size() < 8 || f.at(3) == QLatin1String("N/D")) {
                emit self->quote_failed(QStringLiteral("Stooq: no data for %1").arg(s), r.provenance);
                return;
            }
            StooqQuote q;
            q.symbol = f.at(0);
            q.date = f.at(1);
            q.time = f.at(2);
            q.open = f.at(3).toDouble();
            q.high = f.at(4).toDouble();
            q.low = f.at(5).toDouble();
            q.close = f.at(6).toDouble();
            q.volume = f.at(7).toDouble();
            q.ok = true;
            emit self->quote_ready(q, r.provenance);
        });
}

} // namespace openmarketterminal::connectors
