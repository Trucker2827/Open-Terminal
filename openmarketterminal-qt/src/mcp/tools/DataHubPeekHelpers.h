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

/// Compact prefix injected into Quick Chat when hub has live quote topics.
QString build_screen_context_brief();

/// JSON snapshot for the `get_terminal_context` MCP tool.
QJsonObject build_terminal_context_json();

} // namespace openmarketterminal::mcp::tools::detail
