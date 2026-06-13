#include "services/markets/MarketSearchService.h"

#include "core/logging/Logger.h"
#include "services/connectors/Connector.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QUrl>
#include <algorithm>

namespace openmarketterminal::services {

namespace {

constexpr int kMinLimit = 1;
constexpr int kMaxLimit = 100;

// Map a Yahoo Finance /v1/finance/search "quotes" array to MarketSearchService
// items. Yahoo is keyless and public — no OpenMarketTerminal dependency.
QList<MarketSearchService::Item> parse_yahoo(const QJsonDocument& doc) {
    QList<MarketSearchService::Item> out;
    const QJsonArray quotes = doc.object().value(QStringLiteral("quotes")).toArray();
    out.reserve(quotes.size());
    for (const auto& v : quotes) {
        const QJsonObject o = v.toObject();
        const QString sym = o.value(QStringLiteral("symbol")).toString();
        if (sym.isEmpty())
            continue;
        QString name = o.value(QStringLiteral("shortname")).toString();
        if (name.isEmpty())
            name = o.value(QStringLiteral("longname")).toString();
        out.push_back({sym, name, o.value(QStringLiteral("exchDisp")).toString(),
                       o.value(QStringLiteral("quoteType")).toString(), /*country=*/QString()});
    }
    return out;
}

} // namespace

MarketSearchService& MarketSearchService::instance() {
    static MarketSearchService s;
    return s;
}

MarketSearchService::MarketSearchService() = default;

void MarketSearchService::search(const QString& query, const QString& type, int limit,
                                 const QString& request_id) {
    Q_UNUSED(type); // Yahoo search has no server-side type filter; left for caller-side filtering.
    const QString q = query.trimmed();
    if (q.isEmpty()) {
        emit results_ready(request_id, query, {});
        return;
    }
    const int clamped = std::clamp(limit, kMinLimit, kMaxLimit);

    // OpenMarketTerminal: direct, keyless Yahoo Finance search via the connector
    // framework (timestamped cache + provenance) — replaces the former OpenMarketTerminal
    // /market/search dependency.
    const QString url =
        QStringLiteral("https://query2.finance.yahoo.com/v1/finance/search?q=%1&quotesCount=%2&newsCount=0")
            .arg(QString::fromUtf8(QUrl::toPercentEncoding(q)))
            .arg(clamped);
    const QString cache_key = QStringLiteral("yahoo:search:%1:%2").arg(q.toLower()).arg(clamped);

    QPointer<MarketSearchService> self = this;
    connectors::ConnectorFetch::json(
        this, QStringLiteral("Yahoo Finance"), connectors::KeyRequirement::None, /*key_present=*/false, url,
        cache_key, /*ttl_seconds=*/120, [self, request_id, query](connectors::Provenanced<QJsonDocument> r) {
            if (!self)
                return;
            emit self->provenance_updated(r.provenance);
            if (!r.ok) {
                LOG_WARN("MarketSearch", "Yahoo search failed: " + r.error);
                emit self->search_failed(request_id, query, r.error);
                return;
            }
            emit self->results_ready(request_id, query, parse_yahoo(r.data));
        });
}

} // namespace openmarketterminal::services
