// Core-only DataHub peek helpers. This TU is QtWidgets-free (Qt6::Core only) so
// it can compile into the headless openterminal_core library and be reached from
// register_core_tools(). The GUI screen-context helpers
// (build_screen_context_brief / build_terminal_context_json) that read the active
// WindowFrame live in DataHubScreenContext.cpp.
#include "mcp/tools/DataHubPeekHelpers.h"

#include "datahub/DataHub.h"
#include "mcp/tools/ThreadHelper.h"

#include <QDateTime>

namespace openmarketterminal::mcp::tools::detail {

qint64 topic_age_ms(const QString& topic) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const auto& s : openmarketterminal::datahub::DataHub::instance().stats()) {
        if (s.topic == topic && s.last_publish_ms > 0)
            return now - s.last_publish_ms;
    }
    return -1;
}

std::optional<openmarketterminal::services::QuoteData> peek_quote(const QString& symbol) {
    const QString sym = symbol.trimmed().toUpper();
    if (sym.isEmpty())
        return std::nullopt;

    const QString topic = QStringLiteral("market:quote:") + sym;
    const QVariant v = openmarketterminal::datahub::DataHub::instance().peek(topic);
    if (!v.isValid() || !v.canConvert<openmarketterminal::services::QuoteData>())
        return std::nullopt;

    const auto q = v.value<openmarketterminal::services::QuoteData>();
    if (q.symbol.isEmpty())
        return std::nullopt;
    return q;
}

QJsonObject quote_to_json(const openmarketterminal::services::QuoteData& q) {
    return QJsonObject{{"symbol", q.symbol},
                       {"name", q.name},
                       {"price", q.price},
                       {"change", q.change},
                       {"change_pct", q.change_pct},
                       {"high", q.high},
                       {"low", q.low},
                       {"volume", q.volume}};
}

std::optional<QVector<openmarketterminal::services::NewsArticle>> peek_news_general() {
    const QVariant v =
        openmarketterminal::datahub::DataHub::instance().peek(QStringLiteral("news:general"));
    if (!v.isValid())
        return std::nullopt;

    if (v.canConvert<QVector<openmarketterminal::services::NewsArticle>>()) {
        const auto articles = v.value<QVector<openmarketterminal::services::NewsArticle>>();
        if (!articles.isEmpty())
            return articles;
    }
    return std::nullopt;
}

std::optional<double> peek_or_fetch_quote_price(const QString& symbol) {
    // Cache hit: peek_quote gives a fresh cached price — cheap, no network.
    if (const auto q = peek_quote(symbol); q && q->price > 0.0)
        return q->price;

    // Cache miss: synchronous fetch via MarketDataService (same path as get_quote).
    // run_async_wait posts to the service's thread and blocks the calling thread
    // until the callback fires — safe from worker threads in the MCP handler pool.
    auto* svc = &openmarketterminal::services::MarketDataService::instance();
    const QString sym = symbol.trimmed().toUpper();
    bool ok = false;
    double price = 0.0;
    run_async_wait(svc, [svc, sym, &ok, &price](auto signal_done) {
        svc->fetch_quotes({sym}, [&ok, &price, sym, signal_done](
                                     bool success,
                                     QVector<openmarketterminal::services::QuoteData> quotes) {
            if (success) {
                for (const auto& candidate : quotes) {
                    if (candidate.symbol.compare(sym, Qt::CaseInsensitive) == 0
                        && candidate.price > 0.0) {
                        price = candidate.price;
                        ok = true;
                        break;
                    }
                }
            }
            signal_done();
        });
    });

    return ok ? std::optional<double>{price} : std::nullopt;
}

} // namespace openmarketterminal::mcp::tools::detail
