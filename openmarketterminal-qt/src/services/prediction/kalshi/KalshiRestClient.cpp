#include "services/prediction/kalshi/KalshiRestClient.h"

#include "core/logging/Logger.h"

#include <algorithm>
#include <functional>
#include <memory>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

namespace openmarketterminal::services::prediction::kalshi_ns {

namespace pr = openmarketterminal::services::prediction;

static const QString kProdBase = QStringLiteral("https://external-api.kalshi.com/trade-api/v2");
static const QString kDemoBase = QStringLiteral("https://external-api.demo.kalshi.co/trade-api/v2");
static constexpr const char* kExchangeId = "kalshi";

// ── Fixed-point helpers ─────────────────────────────────────────────────────
//
// Kalshi expresses contract counts in fixed-point strings (_fp suffix)
// with 2 decimal places (1.00 == 1 contract). Convert to double at the
// boundary.

static double fp_to_double(const QJsonValue& v) {
    if (v.isString()) return v.toString().toDouble();
    if (v.isDouble()) return v.toDouble();
    return 0.0;
}

static double str_or_num(const QJsonValue& v) {
    if (v.isString()) {
        QString s = v.toString();
        // Historical endpoints prefix prices with "$" (e.g. "$0.05") — strip it.
        if (s.startsWith('$')) s = s.mid(1);
        return s.toDouble();
    }
    return v.toDouble();
}

// ── Market mapping ──────────────────────────────────────────────────────────

static pr::PredictionMarket parse_market(const QJsonObject& obj) {
    pr::PredictionMarket m;
    m.key.exchange_id = QString::fromLatin1(kExchangeId);
    m.key.market_id = obj.value("ticker").toString();
    m.key.event_id = obj.value("event_ticker").toString();
    m.key.asset_ids = {m.key.market_id + QStringLiteral(":yes"),
                       m.key.market_id + QStringLiteral(":no")};

    // Kalshi has title + yes_sub_title / no_sub_title. We use title as the
    // question and fall back to the combined sub-titles if title is missing.
    m.question = obj.value("title").toString();
    if (m.question.isEmpty()) m.question = obj.value("yes_sub_title").toString();
    m.description = obj.value("rules_primary").toString();
    m.category = obj.value("event_ticker").toString();  // Kalshi has no real category field
    m.image_url = QString();
    m.end_date_iso = obj.value("close_time").toString();

    m.volume = fp_to_double(obj.value("volume_fp"));
    m.open_interest = fp_to_double(obj.value("open_interest_fp"));
    m.liquidity = str_or_num(obj.value("liquidity_dollars"));

    // Kalshi v2 market statuses: unopened, open, active, paused, closed, settled.
    // Real API returns "active" (not "open") for live markets — compare
    // case-insensitively since casing has varied across API versions.
    const QString status = obj.value("status").toString().toLower();
    m.active = (status == QStringLiteral("open") || status == QStringLiteral("active"));
    m.closed = (status == QStringLiteral("closed") || status == QStringLiteral("determined") ||
                status == QStringLiteral("disputed") || status == QStringLiteral("amended") ||
                status == QStringLiteral("settled") || status == QStringLiteral("finalized"));

    pr::Outcome yes;
    yes.name = QStringLiteral("Yes");
    yes.asset_id = m.key.asset_ids[0];
    yes.price = str_or_num(obj.value("yes_bid_dollars"));
    pr::Outcome no;
    no.name = QStringLiteral("No");
    no.asset_id = m.key.asset_ids[1];
    no.price = str_or_num(obj.value("no_bid_dollars"));
    m.outcomes = {yes, no};

    m.extras.insert(QStringLiteral("status"), obj.value("status").toString());
    // Kalshi markets don't carry series_ticker directly; the event does. When
    // parse_market is called from parse_event the caller patches this in.
    // We still insert an empty placeholder so callers can unconditionally read.
    m.extras.insert(QStringLiteral("series_ticker"), QString());
    m.extras.insert(QStringLiteral("yes_ask_dollars"), str_or_num(obj.value("yes_ask_dollars")));
    m.extras.insert(QStringLiteral("no_ask_dollars"), str_or_num(obj.value("no_ask_dollars")));
    m.extras.insert(QStringLiteral("last_price_dollars"), str_or_num(obj.value("last_price_dollars")));
    m.extras.insert(QStringLiteral("volume_24h_fp"), fp_to_double(obj.value("volume_24h_fp")));
    m.extras.insert(QStringLiteral("can_close_early"), obj.value("can_close_early").toBool());
    m.extras.insert(QStringLiteral("market_type"), obj.value("market_type").toString());
    m.extras.insert(QStringLiteral("yes_sub_title"), obj.value("yes_sub_title").toString());
    m.extras.insert(QStringLiteral("no_sub_title"), obj.value("no_sub_title").toString());
    m.extras.insert(QStringLiteral("settlement_value_dollars"),
                    str_or_num(obj.value("settlement_value_dollars")));
    m.extras.insert(QStringLiteral("settlement_timer_seconds"),
                    qint64(obj.value("settlement_timer_seconds").toDouble()));
    m.extras.insert(QStringLiteral("result"), obj.value("result").toString());
    m.extras.insert(QStringLiteral("expiration_value"), obj.value("expiration_value").toString());
    m.extras.insert(QStringLiteral("settlement_ts"), obj.value("settlement_ts").toString());
    m.extras.insert(QStringLiteral("expected_expiration_time"),
                    obj.value("expected_expiration_time").toString());
    m.extras.insert(QStringLiteral("expiration_time"), obj.value("expiration_time").toString());
    m.extras.insert(QStringLiteral("latest_expiration_time"),
                    obj.value("latest_expiration_time").toString());
    m.extras.insert(QStringLiteral("rules_secondary"), obj.value("rules_secondary").toString());
    m.extras.insert(QStringLiteral("strike_type"), obj.value("strike_type").toString());
    m.extras.insert(QStringLiteral("floor_strike"), str_or_num(obj.value("floor_strike")));
    m.extras.insert(QStringLiteral("cap_strike"), str_or_num(obj.value("cap_strike")));
    m.extras.insert(QStringLiteral("functional_strike"), obj.value("functional_strike").toString());
    m.extras.insert(QStringLiteral("custom_strike"), obj.value("custom_strike").toObject().toVariantMap());
    m.extras.insert(QStringLiteral("price_ranges"), obj.value("price_ranges").toArray().toVariantList());
    m.extras.insert(QStringLiteral("fee_waiver_expiration_time"),
                    obj.value("fee_waiver_expiration_time").toString());
    m.extras.insert(QStringLiteral("early_close_condition"),
                    obj.value("early_close_condition").toString());
    m.extras.insert(QStringLiteral("is_provisional"), obj.value("is_provisional").toBool());
    m.extras.insert(QStringLiteral("yes_bid_size_fp"), fp_to_double(obj.value("yes_bid_size_fp")));
    m.extras.insert(QStringLiteral("yes_ask_size_fp"), fp_to_double(obj.value("yes_ask_size_fp")));
    m.extras.insert(QStringLiteral("open_time"), obj.value("open_time").toString());
    m.extras.insert(QStringLiteral("created_time"), obj.value("created_time").toString());
    m.extras.insert(QStringLiteral("updated_time"), obj.value("updated_time").toString());
    m.extras.insert(QStringLiteral("notional_value_dollars"),
                    str_or_num(obj.value("notional_value_dollars")));
    m.extras.insert(QStringLiteral("previous_price_dollars"),
                    str_or_num(obj.value("previous_price_dollars")));
    m.extras.insert(QStringLiteral("previous_yes_bid_dollars"),
                    str_or_num(obj.value("previous_yes_bid_dollars")));
    m.extras.insert(QStringLiteral("previous_yes_ask_dollars"),
                    str_or_num(obj.value("previous_yes_ask_dollars")));
    m.extras.insert(QStringLiteral("price_level_structure"),
                    obj.value("price_level_structure").toString());
    m.extras.insert(QStringLiteral("mve_collection_ticker"),
                    obj.value("mve_collection_ticker").toString());
    m.extras.insert(QStringLiteral("mve_selected_legs"),
                    obj.value("mve_selected_legs").toArray().toVariantList());
    return m;
}

static pr::PredictionEvent parse_event(const QJsonObject& obj) {
    pr::PredictionEvent e;
    e.key.exchange_id = QString::fromLatin1(kExchangeId);
    e.key.event_id = obj.value("event_ticker").toString();
    e.title = obj.value("title").toString();
    if (e.title.isEmpty()) e.title = obj.value("sub_title").toString();
    e.description = obj.value("sub_title").toString();
    // Events carry a real human category ("Crypto", "Politics", …). Markets
    // do not, so this is the only place the category is trustworthy.
    e.category = obj.value("category").toString();
    // Kalshi doesn't give event-level volume; aggregate from markets below.
    e.volume = 0.0;

    // API returns "Active" (capitalised) — compare case-insensitively.
    const QString status = obj.value("status").toString().toLower();
    e.active = (status == QStringLiteral("active") || status == QStringLiteral("open"));
    e.closed = (status == QStringLiteral("closed") || status == QStringLiteral("settled"));

    const QString series = obj.value("series_ticker").toString();
    e.extras.insert(QStringLiteral("series_ticker"), series);
    e.extras.insert(QStringLiteral("mutually_exclusive"), obj.value("mutually_exclusive").toBool());
    e.extras.insert(QStringLiteral("collateral_return_type"),
                    obj.value("collateral_return_type").toString());
    e.extras.insert(QStringLiteral("strike_period"), obj.value("strike_period").toString());
    e.extras.insert(QStringLiteral("last_updated_ts"), obj.value("last_updated_ts").toString());
    e.extras.insert(QStringLiteral("available_on_brokers"),
                    obj.value("available_on_brokers").toBool());
    e.extras.insert(QStringLiteral("settlement_sources"),
                    obj.value("settlement_sources").toArray().toVariantList());
    const auto mk_arr = obj.value("markets").toArray();
    e.markets.reserve(mk_arr.size());
    for (const auto& v : mk_arr) {
        auto m = parse_market(v.toObject());
        // Event carries the series; propagate into each nested market so the
        // candlestick endpoint can look it up without heuristics.
        if (!series.isEmpty()) {
            m.extras.insert(QStringLiteral("series_ticker"), series);
        }
        // Markets have no category of their own; inherit the event's so
        // category-filtered browsing and local search can match nested markets.
        if (!e.category.isEmpty()) {
            m.category = e.category;
        }
        e.volume += m.volume;
        e.markets.push_back(m);
    }
    return e;
}

// ── KalshiRestClient ────────────────────────────────────────────────────────

KalshiRestClient::KalshiRestClient(QObject* parent) : QObject(parent) {
    nam_ = new QNetworkAccessManager(this);
}

KalshiRestClient::~KalshiRestClient() = default;

void KalshiRestClient::set_demo_mode(bool demo) { demo_ = demo; }

QString KalshiRestClient::base_url() const { return demo_ ? kDemoBase : kProdBase; }

QString KalshiRestClient::absolute_url(const QString& path) const {
    if (path.startsWith(QStringLiteral("http"))) return path;
    return base_url() + (path.startsWith('/') ? path : QStringLiteral("/") + path);
}

void KalshiRestClient::get_json(const QString& path, JsonCallback on_success, const QString& error_ctx) {
    QNetworkRequest req{QUrl(absolute_url(path))};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OpenMarketTerminal/0.1.0"));
    auto* reply = nam_->get(req);
    QPointer<KalshiRestClient> self = this;
    connect(reply, &QNetworkReply::finished, this, [self, reply, on_success, error_ctx]() {
        reply->deleteLater();
        if (!self) return;
        const auto data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            const QString msg = QStringLiteral("%1: %2").arg(reply->errorString(),
                                                             QString::fromUtf8(data.left(200)));
            emit self->request_error(error_ctx, msg);
            return;
        }
        QJsonParseError perr;
        auto doc = QJsonDocument::fromJson(data, &perr);
        if (doc.isNull()) {
            emit self->request_error(error_ctx, QStringLiteral("JSON parse: ") + perr.errorString());
            return;
        }
        on_success(doc);
    });
}

// ── fetch_markets ───────────────────────────────────────────────────────────

void KalshiRestClient::fetch_markets(const QString& status, const QString& event_ticker,
                                     const QString& series_ticker, const QString& tickers,
                                     int limit, const QString& cursor) {
    QUrlQuery q;
    if (!status.isEmpty()) q.addQueryItem("status", status);
    if (!event_ticker.isEmpty()) q.addQueryItem("event_ticker", event_ticker);
    if (!series_ticker.isEmpty()) q.addQueryItem("series_ticker", series_ticker);
    if (!tickers.isEmpty()) q.addQueryItem("tickers", tickers);
    q.addQueryItem("limit", QString::number(qBound(1, limit, 1000)));
    if (!cursor.isEmpty()) q.addQueryItem("cursor", cursor);
    QUrl u(base_url() + QStringLiteral("/markets"));
    u.setQuery(q);

    const QString series_filter = series_ticker;
    QPointer<KalshiRestClient> self = this;
    get_json(u.toString(base_url().startsWith(QStringLiteral("https")) ?
                           QUrl::FullyEncoded : QUrl::PrettyDecoded),
             [self, series_filter](const QJsonDocument& doc) {
                 if (!self) return;
                 const auto obj = doc.object();
                 QVector<pr::PredictionMarket> markets;
                 const auto arr = obj.value("markets").toArray();
                 markets.reserve(arr.size());
                 for (const auto& v : arr) {
                     auto m = parse_market(v.toObject());
                     // When the caller filtered by series_ticker the series is
                     // unambiguous, so stamp it so candlestick lookups don't
                     // need heuristic ticker parsing.
                     if (!series_filter.isEmpty()) {
                         m.extras.insert(QStringLiteral("series_ticker"), series_filter);
                     }
                     markets.push_back(m);
                 }
                 emit self->markets_ready(markets, obj.value("cursor").toString());
             },
             QStringLiteral("Kalshi.fetch_markets"));
}

void KalshiRestClient::fetch_market(const QString& ticker) {
    QPointer<KalshiRestClient> self = this;
    get_json(QStringLiteral("/markets/") + ticker,
             [self](const QJsonDocument& doc) {
                 if (!self) return;
                 const auto m = parse_market(doc.object().value("market").toObject());
                 emit self->market_detail_ready(m);
             },
             QStringLiteral("Kalshi.fetch_market"));
}

// ── fetch_events ────────────────────────────────────────────────────────────

void KalshiRestClient::fetch_events(const QString& status, const QString& series_ticker,
                                    bool with_nested_markets, int limit, const QString& cursor) {
    QUrlQuery q;
    if (!status.isEmpty()) q.addQueryItem("status", status);
    if (!series_ticker.isEmpty()) q.addQueryItem("series_ticker", series_ticker);
    q.addQueryItem("with_nested_markets", with_nested_markets ? "true" : "false");
    q.addQueryItem("limit", QString::number(qBound(1, limit, 1000)));
    if (!cursor.isEmpty()) q.addQueryItem("cursor", cursor);
    QUrl u(base_url() + QStringLiteral("/events"));
    u.setQuery(q);

    QPointer<KalshiRestClient> self = this;
    get_json(u.toString(),
             [self](const QJsonDocument& doc) {
                 if (!self) return;
                 const auto obj = doc.object();
                 QVector<pr::PredictionEvent> events;
                 const auto arr = obj.value("events").toArray();
                 events.reserve(arr.size());
                 for (const auto& v : arr) events.push_back(parse_event(v.toObject()));
                 emit self->events_ready(events, obj.value("cursor").toString());
             },
             QStringLiteral("Kalshi.fetch_events"));
}

void KalshiRestClient::fetch_event(const QString& event_ticker) {
    QPointer<KalshiRestClient> self = this;
    get_json(QStringLiteral("/events/") + event_ticker + QStringLiteral("?with_nested_markets=true"),
             [self](const QJsonDocument& doc) {
                 if (!self) return;
                 auto obj = doc.object();
                 auto ev_obj = obj.value("event").toObject();
                 // Depending on with_nested_markets, markets may live at the
                 // top level or inside the event object. Normalise.
                 if (!ev_obj.contains("markets")) {
                     ev_obj.insert("markets", obj.value("markets"));
                 }
                 emit self->event_detail_ready(parse_event(ev_obj));
             },
             QStringLiteral("Kalshi.fetch_event"));
}

// ── fetch_series (used as tags) ─────────────────────────────────────────────

void KalshiRestClient::fetch_series(const QString& status) {
    QUrlQuery q;
    if (!status.isEmpty()) q.addQueryItem("status", status);
    QUrl u(base_url() + QStringLiteral("/series"));
    u.setQuery(q);

    QPointer<KalshiRestClient> self = this;
    get_json(u.toString(),
             [self](const QJsonDocument& doc) {
                 if (!self) return;
                 const auto arr = doc.object().value("series").toArray();
                 // Kalshi's /series catalog runs to ~11k tickers — useless as a
                 // category dropdown. Emit the ~18 distinct human categories
                 // instead (Crypto, Politics, Sports, …); category selection is
                 // resolved back to series in fetch_category().
                 QStringList cats;
                 QSet<QString> seen;
                 for (const auto& v : arr) {
                     const QString c = v.toObject().value("category").toString();
                     if (c.isEmpty() || seen.contains(c)) continue;
                     seen.insert(c);
                     cats.push_back(c);
                 }
                 std::sort(cats.begin(), cats.end());
                 LOG_INFO(QStringLiteral("Kalshi.fetch_series"),
                          QStringLiteral("emitting %1 categories from %2 series")
                              .arg(cats.size())
                              .arg(arr.size()));
                 emit self->tags_ready(cats);
             },
             QStringLiteral("Kalshi.fetch_series"));
}

// ── fetch_category (category browse) ────────────────────────────────────────

static bool series_matches_keywords(const QJsonObject& obj, const QStringList& keywords) {
    if (keywords.isEmpty())
        return true;

    QStringList parts;
    parts << obj.value(QStringLiteral("ticker")).toString()
          << obj.value(QStringLiteral("title")).toString()
          << obj.value(QStringLiteral("sub_title")).toString()
          << obj.value(QStringLiteral("name")).toString();
    const QString haystack = parts.join(QLatin1Char(' ')).toLower();
    for (const QString& kw : keywords) {
        const QString needle = kw.trimmed().toLower();
        if (!needle.isEmpty() && haystack.contains(needle))
            return true;
    }
    return false;
}

void KalshiRestClient::fetch_category(const QString& category, const QStringList& frequencies,
                                      const QStringList& series_keywords,
                                      bool as_events, int limit) {
    // Kalshi's broad /markets and /events list endpoints silently ignore any
    // category/tag filter, and category-relevant rows are buried far past the
    // first page (a 1000-market page is 100% "Exotics"). The only category-aware
    // endpoint is /series?category=…, so we resolve the category to its series,
    // then fan out per-series requests and aggregate.
    if (category.isEmpty()) {
        if (as_events) emit events_ready({}, QString());
        else emit markets_ready({}, QString());
        return;
    }
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("category"), category);
    QUrl u(base_url() + QStringLiteral("/series"));
    u.setQuery(q);

    const int cap = qBound(1, limit, 500);
    QPointer<KalshiRestClient> self = this;
    get_json(u.toString(),
             [self, category, frequencies, series_keywords, as_events, cap](const QJsonDocument& doc) {
                 if (!self) return;
                 const auto arr = doc.object().value("series").toArray();
                 // Sort series by recency (last_updated_ts, ISO-8601 sorts
                 // lexically) so the most-active series come first. Active crypto
                 // series carry hundreds of markets, so the throttled fan-out
                 // early-stops after just a few requests instead of probing all
                 // ~250 series — which is what tripped Kalshi's rate limiter (429).
                 QVector<QPair<QString, QString>> by_recency;  // (ts, ticker)
                 by_recency.reserve(arr.size());
                 for (const auto& v : arr) {
                     const auto o = v.toObject();
                     const QString t = o.value("ticker").toString();
                     if (t.isEmpty()) continue;
                     // Optional cadence filter (e.g. fifteen_min / hourly crypto).
                     if (!frequencies.isEmpty() &&
                         !frequencies.contains(o.value("frequency").toString()))
                         continue;
                     if (!series_matches_keywords(o, series_keywords))
                         continue;
                     by_recency.push_back({o.value("last_updated_ts").toString(), t});
                 }
                 std::sort(by_recency.begin(), by_recency.end(),
                           [](const auto& a, const auto& b) { return a.first > b.first; });
                 QStringList series;
                 series.reserve(by_recency.size());
                 for (const auto& p : by_recency) series.push_back(p.second);
                 if (as_events) {
                     self->fan_out_series_events(series, as_events, cap);
                 } else {
                     self->fan_out_series_markets(series, category, cap);
                 }
             },
             QStringLiteral("Kalshi.fetch_category.series"));
}

void KalshiRestClient::fan_out_series_markets(const QStringList& series,
                                              const QString& category, int limit) {
    if (series.isEmpty()) {
        emit markets_ready({}, QString());
        return;
    }

    constexpr int kConcurrency = 5;
    struct State {
        QStringList series;
        int next = 0;
        int in_flight = 0;
        bool emitted = false;
        int limit = 0;
        int error_count = 0;
        QString last_error;
        QVector<pr::PredictionMarket> markets;
    };
    auto st = std::make_shared<State>();
    st->series = series;
    st->limit = limit;
    QPointer<KalshiRestClient> self = this;

    auto pump = std::make_shared<std::function<void()>>();
    *pump = [self, st, pump, category]() {
        if (!self || st->emitted) return;

        const bool enough = st->markets.size() >= st->limit;
        const bool exhausted = st->next >= st->series.size() && st->in_flight == 0;
        if (enough || exhausted) {
            st->emitted = true;
            LOG_INFO(QStringLiteral("Kalshi.fetch_category.markets"),
                     QStringLiteral("fan-out done: %1 markets from %2 series probed "
                                    "(cap %3, errors %4)")
                         .arg(st->markets.size())
                         .arg(st->next)
                         .arg(st->limit)
                         .arg(st->error_count));
            QVector<pr::PredictionMarket> out = st->markets;
            if (out.size() > st->limit) out.resize(st->limit);
            emit self->markets_ready(out, QString());
            if (out.isEmpty() && st->error_count > 0) {
                emit self->request_error(QStringLiteral("Kalshi.fetch_category.markets"),
                                         st->last_error);
            }
            return;
        }

        while (st->in_flight < kConcurrency && st->next < st->series.size() &&
               st->markets.size() < st->limit) {
            const QString s = st->series[st->next++];
            ++st->in_flight;

            QUrl u(self->base_url() + QStringLiteral("/markets"));
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("status"), QStringLiteral("open"));
            q.addQueryItem(QStringLiteral("series_ticker"), s);
            q.addQueryItem(QStringLiteral("limit"), QStringLiteral("100"));
            u.setQuery(q);

            QNetworkRequest req{QUrl(u.toString())};
            req.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("OpenMarketTerminal/0.1.0"));
            auto* reply = self->nam_->get(req);
            self->connect(reply, &QNetworkReply::finished, self,
                          [self, reply, st, pump, s, category]() {
                              reply->deleteLater();
                              --st->in_flight;
                              if (self && !st->emitted &&
                                  reply->error() == QNetworkReply::NoError) {
                                  const auto mkts = QJsonDocument::fromJson(reply->readAll())
                                                        .object()
                                                        .value("markets")
                                                        .toArray();
                                  for (const auto& v : mkts) {
                                      auto m = parse_market(v.toObject());
                                      m.extras.insert(QStringLiteral("series_ticker"), s);
                                      if (!category.isEmpty()) m.category = category;
                                      st->markets.push_back(std::move(m));
                                      if (st->markets.size() >= st->limit) break;
                                  }
                              } else if (self && !st->emitted) {
                                  ++st->error_count;
                                  const QString msg =
                                      QStringLiteral("%1: %2")
                                          .arg(reply->errorString(),
                                               QString::fromUtf8(reply->readAll().left(200)));
                                  st->last_error = msg;
                                  LOG_WARN(QStringLiteral("Kalshi.fetch_category.markets"),
                                           QStringLiteral("%1: %2").arg(s, msg));
                              }
                              (*pump)();
                          });
        }
    };
    (*pump)();
}

void KalshiRestClient::fan_out_series_events(const QStringList& series, bool as_events,
                                             int limit) {
    if (series.isEmpty()) {
        if (as_events) emit events_ready({}, QString());
        else emit markets_ready({}, QString());
        return;
    }

    // Throttled, early-stopping fan-out. `series` is recency-sorted (most-active
    // first), so we fire only a few concurrent /events requests at a time and
    // stop as soon as we've collected `limit` markets/events. This keeps us well
    // under Kalshi's read rate limit (a 250-request burst returns HTTP 429), and
    // typically completes in a handful of requests because active crypto series
    // carry hundreds of markets each.
    constexpr int kConcurrency = 5;
    struct State {
        QStringList series;
        int next = 0;
        int in_flight = 0;
        bool emitted = false;
        bool as_events = false;
        int limit = 0;
        int error_count = 0;
        QString last_error;
        QVector<pr::PredictionEvent> events;
        int market_count = 0;
    };
    auto st = std::make_shared<State>();
    st->series = series;
    st->as_events = as_events;
    st->limit = limit;
    QPointer<KalshiRestClient> self = this;

    auto collected = [](const std::shared_ptr<State>& s) {
        return s->as_events ? s->events.size() : s->market_count;
    };

    auto pump = std::make_shared<std::function<void()>>();
    *pump = [self, st, pump, collected]() {
        if (!self || st->emitted) return;

        const bool enough = collected(st) >= st->limit;
        const bool exhausted = st->next >= st->series.size() && st->in_flight == 0;
        if (enough || exhausted) {
            // Emit once. If we hit `limit` with requests still in flight, emit
            // immediately — the stragglers' replies are ignored (emitted flag).
            st->emitted = true;
            LOG_INFO(QStringLiteral("Kalshi.fetch_category"),
                     QStringLiteral("fan-out done: %1 events / %2 markets from %3 "
                                    "series probed (cap %4)")
                         .arg(st->events.size())
                         .arg(st->market_count)
                         .arg(st->next)
                         .arg(st->limit));
            if (st->as_events) {
                QVector<pr::PredictionEvent> out = st->events;
                if (out.size() > st->limit) out.resize(st->limit);
                emit self->events_ready(out, QString());
                if (out.isEmpty() && st->error_count > 0) {
                    emit self->request_error(QStringLiteral("Kalshi.fetch_category.events"),
                                             st->last_error);
                }
            } else {
                QVector<pr::PredictionMarket> mkts;
                for (const auto& e : st->events)
                    for (const auto& m : e.markets) mkts.push_back(m);
                if (mkts.size() > st->limit) mkts.resize(st->limit);
                emit self->markets_ready(mkts, QString());
                if (mkts.isEmpty() && st->error_count > 0) {
                    emit self->request_error(QStringLiteral("Kalshi.fetch_category.events"),
                                             st->last_error);
                }
            }
            return;
        }

        // Top up to the concurrency cap.
        while (st->in_flight < kConcurrency && st->next < st->series.size() &&
               collected(st) < st->limit) {
            const QString s = st->series[st->next++];
            ++st->in_flight;

            QUrl u(self->base_url() + QStringLiteral("/events"));
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("status"), QStringLiteral("open"));
            q.addQueryItem(QStringLiteral("series_ticker"), s);
            q.addQueryItem(QStringLiteral("with_nested_markets"), QStringLiteral("true"));
            q.addQueryItem(QStringLiteral("limit"), QStringLiteral("20"));
            u.setQuery(q);

            QNetworkRequest req{QUrl(u.toString())};
            req.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("OpenMarketTerminal/0.1.0"));
            auto* reply = self->nam_->get(req);
            self->connect(reply, &QNetworkReply::finished, self,
                          [self, reply, st, pump]() {
                              reply->deleteLater();
                              --st->in_flight;
                              if (self && !st->emitted &&
                                  reply->error() == QNetworkReply::NoError) {
                                  const auto evs = QJsonDocument::fromJson(reply->readAll())
                                                       .object()
                                                       .value("events")
                                                       .toArray();
                                  for (const auto& v : evs) {
                                      auto ev = parse_event(v.toObject());
                                      st->market_count += ev.markets.size();
                                      st->events.push_back(std::move(ev));
                                  }
                              } else if (self && !st->emitted) {
                                  ++st->error_count;
                                  const QString msg =
                                      QStringLiteral("%1: %2")
                                          .arg(reply->errorString(),
                                               QString::fromUtf8(reply->readAll().left(200)));
                                  st->last_error = msg;
                                  LOG_WARN(QStringLiteral("Kalshi.fetch_category.events"), msg);
                              }
                              (*pump)();
                          });
        }
    };
    (*pump)();
}

// ── fetch_order_book ────────────────────────────────────────────────────────

static void fill_book_from_levels(pr::PredictionOrderBook& book, const QJsonArray& levels_bids,
                                  const QJsonArray& opposite_bids) {
    // `levels_bids` are this side's bids (best first). Convert directly.
    for (const auto& v : levels_bids) {
        const auto arr = v.toArray();
        if (arr.size() < 2) continue;
        book.bids.push_back({str_or_num(arr[0]), fp_to_double(arr[1])});
    }
    // Asks are synthesised from the opposite side's bids:
    // "a YES ask at $X is equivalent to a NO bid at $(1 - X)."
    for (const auto& v : opposite_bids) {
        const auto arr = v.toArray();
        if (arr.size() < 2) continue;
        const double opp_price = str_or_num(arr[0]);
        book.asks.push_back({1.0 - opp_price, fp_to_double(arr[1])});
    }
    std::sort(book.bids.begin(), book.bids.end(),
              [](const auto& a, const auto& b) { return a.price > b.price; });
    std::sort(book.asks.begin(), book.asks.end(),
              [](const auto& a, const auto& b) { return a.price < b.price; });
}

void KalshiRestClient::fetch_order_book(const QString& ticker, int depth) {
    QUrlQuery q;
    if (depth > 0) q.addQueryItem("depth", QString::number(depth));
    QUrl u(base_url() + QStringLiteral("/markets/") + ticker + QStringLiteral("/orderbook"));
    u.setQuery(q);

    const QString ticker_copy = ticker;
    QPointer<KalshiRestClient> self = this;
    get_json(u.toString(),
             [self, ticker_copy](const QJsonDocument& doc) {
                 if (!self) return;
                 const auto ob = doc.object().value("orderbook_fp").toObject();
                 const auto yes_bids = ob.value("yes_dollars").toArray();
                 const auto no_bids = ob.value("no_dollars").toArray();

                 pr::PredictionOrderBook yes_book;
                 yes_book.asset_id = ticker_copy + QStringLiteral(":yes");
                 fill_book_from_levels(yes_book, yes_bids, no_bids);

                 pr::PredictionOrderBook no_book;
                 no_book.asset_id = ticker_copy + QStringLiteral(":no");
                 fill_book_from_levels(no_book, no_bids, yes_bids);

                 emit self->order_book_ready(yes_book, no_book, ticker_copy);
             },
             QStringLiteral("Kalshi.fetch_order_book"));
}

void KalshiRestClient::fetch_batch_order_books(const QStringList& tickers) {
    if (tickers.isEmpty()) {
        emit batch_order_books_ready({});
        return;
    }
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("tickers"), tickers.mid(0, 100).join(QLatin1Char(',')));
    QUrl url(base_url() + QStringLiteral("/markets/orderbooks"));
    url.setQuery(query);
    QPointer<KalshiRestClient> self = this;
    get_json(url.toString(), [self](const QJsonDocument& doc) {
        if (!self) return;
        QHash<QString, pr::PredictionOrderBook> books;
        for (const auto& value : doc.object().value(QStringLiteral("orderbooks")).toArray()) {
            const QJsonObject row = value.toObject();
            const QString ticker = row.value(QStringLiteral("ticker")).toString();
            const QJsonObject book = row.value(QStringLiteral("orderbook_fp")).toObject();
            if (ticker.isEmpty()) continue;
            const QJsonArray yes_bids = book.value(QStringLiteral("yes_dollars")).toArray();
            const QJsonArray no_bids = book.value(QStringLiteral("no_dollars")).toArray();
            pr::PredictionOrderBook yes;
            yes.asset_id = ticker + QStringLiteral(":yes");
            yes.last_update_ms = QDateTime::currentMSecsSinceEpoch();
            fill_book_from_levels(yes, yes_bids, no_bids);
            pr::PredictionOrderBook no;
            no.asset_id = ticker + QStringLiteral(":no");
            no.last_update_ms = yes.last_update_ms;
            fill_book_from_levels(no, no_bids, yes_bids);
            books.insert(yes.asset_id, yes);
            books.insert(no.asset_id, no);
        }
        emit self->batch_order_books_ready(books);
    }, QStringLiteral("Kalshi.fetch_batch_order_books"));
}

// ── fetch_candlesticks ──────────────────────────────────────────────────────

void KalshiRestClient::fetch_candlesticks(const QString& series_ticker, const QString& ticker,
                                          int period_interval_min, qint64 start_ts, qint64 end_ts) {
    QUrlQuery q;
    q.addQueryItem("period_interval", QString::number(period_interval_min));
    if (start_ts > 0) q.addQueryItem("start_ts", QString::number(start_ts));
    if (end_ts > 0) q.addQueryItem("end_ts", QString::number(end_ts));
    QUrl u(base_url() + QStringLiteral("/series/") + series_ticker +
           QStringLiteral("/markets/") + ticker + QStringLiteral("/candlesticks"));
    u.setQuery(q);

    const QString ticker_copy = ticker;
    QPointer<KalshiRestClient> self = this;
    get_json(u.toString(),
             [self, ticker_copy](const QJsonDocument& doc) {
                 if (!self) return;
                 pr::PriceHistory h;
                 h.asset_id = ticker_copy + QStringLiteral(":yes");
                 const auto arr = doc.object().value("candlesticks").toArray();
                 h.points.reserve(arr.size());
                 for (const auto& v : arr) {
                     const auto o = v.toObject();
                     const qint64 ts = qint64(o.value("end_period_ts").toDouble()) * 1000;
                     const auto price = o.value("price").toObject();
                     // Fall back to yes_bid close if no trades happened.
                     QJsonValue close = price.value("close_dollars");
                     if (close.isNull() || close.isUndefined()) {
                         const double bid = str_or_num(
                             o.value("yes_bid").toObject().value("close_dollars"));
                         const double ask = str_or_num(
                             o.value("yes_ask").toObject().value("close_dollars"));
                         const double midpoint = bid > 0.0 && ask > 0.0
                             ? (bid + ask) / 2.0 : qMax(bid, ask);
                         if (ts > 0 && midpoint > 0.0) h.points.push_back({ts, midpoint});
                         continue;
                     }
                     const double p = str_or_num(close);
                     if (ts > 0 && p > 0) h.points.push_back({ts, p});
                 }
                 emit self->price_history_ready(h, ticker_copy);
             },
             QStringLiteral("Kalshi.fetch_candlesticks"));
}

// ── fetch_market_trades ─────────────────────────────────────────────────────

void KalshiRestClient::fetch_market_trades(const QString& ticker, int limit, const QString& cursor) {
    QUrlQuery q;
    q.addQueryItem("ticker", ticker);
    q.addQueryItem("limit", QString::number(qBound(1, limit, 1000)));
    if (!cursor.isEmpty()) q.addQueryItem("cursor", cursor);
    QUrl u(base_url() + QStringLiteral("/markets/trades"));
    u.setQuery(q);

    QPointer<KalshiRestClient> self = this;
    get_json(u.toString(),
             [self](const QJsonDocument& doc) {
                 if (!self) return;
                 QVector<pr::PredictionTrade> trades;
                 const auto arr = doc.object().value("trades").toArray();
                 trades.reserve(arr.size());
                 for (const auto& v : arr) {
                     const auto o = v.toObject();
                     pr::PredictionTrade t;
                     t.asset_id = o.value("ticker").toString() + QStringLiteral(":yes");
                     // taker_side is "yes"/"no"; map to BUY/SELL for the
                     // unified shape. YES-taker = BUY of YES; NO-taker = SELL.
                     const QString ts_side = o.value("taker_side").toString().toLower();
                     t.side = (ts_side == QStringLiteral("no")) ? QStringLiteral("SELL")
                                                                 : QStringLiteral("BUY");
                     t.price = str_or_num(o.value("yes_price_dollars"));
                     t.size = fp_to_double(o.value("count_fp"));
                     // Kalshi returns ISO 8601 `created_time`; there is no
                     // epoch variant on trades.
                     const QString iso = o.value("created_time").toString();
                     t.ts_ms = QDateTime::fromString(iso, Qt::ISODate).toMSecsSinceEpoch();
                     trades.push_back(t);
                 }
                 emit self->trades_ready(trades);
             },
             QStringLiteral("Kalshi.fetch_market_trades"));
}

// ── Shared trade parser (used by market + historical trades) ───────────────

static pr::PredictionTrade parse_trade(const QJsonObject& o) {
    pr::PredictionTrade t;
    t.asset_id = o.value("ticker").toString() + QStringLiteral(":yes");
    const QString ts_side = o.value("taker_side").toString().toLower();
    t.side = (ts_side == QStringLiteral("no")) ? QStringLiteral("SELL")
                                                : QStringLiteral("BUY");
    t.price = str_or_num(o.value("yes_price_dollars"));
    t.size = fp_to_double(o.value("count_fp"));
    const QString iso = o.value("created_time").toString();
    t.ts_ms = QDateTime::fromString(iso, Qt::ISODate).toMSecsSinceEpoch();
    return t;
}

// ── Candlestick parser (shared by per-market + historical + batch) ─────────

static pr::PriceHistory parse_candlestick_history(const QJsonArray& arr,
                                                  const QString& asset_id) {
    pr::PriceHistory h;
    h.asset_id = asset_id;
    h.points.reserve(arr.size());
    for (const auto& v : arr) {
        const auto o = v.toObject();
        const qint64 ts = qint64(o.value("end_period_ts").toDouble()) * 1000;
        const auto price = o.value("price").toObject();
        QJsonValue close = price.value("close_dollars");
        if (close.isNull() || close.isUndefined()) {
            close = o.value("yes_bid").toObject().value("close_dollars");
        }
        const double p = str_or_num(close);
        if (ts > 0 && p > 0) h.points.push_back({ts, p});
    }
    return h;
}

// ── Exchange metadata ──────────────────────────────────────────────────────

void KalshiRestClient::fetch_exchange_status() {
    QPointer<KalshiRestClient> self = this;
    get_json(QStringLiteral("/exchange/status"),
             [self](const QJsonDocument& doc) {
                 if (!self) return;
                 emit self->exchange_status_ready(doc.object());
             },
             QStringLiteral("Kalshi.fetch_exchange_status"));
}

void KalshiRestClient::fetch_exchange_schedule() {
    QPointer<KalshiRestClient> self = this;
    get_json(QStringLiteral("/exchange/schedule"),
             [self](const QJsonDocument& doc) {
                 if (!self) return;
                 emit self->exchange_schedule_ready(doc.object());
             },
             QStringLiteral("Kalshi.fetch_exchange_schedule"));
}

// ── Series metadata ────────────────────────────────────────────────────────

void KalshiRestClient::fetch_series_detail(const QString& series_ticker) {
    QPointer<KalshiRestClient> self = this;
    get_json(QStringLiteral("/series/") + series_ticker,
             [self](const QJsonDocument& doc) {
                 if (!self) return;
                 // Response is wrapped as { "series": {...} }; unwrap if present.
                 const auto obj = doc.object();
                 const auto inner = obj.value("series").toObject();
                 emit self->series_detail_ready(inner.isEmpty() ? obj : inner);
             },
             QStringLiteral("Kalshi.fetch_series_detail"));
}

void KalshiRestClient::fetch_series_fee_changes() {
    QPointer<KalshiRestClient> self = this;
    get_json(QStringLiteral("/series/fee_changes"),
             [self](const QJsonDocument& doc) {
                 if (!self) return;
                 // Response is { "fee_changes": [...] } — emit the array.
                 emit self->series_fee_changes_ready(
                     doc.object().value("fee_changes").toArray());
             },
             QStringLiteral("Kalshi.fetch_series_fee_changes"));
}

// ── Batch market candlesticks ──────────────────────────────────────────────

void KalshiRestClient::fetch_batch_candlesticks(const QStringList& tickers,
                                                int period_interval_min,
                                                qint64 start_ts, qint64 end_ts) {
    if (tickers.isEmpty()) {
        emit batch_candlesticks_ready({});
        return;
    }
    QUrlQuery q;
    // Kalshi renamed this param: the batch candlesticks endpoint now requires
    // "market_tickers" (sending the old "tickers" yields HTTP 400).
    q.addQueryItem("market_tickers", tickers.join(","));
    q.addQueryItem("period_interval", QString::number(period_interval_min));
    if (start_ts > 0) q.addQueryItem("start_ts", QString::number(start_ts));
    if (end_ts > 0) q.addQueryItem("end_ts", QString::number(end_ts));
    QUrl u(base_url() + QStringLiteral("/markets/candlesticks"));
    u.setQuery(q);

    QPointer<KalshiRestClient> self = this;
    get_json(u.toString(),
             [self](const QJsonDocument& doc) {
                 if (!self) return;
                 QHash<QString, pr::PriceHistory> out;
                 // Response: { "candlesticks_by_ticker": [ { ticker, candlesticks: [...] } ] }
                 const auto arr = doc.object().value("candlesticks_by_ticker").toArray();
                 for (const auto& v : arr) {
                     const auto o = v.toObject();
                     const QString t = o.value("ticker").toString();
                     if (t.isEmpty()) continue;
                     out.insert(t, parse_candlestick_history(
                                      o.value("candlesticks").toArray(),
                                      t + QStringLiteral(":yes")));
                 }
                 emit self->batch_candlesticks_ready(out);
             },
             QStringLiteral("Kalshi.fetch_batch_candlesticks"));
}

// ── Historical (public slice) ──────────────────────────────────────────────

void KalshiRestClient::fetch_historical_markets(const QString& series_ticker,
                                                int limit, const QString& cursor) {
    QUrlQuery q;
    if (!series_ticker.isEmpty()) q.addQueryItem("series_ticker", series_ticker);
    q.addQueryItem("limit", QString::number(qBound(1, limit, 1000)));
    if (!cursor.isEmpty()) q.addQueryItem("cursor", cursor);
    QUrl u(base_url() + QStringLiteral("/historical/markets"));
    u.setQuery(q);

    QPointer<KalshiRestClient> self = this;
    get_json(u.toString(),
             [self](const QJsonDocument& doc) {
                 if (!self) return;
                 const auto obj = doc.object();
                 QVector<pr::PredictionMarket> markets;
                 const auto arr = obj.value("markets").toArray();
                 markets.reserve(arr.size());
                 for (const auto& v : arr) markets.push_back(parse_market(v.toObject()));
                 emit self->historical_markets_ready(markets, obj.value("cursor").toString());
             },
             QStringLiteral("Kalshi.fetch_historical_markets"));
}

void KalshiRestClient::fetch_historical_candlesticks(const QString& ticker,
                                                     int period_interval_min,
                                                     qint64 start_ts, qint64 end_ts) {
    QUrlQuery q;
    q.addQueryItem("period_interval", QString::number(period_interval_min));
    if (start_ts > 0) q.addQueryItem("start_ts", QString::number(start_ts));
    if (end_ts > 0) q.addQueryItem("end_ts", QString::number(end_ts));
    QUrl u(base_url() + QStringLiteral("/historical/markets/") + ticker +
           QStringLiteral("/candlesticks"));
    u.setQuery(q);

    const QString ticker_copy = ticker;
    QPointer<KalshiRestClient> self = this;
    get_json(u.toString(),
             [self, ticker_copy](const QJsonDocument& doc) {
                 if (!self) return;
                 auto h = parse_candlestick_history(
                     doc.object().value("candlesticks").toArray(),
                     ticker_copy + QStringLiteral(":yes"));
                 emit self->historical_candlesticks_ready(h, ticker_copy);
             },
             QStringLiteral("Kalshi.fetch_historical_candlesticks"));
}

void KalshiRestClient::fetch_historical_trades(const QString& ticker, int limit,
                                               const QString& cursor) {
    QUrlQuery q;
    if (!ticker.isEmpty()) q.addQueryItem("ticker", ticker);
    q.addQueryItem("limit", QString::number(qBound(1, limit, 1000)));
    if (!cursor.isEmpty()) q.addQueryItem("cursor", cursor);
    QUrl u(base_url() + QStringLiteral("/historical/trades"));
    u.setQuery(q);

    QPointer<KalshiRestClient> self = this;
    get_json(u.toString(),
             [self](const QJsonDocument& doc) {
                 if (!self) return;
                 const auto obj = doc.object();
                 QVector<pr::PredictionTrade> trades;
                 const auto arr = obj.value("trades").toArray();
                 trades.reserve(arr.size());
                 for (const auto& v : arr) trades.push_back(parse_trade(v.toObject()));
                 emit self->historical_trades_ready(trades, obj.value("cursor").toString());
             },
             QStringLiteral("Kalshi.fetch_historical_trades"));
}

// ── Search / filter metadata ───────────────────────────────────────────────

void KalshiRestClient::fetch_search_tags_by_categories() {
    QPointer<KalshiRestClient> self = this;
    get_json(QStringLiteral("/search/tags_by_categories"),
             [self](const QJsonDocument& doc) {
                 if (!self) return;
                 emit self->search_tags_ready(doc.object());
             },
             QStringLiteral("Kalshi.fetch_search_tags_by_categories"));
}

void KalshiRestClient::fetch_search_filters_by_sport(const QString& sport) {
    QUrlQuery q;
    if (!sport.isEmpty()) q.addQueryItem("sport", sport);
    QUrl u(base_url() + QStringLiteral("/search/filters_by_sport"));
    u.setQuery(q);

    QPointer<KalshiRestClient> self = this;
    get_json(u.toString(),
             [self](const QJsonDocument& doc) {
                 if (!self) return;
                 emit self->search_filters_ready(doc.object());
             },
             QStringLiteral("Kalshi.fetch_search_filters_by_sport"));
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
