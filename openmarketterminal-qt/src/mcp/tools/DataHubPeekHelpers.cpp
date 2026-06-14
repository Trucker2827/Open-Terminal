#include "mcp/tools/DataHubPeekHelpers.h"

#include "app/DockScreenRouter.h"
#include "app/WindowFrame.h"
#include "datahub/DataHub.h"
#include "mcp/tools/ThreadHelper.h"

#include <QApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QWidget>

namespace openmarketterminal::mcp::tools::detail {

namespace {

constexpr int kMaxContextQuotes = 12;
constexpr qint64 kMaxQuoteAgeMs = 5 * 60 * 1000;

openmarketterminal::DockScreenRouter* find_active_router_main_thread() {
    if (auto* active = QApplication::activeWindow()) {
        if (auto* mw = qobject_cast<openmarketterminal::WindowFrame*>(active))
            return mw->dock_router();
    }
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (auto* mw = qobject_cast<openmarketterminal::WindowFrame*>(w)) {
            if (mw->window_id() == 0)
                return mw->dock_router();
        }
    }
    return nullptr;
}

} // namespace

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

QString build_screen_context_brief() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList lines;
    int count = 0;

    for (const auto& s : openmarketterminal::datahub::DataHub::instance().stats()) {
        if (!s.topic.startsWith(QStringLiteral("market:quote:")))
            continue;
        if (s.subscriber_count <= 0 || s.last_publish_ms <= 0)
            continue;
        if ((now - s.last_publish_ms) > kMaxQuoteAgeMs)
            continue;

        const QString sym = s.topic.mid(QStringLiteral("market:quote:").size());
        const auto q = peek_quote(sym);
        if (!q)
            continue;

        lines << QStringLiteral("%1 $%2 (%3%)")
                     .arg(q->symbol)
                     .arg(q->price, 0, 'f', 2)
                     .arg(q->change_pct, 0, 'f', 2);
        if (++count >= kMaxContextQuotes)
            break;
    }

    if (lines.isEmpty())
        return {};

    return QStringLiteral("[Screen context — live quotes streaming on your terminal]\n") + lines.join('\n');
}

QJsonObject build_terminal_context_json() {
    QJsonObject out;
    QString current_id;
    QString current_title;

    if (qApp) {
        detail::run_on_target_thread_sync(qApp, [&]() {
            if (auto* router = find_active_router_main_thread()) {
                current_id = router->current_screen_id();
                if (!current_id.isEmpty())
                    current_title = openmarketterminal::DockScreenRouter::title_for_id(current_id);
            }
        });
    }

    out["current_screen"] =
        current_id.isEmpty()
            ? QJsonValue(QJsonValue::Null)
            : QJsonValue(QJsonObject{{"id", current_id}, {"title", current_title}});

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QJsonArray quotes;
    QJsonArray topics;

    for (const auto& s : openmarketterminal::datahub::DataHub::instance().stats()) {
        const bool is_quote = s.topic.startsWith(QStringLiteral("market:quote:"));
        const bool is_news = s.topic.startsWith(QStringLiteral("news:"));
        if (!is_quote && !is_news)
            continue;
        if (s.subscriber_count <= 0 && s.last_publish_ms <= 0)
            continue;

        QJsonObject t;
        t["topic"] = s.topic;
        t["subscribers"] = s.subscriber_count;
        t["age_ms"] = s.last_publish_ms > 0 ? static_cast<qint64>(now - s.last_publish_ms) : -1;
        topics.append(t);

        if (is_quote && quotes.size() < kMaxContextQuotes && s.last_publish_ms > 0 &&
            (now - s.last_publish_ms) <= kMaxQuoteAgeMs) {
            const QString sym = s.topic.mid(QStringLiteral("market:quote:").size());
            if (const auto q = peek_quote(sym)) {
                QJsonObject qj = quote_to_json(*q);
                qj["age_ms"] = t["age_ms"];
                quotes.append(qj);
            }
        }
    }

    out["live_quotes"] = quotes;
    out["active_topics"] = topics;
    out["hint"] =
        QStringLiteral("Prefer datahub_peek / get_quote (hub-first) for symbols listed in live_quotes.");
    return out;
}

} // namespace openmarketterminal::mcp::tools::detail
