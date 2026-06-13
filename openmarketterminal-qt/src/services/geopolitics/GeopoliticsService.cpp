// src/services/geopolitics/GeopoliticsService.cpp
#include "services/geopolitics/GeopoliticsService.h"

#include "core/config/AppConfig.h"
#include "core/logging/Logger.h"
#include "network/http/HttpClient.h"
#include "python/PythonRunner.h"
#include "storage/cache/CacheManager.h"

#    include "datahub/DataHub.h"
#    include "datahub/DataHubMetaTypes.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QDateTime>
#include <QUrlQuery>
#include <QVariant>

namespace openmarketterminal::services::geo {


namespace {
inline void publish_to_hub(const QString& topic, const QVariant& value) {
    openmarketterminal::datahub::DataHub::instance().publish(topic, value);
}
}  // namespace

// ── Singleton ────────────────────────────────────────────────────────────────
GeopoliticsService& GeopoliticsService::instance() {
    static GeopoliticsService inst;
    return inst;
}

GeopoliticsService::GeopoliticsService(QObject* parent) : QObject(parent) {}

// ── Python helper ────────────────────────────────────────────────────────────
void GeopoliticsService::run_python(const QString& script, const QStringList& args, const QString& context,
                                    std::function<void(bool, const QString&)> cb) {
    QPointer<GeopoliticsService> self = this;
    python::PythonRunner::instance().run(script, args, [self, context, cb](python::PythonResult result) {
        if (!self)
            return;
        cb(result.success, result.success ? result.output : result.error);
    });
}

// ═══════════════════════════════════════════════════════════════════════════════
// CONFLICT MONITOR — HTTP API
// ═══════════════════════════════════════════════════════════════════════════════

void GeopoliticsService::fetch_events(const QString& country, const QString& city, const QString& category,
                                      int limit, int page, const QString& source,
                                      const QString& date_from, const QString& date_to) {
    // LOCAL-FIRST FORK: geopolitical events are sourced directly from the GDELT
    // Project (api.gdeltproject.org) — a free, key-less, public global news/event
    // database — instead of the removed vendor cloud. We use the
    // DOC 2.0 API in artlist mode and map each article to a NewsEvent. The actual
    // request is wrapped so the rate-limit throttle below can defer it.
    auto do_fetch = [this, country, city, category, limit, page, source, date_from, date_to]() {
    QStringList terms;
    if (!country.isEmpty())  terms << QStringLiteral("\"%1\"").arg(country);
    if (!city.isEmpty())     terms << QStringLiteral("\"%1\"").arg(city);
    if (!category.isEmpty()) terms << QStringLiteral("\"%1\"").arg(category);
    if (!source.isEmpty())   terms << QStringLiteral("domain:%1").arg(source);
    QString query = terms.join(QLatin1Char(' ')).trimmed();
    if (query.isEmpty())
        // Default geopolitics lens when no filter is applied.
        query = QStringLiteral("(sanctions OR election OR protest OR diplomacy OR conflict OR "
                               "tariff OR treaty OR military OR \"central bank\")");

    // GDELT wants YYYYMMDDHHMMSS. A date-only value must span the WHOLE day:
    // start-of-day for startdatetime, end-of-day for enddatetime — otherwise a
    // date-only enddatetime collapses to 00:00:00 and silently drops the entire
    // final day (e.g. enddatetime=today returns nothing). Strip ISO separators
    // including the 'T' so "2024-01-31T23:59:59" isn't mangled.
    auto to_gdelt_dt = [](QString d, bool end_of_day) {
        d.remove(QLatin1Char('-')).remove(QLatin1Char(':'))
            .remove(QLatin1Char(' ')).remove(QLatin1Char('T'));
        if (d.size() <= 8) {  // date-only → span the day
            while (d.size() < 8) d += QLatin1Char('0');
            d += end_of_day ? QStringLiteral("235959") : QStringLiteral("000000");
        } else {
            while (d.size() < 14) d += QLatin1Char('0');  // partial time → pad seconds
        }
        return d.left(14);  // YYYYMMDDHHMMSS
    };

    QUrl url(QStringLiteral("https://api.gdeltproject.org/api/v2/doc/doc"));
    QUrlQuery q;
    q.addQueryItem("query", query);
    q.addQueryItem("mode", "artlist");
    q.addQueryItem("format", "json");
    q.addQueryItem("sort", "datedesc");
    q.addQueryItem("maxrecords", QString::number(qBound(1, limit, 250)));
    if (!date_from.isEmpty()) q.addQueryItem("startdatetime", to_gdelt_dt(date_from, /*end_of_day=*/false));
    if (!date_to.isEmpty())   q.addQueryItem("enddatetime", to_gdelt_dt(date_to, /*end_of_day=*/true));
    url.setQuery(q);

    QPointer<GeopoliticsService> self = this;
    HttpClient::instance().get(
        url.toString(),
        [self, country, city, category, limit, page](Result<QJsonDocument> result) {
            if (!self)
                return;
            if (!result.is_ok()) {
                LOG_ERROR("Geopolitics", "GDELT fetch failed: " + QString::fromStdString(result.error()));
                emit self->error_occurred("events", QString::fromStdString(result.error()));
                return;
            }
            const auto root = result.value().object();
            const auto arr  = root["articles"].toArray();

            EventsPage page_data;
            page_data.events.reserve(arr.size());
            for (const auto& v : arr) {
                const auto a = v.toObject();
                NewsEvent ev;
                ev.url            = a["url"].toString();
                ev.source         = a["domain"].toString();
                ev.event_category = category;  // echo the active filter
                ev.title          = a["title"].toString();
                ev.city           = city;
                // GDELT "sourcecountry" is the PUBLISHER's country, not the event
                // location, so using it as ev.country mislabels events (a Ukraine
                // story from a US outlet would read "US"). When the user filtered
                // by country the result set IS about that country, so echo the
                // filter; otherwise leave it blank rather than assert a wrong country.
                ev.country        = country;   // empty when no country filter applied
                ev.has_coords     = false;     // DOC API has no coordinates
                ev.latitude       = 0.0;
                ev.longitude      = 0.0;
                // GDELT seendate: "YYYYMMDDTHHMMSSZ" → "YYYY-MM-DD HH:MM:SS"
                const QString sd = a["seendate"].toString();
                ev.extracted_date = (sd.size() >= 15)
                    ? QStringLiteral("%1-%2-%3 %4:%5:%6")
                          .arg(sd.mid(0, 4), sd.mid(4, 2), sd.mid(6, 2),
                               sd.mid(9, 2), sd.mid(11, 2), sd.mid(13, 2))
                    : sd;
                ev.created_at = ev.extracted_date;
                page_data.events.append(ev);
            }

            // Newest first — extracted_date is "YYYY-MM-DD HH:MM:SS" so plain
            // string comparison is lexicographically correct.
            std::sort(page_data.events.begin(), page_data.events.end(),
                      [](const NewsEvent& a, const NewsEvent& b) {
                          return a.extracted_date > b.extracted_date;
                      });

            // GDELT DOC returns a single result set (no server pagination).
            page_data.total_events    = page_data.events.size();
            page_data.current_page    = page;
            page_data.total_pages     = 1;
            page_data.events_per_page = limit;
            page_data.has_next        = false;
            page_data.has_prev        = false;
            page_data.credits_used      = 0.0;  // GDELT is free
            page_data.remaining_credits = -1;

            // Cache the (already-sorted) events for offline replay.
            QJsonArray cached_arr;
            for (const auto& ev : page_data.events) {
                QJsonObject o;
                o["url"]            = ev.url;
                o["source"]         = ev.source;
                o["event_category"] = ev.event_category;
                o["title"]          = ev.title;
                o["city"]           = ev.city;
                o["country"]        = ev.country;
                if (ev.has_coords) {
                    o["latitude"]   = ev.latitude;
                    o["longitude"]  = ev.longitude;
                }
                o["extracted_date"] = ev.extracted_date;
                o["created_at"]     = ev.created_at;
                cached_arr.append(o);
            }
            QJsonObject cached_root;
            cached_root["events"]            = cached_arr;
            cached_root["total_events"]      = page_data.total_events;
            cached_root["current_page"]      = page_data.current_page;
            cached_root["total_pages"]       = page_data.total_pages;
            cached_root["events_per_page"]   = page_data.events_per_page;
            cached_root["has_next"]          = page_data.has_next;
            cached_root["has_prev"]          = page_data.has_prev;
            cached_root["credits_used"]      = page_data.credits_used;
            cached_root["remaining_credits"] = page_data.remaining_credits;
            const QString cache_key = QString("geo:events:%1:%2:%3:%4:%5")
                                          .arg(country, city, category)
                                          .arg(limit)
                                          .arg(page);
            openmarketterminal::CacheManager::instance().put(
                cache_key,
                QVariant(QJsonDocument(cached_root).toJson(QJsonDocument::Compact)),
                kEventsTtlSec, "geopolitics");

            LOG_INFO("Geopolitics", QString("Loaded %1 events (page %2/%3, total %4, %5 credits left)")
                                        .arg(page_data.events.size())
                                        .arg(page_data.current_page)
                                        .arg(page_data.total_pages)
                                        .arg(page_data.total_events)
                                        .arg(page_data.remaining_credits));

            emit self->events_loaded(page_data);
            if (self->hub_registered_)
                publish_to_hub(QStringLiteral("geopolitics:events"), QVariant::fromValue(page_data));
        });
    };  // end do_fetch

    // GDELT's free DOC API rate-limits to ~1 request / 5 s; exceeding it returns
    // a plain-text notice (non-JSON) and empty results. Run immediately if the
    // last call is old enough, otherwise coalesce to this latest request and
    // fire it once the cooldown elapses.
    constexpr qint64 kMinGapMs = 5200;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsed = now - last_gdelt_ms_;
    if (last_gdelt_ms_ == 0 || elapsed >= kMinGapMs) {
        last_gdelt_ms_ = now;
        do_fetch();
        return;
    }
    pending_gdelt_ = do_fetch;
    if (!gdelt_timer_) {
        gdelt_timer_ = new QTimer(this);
        gdelt_timer_->setSingleShot(true);
        connect(gdelt_timer_, &QTimer::timeout, this, [this]() {
            last_gdelt_ms_ = QDateTime::currentMSecsSinceEpoch();
            if (pending_gdelt_) {
                auto f = pending_gdelt_;
                pending_gdelt_ = nullptr;
                f();
            }
        });
    }
    gdelt_timer_->start(static_cast<int>(kMinGapMs - elapsed));
}

// LOCAL-FIRST FORK: the filter dropdowns are curated static lists (no cloud
// call). Selecting a country/category/city just adds it as a GDELT search term
// in fetch_events(), so filtering still works against the free GDELT feed.
void GeopoliticsService::fetch_unique_countries() {
    static const char* kCountries[] = {
        "United States", "China", "Russia", "Ukraine", "Israel", "Iran", "India",
        "United Kingdom", "France", "Germany", "Japan", "North Korea", "South Korea",
        "Taiwan", "Saudi Arabia", "Turkey", "Pakistan", "Brazil", "Venezuela", "Syria",
        "Yemen", "Afghanistan", "Egypt", "Nigeria", "South Africa", "Mexico", "Canada",
        "Australia", "Italy", "Spain", "Poland", "European Union"};
    QVector<UniqueCountry> countries;
    for (auto* c : kCountries)
        countries.append({QString::fromLatin1(c), 0});
    emit countries_loaded(countries);
}

void GeopoliticsService::fetch_unique_categories() {
    static const char* kCats[] = {
        "Conflict", "Military", "Sanctions", "Elections", "Diplomacy", "Protest",
        "Trade", "Energy", "Cyber", "Terrorism", "Economy", "Nuclear", "Migration",
        "Climate", "Health"};
    QVector<UniqueCategory> cats;
    for (auto* c : kCats)
        cats.append({QString::fromLatin1(c), 0});
    emit categories_loaded(cats);
}

void GeopoliticsService::fetch_unique_cities() {
    static const char* kCities[] = {
        "Washington", "Moscow", "Kyiv", "Beijing", "Jerusalem", "Tehran", "London",
        "Paris", "Berlin", "Tokyo", "New Delhi", "Seoul", "Taipei", "Gaza", "Brussels",
        "Geneva", "Istanbul", "Riyadh", "Hong Kong", "Davos"};
    QStringList cities;
    for (auto* c : kCities)
        cities.append(QString::fromLatin1(c));
    emit cities_loaded(cities);
}

// ═══════════════════════════════════════════════════════════════════════════════
// HDX HUMANITARIAN DATA — Python
// ═══════════════════════════════════════════════════════════════════════════════

static QVector<HDXDataset> parse_hdx_results(const QString& output) {
    auto json_str = python::extract_json(output);
    auto doc = QJsonDocument::fromJson(json_str.toUtf8());
    QVector<HDXDataset> datasets;

    // Script returns: {"success": true, "data": {"datasets": [...]}}
    // Try unwrapping the data envelope first, then fall back to bare array/object
    auto root = doc.object();
    QJsonArray arr;
    if (root.contains("data")) {
        auto data = root["data"].toObject();
        arr = data["datasets"].toArray();
    } else if (root.contains("datasets")) {
        arr = root["datasets"].toArray();
    } else {
        arr = doc.array();
    }

    datasets.reserve(arr.size());
    for (const auto& v : arr) {
        auto o = v.toObject();
        HDXDataset d;
        d.id = o["id"].toString();
        d.title = o["title"].toString();
        d.organization = o["organization"].toString();
        d.notes = o["notes"].toString();
        d.date = o["dataset_date"].toString();
        d.num_resources = o["num_resources"].toInt();
        // Tags in summary are plain strings; in full detail they are objects with "name"
        for (const auto& t : o["tags"].toArray()) {
            if (t.isString())
                d.tags.append(t.toString());
            else
                d.tags.append(t.toObject()["name"].toString());
        }
        d.raw = o;
        datasets.append(d);
    }
    return datasets;
}

void GeopoliticsService::search_hdx_conflicts() {
    run_python("hdx_data.py", {"search_conflict", "", "20"}, "hdx_conflicts", [this](bool ok, const QString& out) {
        if (!ok) {
            emit error_occurred("hdx_conflicts", out);
            return;
        }
        const auto datasets = parse_hdx_results(out);
        emit hdx_results_loaded("conflicts", datasets);
        publish_hdx_result(QStringLiteral("conflicts"), datasets);
    });
}

void GeopoliticsService::search_hdx_humanitarian() {
    run_python("hdx_data.py", {"search_humanitarian", "", "20"}, "hdx_humanitarian",
               [this](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("hdx_humanitarian", out);
                       return;
                   }
                   const auto datasets = parse_hdx_results(out);
                   emit hdx_results_loaded("humanitarian", datasets);
                   publish_hdx_result(QStringLiteral("humanitarian"), datasets);
               });
}

void GeopoliticsService::search_hdx_by_country(const QString& country) {
    run_python("hdx_data.py", {"search_by_country", country}, "hdx_country", [this, country](bool ok, const QString& out) {
        if (!ok) {
            emit error_occurred("hdx_country", out);
            return;
        }
        const auto datasets = parse_hdx_results(out);
        emit hdx_results_loaded("country", datasets);
        publish_hdx_result(QStringLiteral("country:") + country, datasets);
    });
}

void GeopoliticsService::search_hdx_by_topic(const QString& topic) {
    run_python("hdx_data.py", {"search_by_topic", topic}, "hdx_topic", [this, topic](bool ok, const QString& out) {
        if (!ok) {
            emit error_occurred("hdx_topic", out);
            return;
        }
        const auto datasets = parse_hdx_results(out);
        emit hdx_results_loaded("topic", datasets);
        publish_hdx_result(QStringLiteral("topic:") + topic, datasets);
    });
}

void GeopoliticsService::search_hdx_advanced(const QString& query) {
    // Use search_datasets for free-text queries (advanced_search expects key:value pairs)
    run_python("hdx_data.py", {"search_datasets", query, "20"}, "hdx_search",
               [this, query](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("hdx_search", out);
                       return;
                   }
                   const auto datasets = parse_hdx_results(out);
                   emit hdx_results_loaded("search", datasets);
                   publish_hdx_result(QStringLiteral("search:") + query, datasets);
               });
}

// ═══════════════════════════════════════════════════════════════════════════════
// TRADE ANALYSIS — Python
// ═══════════════════════════════════════════════════════════════════════════════

void GeopoliticsService::analyze_trade_benefits(const QJsonObject& params) {
    auto json_str = QJsonDocument(params).toJson(QJsonDocument::Compact);
    run_python("Analytics/economics/trade_geopolitics.py", {"benefits_costs", json_str}, "trade_benefits",
               [this](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("trade_benefits", out);
                       return;
                   }
                   auto doc = QJsonDocument::fromJson(python::extract_json(out).toUtf8());
                   const auto obj = doc.object();
                   emit trade_result_ready("trade_benefits", obj);
                   if (hub_registered_)
                       publish_to_hub(QStringLiteral("geopolitics:trade:benefits"), QVariant(obj));
               });
}

void GeopoliticsService::analyze_trade_restrictions(const QJsonObject& params) {
    auto json_str = QJsonDocument(params).toJson(QJsonDocument::Compact);
    run_python("Analytics/economics/trade_geopolitics.py", {"restrictions", json_str}, "trade_restrictions",
               [this](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("trade_restrictions", out);
                       return;
                   }
                   auto doc = QJsonDocument::fromJson(python::extract_json(out).toUtf8());
                   const auto obj = doc.object();
                   emit trade_result_ready("trade_restrictions", obj);
                   if (hub_registered_)
                       publish_to_hub(QStringLiteral("geopolitics:trade:restrictions"), QVariant(obj));
               });
}

// ═══════════════════════════════════════════════════════════════════════════════
// GEOLOCATION — Python
// ═══════════════════════════════════════════════════════════════════════════════

void GeopoliticsService::extract_geolocations(const QStringList& headlines) {
    auto json = QJsonDocument(QJsonArray::fromStringList(headlines)).toJson(QJsonDocument::Compact);
    run_python("news_geolocation.py", {"extract_and_geocode", json}, "geolocation",
               [this](bool ok, const QString& out) {
                   if (!ok) {
                       emit error_occurred("geolocation", out);
                       return;
                   }
                   auto doc = QJsonDocument::fromJson(python::extract_json(out).toUtf8());
                   const auto obj = doc.object();
                   emit geolocation_ready(obj);
                   if (hub_registered_)
                       publish_to_hub(QStringLiteral("geopolitics:geolocation"), QVariant(obj));
               });
}

// ═══════════════════════════════════════════════════════════════════════════════
// DATAHUB PRODUCER — geopolitics:*
// ═══════════════════════════════════════════════════════════════════════════════

void GeopoliticsService::publish_hdx_result(const QString& context, const QVector<HDXDataset>& datasets) {
    if (!hub_registered_) return;
    publish_to_hub(QStringLiteral("geopolitics:hdx:") + context, QVariant::fromValue(datasets));
}

QStringList GeopoliticsService::topic_patterns() const {
    return {QStringLiteral("geopolitics:*")};
}

void GeopoliticsService::refresh(const QStringList& topics) {
    // Hub-driven refresh for the lightweight reference endpoints. Anything
    // parameterised (events:<filters>, hdx:*, trade:*, geolocation) is
    // user-invoked and not driven through the hub scheduler.
    for (const auto& topic : topics) {
        if (topic == QStringLiteral("geopolitics:events")) {
            fetch_events();
        } else if (topic == QStringLiteral("geopolitics:countries")) {
            fetch_unique_countries();
        } else if (topic == QStringLiteral("geopolitics:categories")) {
            fetch_unique_categories();
        } else if (topic == QStringLiteral("geopolitics:cities")) {
            fetch_unique_cities();
        }
    }
}

int GeopoliticsService::max_requests_per_sec() const {
    return 2;  // OpenMarketTerminal research API + HDX Python — conservative
}

void GeopoliticsService::ensure_registered_with_hub() {
    if (hub_registered_) return;
    auto& hub = openmarketterminal::datahub::DataHub::instance();
    hub.register_producer(this);

    // Events — 2 min TTL (matches kEventsTtlSec), 30s min_interval.
    openmarketterminal::datahub::TopicPolicy events_policy;
    events_policy.ttl_ms = kEventsTtlSec * 1000;
    events_policy.min_interval_ms = 30 * 1000;
    hub.set_policy_pattern(QStringLiteral("geopolitics:events"), events_policy);

    // Reference data (countries/categories/cities) — 10 min TTL.
    openmarketterminal::datahub::TopicPolicy ref_policy;
    ref_policy.ttl_ms = kRefDataTtlSec * 1000;
    ref_policy.min_interval_ms = 60 * 1000;
    hub.set_policy_pattern(QStringLiteral("geopolitics:countries"), ref_policy);
    hub.set_policy_pattern(QStringLiteral("geopolitics:categories"), ref_policy);
    hub.set_policy_pattern(QStringLiteral("geopolitics:cities"), ref_policy);

    // HDX datasets — 1 hour TTL (humanitarian data refresh cadence).
    openmarketterminal::datahub::TopicPolicy hdx_policy;
    hdx_policy.ttl_ms = 60 * 60 * 1000;
    hdx_policy.min_interval_ms = 60 * 1000;
    hub.set_policy_pattern(QStringLiteral("geopolitics:hdx:*"), hdx_policy);

    // Trade analysis + geolocation — user-invoked, treat as push-only so the
    // hub caches the most recent result without scheduling a refresh.
    openmarketterminal::datahub::TopicPolicy push_policy;
    push_policy.push_only = true;
    push_policy.ttl_ms = 15 * 60 * 1000;
    hub.set_policy_pattern(QStringLiteral("geopolitics:trade:*"), push_policy);
    hub.set_policy_pattern(QStringLiteral("geopolitics:geolocation"), push_policy);

    hub_registered_ = true;
    LOG_INFO("GeopoliticsService", "Registered with DataHub (geopolitics:*)");
}

} // namespace openmarketterminal::services::geo
