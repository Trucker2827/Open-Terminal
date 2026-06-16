#pragma once

#include "services/markets/MarketDataService.h"
#include "services/news/NewsService.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <optional>

namespace openmarketterminal::mcp::tools::detail {

/// Age of the last publish for `topic`, or -1 if unknown.
qint64 topic_age_ms(const QString& topic);

/// Fresh cached quote from `market:quote:<sym>` (respects DataHub TTL).
std::optional<openmarketterminal::services::QuoteData> peek_quote(const QString& symbol);

QJsonObject quote_to_json(const openmarketterminal::services::QuoteData& q);

/// Fresh cached general news feed from `news:general`.
std::optional<QVector<openmarketterminal::services::NewsArticle>> peek_news_general();

/// Cache-first price: peek_quote first; on a miss, falls back to a synchronous
/// MarketDataService fetch (the same path get_quote uses). Returns the price
/// (>0) or nullopt. Blocks the calling thread until the fetch callback fires
/// when the cache is empty; the calling thread must NOT be the main thread
/// in GUI builds (run_async_wait handles the cross-thread post).
std::optional<double> peek_or_fetch_quote_price(const QString& symbol);

} // namespace openmarketterminal::mcp::tools::detail
