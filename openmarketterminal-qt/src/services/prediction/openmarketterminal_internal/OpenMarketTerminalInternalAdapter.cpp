#include "services/prediction/openmarketterminal_internal/OpenMarketTerminalInternalAdapter.h"

#include "core/logging/Logger.h"
#include "datahub/DataHub.h"
#include "datahub/TopicPolicy.h"
#include "storage/secure/SecureStorage.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QVector>

#include <atomic>

namespace openmarketterminal::services::prediction::openmarketterminal_internal {

namespace {

// SecureStorage keys.
constexpr const char* kKeyEndpoint = "openmarketterminal.markets_endpoint";
constexpr const char* kKeyMarketProgramId = "openmarketterminal.market_program_id";

// Hub topic prefix — mirrors `prediction:polymarket:*` and
// `prediction:kalshi:*` already in DATAHUB_TOPICS.md. The wildcard tail
// matches asset_ids and market_ids exactly the way the existing adapters
// do, so screens iterate the registry without special-casing.
constexpr const char* kTopicPrefixMarkets = "prediction:openmarketterminal:markets";
constexpr const char* kTopicPrefixOrderbook = "prediction:openmarketterminal:orderbook:";
constexpr const char* kTopicPrefixPrice = "prediction:openmarketterminal:price:";

// Curated demo dataset. The three example markets from plan §4.2 — used
// when `openmarketterminal.markets_endpoint` is unset so the MarketsTab is reviewable
// before the matching engine is deployed. Numbers are intentionally
// rounded — these are demo prices, not implied probabilities.
struct DemoMarketSeed {
    const char* market_id;       ///< stable id, used as an asset_id suffix too
    const char* question;
    const char* category;
    double yes_price;            ///< 0..1
    double volume;
    const char* end_date_iso;
};

constexpr DemoMarketSeed kDemoMarkets[] = {
    {"openmarketterminal-fed-cuts-2026-05",
     "Fed cuts in May 2026",
     "Macro", 0.62, 1'200'000.0, "2026-05-07"},
    {"openmarketterminal-nyc-80f-2026-05-01",
     "NYC max 80F+ on May 1",
     "Weather", 0.71, 83'000.0, "2026-05-01"},
    {"openmarketterminal-btc-100k-2026-05-31",
     "BTC above $100k by month end",
     "Crypto", 0.45, 421'000.0, "2026-05-31"},
};

QString trim_trailing_slash(QString s) {
    while (s.endsWith('/')) s.chop(1);
    return s;
}

PredictionMarket build_demo_market(const DemoMarketSeed& seed) {
    PredictionMarket m;
    m.key.exchange_id = QStringLiteral("openmarketterminal");
    m.key.market_id = QString::fromLatin1(seed.market_id);
    m.key.event_id = m.key.market_id; // 1:1 for binary demo markets
    m.key.asset_ids = {
        m.key.market_id + QStringLiteral(":yes"),
        m.key.market_id + QStringLiteral(":no"),
    };
    m.question = QString::fromUtf8(seed.question);
    m.category = QString::fromUtf8(seed.category);
    m.end_date_iso = QString::fromLatin1(seed.end_date_iso);
    m.volume = seed.volume;
    m.liquidity = seed.volume * 0.05; // illustrative
    m.active = true;
    m.closed = false;

    Outcome yes;
    yes.name = QStringLiteral("Yes");
    yes.asset_id = m.key.asset_ids.value(0);
    yes.price = seed.yes_price;

    Outcome no;
    no.name = QStringLiteral("No");
    no.asset_id = m.key.asset_ids.value(1);
    no.price = 1.0 - seed.yes_price;

    m.outcomes = {yes, no};
    m.tags = {m.category};
    m.extras.insert(QStringLiteral("is_demo"), true);
    m.extras.insert(QStringLiteral("settlement_currency"), QStringLiteral("USD"));
    return m;
}

} // namespace

OpenMarketTerminalInternalAdapter::OpenMarketTerminalInternalAdapter(QObject* parent)
    : PredictionExchangeAdapter(parent) {
    nam_ = new QNetworkAccessManager(this);
}

OpenMarketTerminalInternalAdapter::~OpenMarketTerminalInternalAdapter() = default;

// ── Identity ───────────────────────────────────────────────────────────────

QString OpenMarketTerminalInternalAdapter::id() const {
    return QStringLiteral("openmarketterminal");
}

QString OpenMarketTerminalInternalAdapter::display_name() const {
    return QStringLiteral("OpenMarketTerminal Internal");
}

ExchangeCapabilities OpenMarketTerminalInternalAdapter::capabilities() const {
    ExchangeCapabilities c;
    c.has_events = true;
    c.has_multi_outcome = false;        // binary markets only at launch
    c.has_orderbook_ws = true;
    c.has_trade_ws = true;
    c.has_rewards = false;
    c.has_maker_rebates = false;
    c.has_leaderboard = false;
    c.supports_limit_orders = true;
    c.supports_market_orders = true;
    c.supports_gtd = false;
    c.supports_fok = true;
    c.supports_fak = true;
    c.supports_decrease_order = false;
    c.supports_batch_orders = false;
    c.quote_currency = QStringLiteral("USD");
    c.min_order_size = 1.0;
    c.default_tick_size = 0.01;
    c.max_requests_per_sec = 5;
    return c;
}

// ── Endpoint resolution ────────────────────────────────────────────────────

QString OpenMarketTerminalInternalAdapter::resolve_endpoint() const {
    auto r = SecureStorage::instance().retrieve(QString::fromLatin1(kKeyEndpoint));
    if (r.is_ok()) return trim_trailing_slash(r.value().trimmed());
    return {};
}

bool OpenMarketTerminalInternalAdapter::is_demo_mode() const {
    return resolve_endpoint().isEmpty();
}

// ── Read endpoints ─────────────────────────────────────────────────────────
//
// All read endpoints currently route through the demo emitters when the
// markets endpoint is unset. The HTTP path is left as a TODO seam: when
// the OpenMarketTerminal matching engine ships, swap the demo emit for a fetch
// against `<endpoint>/markets`, `<endpoint>/orderbook/<asset_id>`, etc.

void OpenMarketTerminalInternalAdapter::list_markets(const QString& /*category*/,
                                          const QString& /*sort_by*/,
                                          int /*limit*/, int /*offset*/) {
    if (is_demo_mode()) {
        emit_mock_markets();
    } else {
        // Real HTTP path lands when the matching engine ships. For now,
        // surface a clean error so the UI shows "service unavailable"
        // instead of hanging.
        emit_demo_unavailable(QStringLiteral("list_markets"));
    }
}

void OpenMarketTerminalInternalAdapter::list_events(const QString& /*category*/,
                                         const QString& /*sort_by*/,
                                         int /*limit*/, int /*offset*/) {
    if (is_demo_mode()) {
        emit_mock_events();
    } else {
        emit_demo_unavailable(QStringLiteral("list_events"));
    }
}

void OpenMarketTerminalInternalAdapter::search(const QString& query, int /*limit*/) {
    if (is_demo_mode()) {
        // Filter the curated dataset client-side. Cheap and good enough
        // for demo mode.
        QVector<PredictionMarket> hits;
        for (const auto& seed : kDemoMarkets) {
            const auto question = QString::fromUtf8(seed.question);
            if (query.isEmpty() ||
                question.contains(query, Qt::CaseInsensitive)) {
                hits.push_back(build_demo_market(seed));
            }
        }
        QPointer<OpenMarketTerminalInternalAdapter> self = this;
        QTimer::singleShot(0, this, [self, hits]() {
            if (!self) return;
            emit self->search_results_ready(hits, {});
        });
        return;
    }
    emit_demo_unavailable(QStringLiteral("search"));
}

void OpenMarketTerminalInternalAdapter::list_tags() {
    if (is_demo_mode()) {
        QPointer<OpenMarketTerminalInternalAdapter> self = this;
        QTimer::singleShot(0, this, [self]() {
            if (!self) return;
            emit self->tags_ready(QStringList{
                QStringLiteral("Macro"),
                QStringLiteral("Weather"),
                QStringLiteral("Crypto"),
            });
        });
        return;
    }
    emit_demo_unavailable(QStringLiteral("list_tags"));
}

void OpenMarketTerminalInternalAdapter::fetch_market(const MarketKey& key) {
    if (is_demo_mode()) {
        for (const auto& seed : kDemoMarkets) {
            if (key.market_id == QString::fromLatin1(seed.market_id)) {
                auto m = build_demo_market(seed);
                QPointer<OpenMarketTerminalInternalAdapter> self = this;
                QTimer::singleShot(0, this, [self, m]() {
                    if (!self) return;
                    emit self->market_detail_ready(m);
                });
                return;
            }
        }
    }
    emit_demo_unavailable(QStringLiteral("fetch_market"));
}

void OpenMarketTerminalInternalAdapter::fetch_event(const MarketKey& key) {
    fetch_market(key); // 1:1 in demo binary markets
}

void OpenMarketTerminalInternalAdapter::fetch_order_book(const QString& asset_id) {
    if (is_demo_mode()) {
        // A flat shallow book centred on each demo asset's price. Replace
        // with a real orderbook fetch when the matching engine ships.
        PredictionOrderBook book;
        book.asset_id = asset_id;
        for (const auto& seed : kDemoMarkets) {
            const auto yes_id = QString::fromLatin1(seed.market_id) +
                                QStringLiteral(":yes");
            if (asset_id == yes_id) {
                const double mid = seed.yes_price;
                book.bids = {
                    {mid - 0.01, 250.0},
                    {mid - 0.02, 500.0},
                    {mid - 0.05, 1'200.0},
                };
                book.asks = {
                    {mid + 0.01, 250.0},
                    {mid + 0.02, 500.0},
                    {mid + 0.05, 1'200.0},
                };
                break;
            }
        }
        QPointer<OpenMarketTerminalInternalAdapter> self = this;
        QTimer::singleShot(0, this, [self, book]() {
            if (!self) return;
            emit self->order_book_ready(book);
        });
        return;
    }
    emit_demo_unavailable(QStringLiteral("fetch_order_book"));
}

void OpenMarketTerminalInternalAdapter::fetch_price_history(const QString& /*asset_id*/,
                                                 const QString& /*interval*/,
                                                 int /*fidelity*/) {
    if (is_demo_mode()) {
        emit_demo_unavailable(QStringLiteral("fetch_price_history (demo)"));
        return;
    }
    emit_demo_unavailable(QStringLiteral("fetch_price_history"));
}

void OpenMarketTerminalInternalAdapter::fetch_recent_trades(const MarketKey& /*key*/, int /*limit*/) {
    QPointer<OpenMarketTerminalInternalAdapter> self = this;
    QTimer::singleShot(0, this, [self]() {
        if (!self) return;
        emit self->recent_trades_ready({});
    });
}

// ── Real-time ──────────────────────────────────────────────────────────────

void OpenMarketTerminalInternalAdapter::subscribe_market(const QStringList& /*asset_ids*/) {
    // No-op in demo mode. Real path: open a WS to <endpoint>/ws and forward
    // ws_price_updated / ws_orderbook_updated when the engine ships.
}

void OpenMarketTerminalInternalAdapter::unsubscribe_market(const QStringList& /*asset_ids*/) {
    // No-op in demo mode.
}

bool OpenMarketTerminalInternalAdapter::is_ws_connected() const {
    // Demo mode is never "connected" — screens use this to choose a
    // disabled state for live-stream-only widgets.
    return false;
}

// ── Trading ────────────────────────────────────────────────────────────────
//
// Trading is gated behind both `has_credentials()` and the existence of
// the on-chain `openmarketterminal_market` program. Until either lands, all trading
// methods emit `error_occurred("not deployed", …)` so the order ticket
// can stay disabled with a clear message.

bool OpenMarketTerminalInternalAdapter::has_credentials() const {
    auto r = SecureStorage::instance().retrieve(
        QString::fromLatin1(kKeyMarketProgramId));
    return r.is_ok() && !r.value().trimmed().isEmpty();
}

QString OpenMarketTerminalInternalAdapter::account_label() const {
    return QStringLiteral("(connect wallet)");
}

void OpenMarketTerminalInternalAdapter::fetch_balance()           { emit_demo_unavailable(QStringLiteral("fetch_balance")); }
void OpenMarketTerminalInternalAdapter::fetch_positions()         { emit_demo_unavailable(QStringLiteral("fetch_positions")); }
void OpenMarketTerminalInternalAdapter::fetch_open_orders()       { emit_demo_unavailable(QStringLiteral("fetch_open_orders")); }
void OpenMarketTerminalInternalAdapter::fetch_user_activity(int)  { emit_demo_unavailable(QStringLiteral("fetch_user_activity")); }

void OpenMarketTerminalInternalAdapter::place_order(const OrderRequest& /*req*/) {
    emit_demo_unavailable(QStringLiteral("place_order"));
}

void OpenMarketTerminalInternalAdapter::cancel_order(const QString& order_id) {
    emit error_occurred(QStringLiteral("cancel_order"),
                        QStringLiteral("openmarketterminal_market program not deployed"));
    emit order_cancelled(order_id, /*ok=*/false,
                         QStringLiteral("openmarketterminal_market program not deployed"));
}

void OpenMarketTerminalInternalAdapter::cancel_all_for_market(const MarketKey& /*key*/,
                                                   const QString& /*asset_id*/) {
    emit_demo_unavailable(QStringLiteral("cancel_all_for_market"));
}

// ── Hub ────────────────────────────────────────────────────────────────────

void OpenMarketTerminalInternalAdapter::ensure_registered_with_hub() {
    if (hub_registered_) return;
    hub_registered_ = true;

    // No producer to register yet — until the matching engine is up,
    // the adapter doesn't own any topic family that's ready to publish.
    // Reserve the topic policies anyway so subscribers in screens see a
    // policy when they peek (TTL applied to mock publishes once we have
    // a producer for them).
    auto& hub = openmarketterminal::datahub::DataHub::instance();
    openmarketterminal::datahub::TopicPolicy markets_p;
    markets_p.ttl_ms = 30 * 1000;
    markets_p.min_interval_ms = 10 * 1000;
    hub.set_policy_pattern(QString::fromLatin1(kTopicPrefixMarkets), markets_p);

    openmarketterminal::datahub::TopicPolicy book_p;
    book_p.ttl_ms = 5 * 1000;
    book_p.min_interval_ms = 1 * 1000;
    hub.set_policy_pattern(QString::fromLatin1(kTopicPrefixOrderbook) +
                           QStringLiteral("*"), book_p);

    openmarketterminal::datahub::TopicPolicy price_p;
    price_p.ttl_ms = 5 * 1000;
    price_p.min_interval_ms = 1 * 1000;
    hub.set_policy_pattern(QString::fromLatin1(kTopicPrefixPrice) +
                           QStringLiteral("*"), price_p);

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        if (is_demo_mode()) {
            LOG_INFO("OpenMarketTerminalInternalAdapter",
                     "registered (demo mode — no openmarketterminal.markets_endpoint configured)");
        } else {
            LOG_INFO("OpenMarketTerminalInternalAdapter",
                     QString("registered (endpoint=%1)").arg(resolve_endpoint()));
        }
    }
}

// ── Mock emitters ──────────────────────────────────────────────────────────

void OpenMarketTerminalInternalAdapter::emit_mock_markets() {
    QVector<PredictionMarket> markets;
    markets.reserve(static_cast<int>(std::size(kDemoMarkets)));
    for (const auto& seed : kDemoMarkets) {
        markets.push_back(build_demo_market(seed));
    }
    QPointer<OpenMarketTerminalInternalAdapter> self = this;
    QTimer::singleShot(0, this, [self, markets]() {
        if (!self) return;
        emit self->markets_ready(markets);
    });
}

void OpenMarketTerminalInternalAdapter::emit_mock_events() {
    QVector<PredictionEvent> events;
    events.reserve(static_cast<int>(std::size(kDemoMarkets)));
    for (const auto& seed : kDemoMarkets) {
        PredictionEvent e;
        e.key.exchange_id = QStringLiteral("openmarketterminal");
        e.key.event_id = QString::fromLatin1(seed.market_id);
        e.title = QString::fromUtf8(seed.question);
        e.category = QString::fromUtf8(seed.category);
        e.volume = seed.volume;
        e.active = true;
        e.markets = {build_demo_market(seed)};
        e.tags = {e.category};
        e.extras.insert(QStringLiteral("is_demo"), true);
        events.push_back(e);
    }
    QPointer<OpenMarketTerminalInternalAdapter> self = this;
    QTimer::singleShot(0, this, [self, events]() {
        if (!self) return;
        emit self->events_ready(events);
    });
}

void OpenMarketTerminalInternalAdapter::emit_mock_tags() {
    QPointer<OpenMarketTerminalInternalAdapter> self = this;
    QTimer::singleShot(0, this, [self]() {
        if (!self) return;
        emit self->tags_ready(QStringList{
            QStringLiteral("Macro"),
            QStringLiteral("Weather"),
            QStringLiteral("Crypto"),
        });
    });
}

void OpenMarketTerminalInternalAdapter::emit_demo_unavailable(const QString& context) {
    QPointer<OpenMarketTerminalInternalAdapter> self = this;
    const auto msg = QStringLiteral(
        "OpenMarketTerminal Internal markets are in demo mode — set "
        "`openmarketterminal.markets_endpoint` in SecureStorage to enable live data.");
    QTimer::singleShot(0, this, [self, context, msg]() {
        if (!self) return;
        emit self->error_occurred(context, msg);
    });
}

} // namespace openmarketterminal::services::prediction::openmarketterminal_internal
