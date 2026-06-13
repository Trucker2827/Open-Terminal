#include "services/economics/MacroCalendarService.h"

#include "core/logging/Logger.h"
#include "datahub/DataHub.h"
#include "datahub/TopicPolicy.h"
#include "network/http/HttpClient.h"
#include "storage/repositories/SettingsRepository.h"

#include <QDate>
#include <QDateTime>
#include <QTimeZone>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>

namespace openmarketterminal::services {

namespace {
constexpr const char* kTopic = "econ:openmarketterminal:upcoming_events";

// LOCAL-FIRST FORK: the macro calendar is sourced directly from FRED (St. Louis
// Fed) — a free, transparent public API — instead of the removed vendor
// cloud. FRED's /releases/dates gives the upcoming US economic-data release
// schedule (release name + date). It does not carry forecast/actual values
// (those need a paid calendar provider), so those fields are left blank.
QString fred_key() {
    auto r = openmarketterminal::SettingsRepository::instance().get(
        QStringLiteral("connectors.fred_api_key"));
    return r.is_ok() ? r.value().trimmed() : QString();
}

// Map FRED release-dates → the event shape EconomicCalendarWidget expects
// (event, country, date, time, actual, forecast, previous, sentiment, importance).
QJsonArray parse_fred_releases(const QJsonDocument& doc) {
    QJsonArray out;
    const auto arr = doc.object().value(QStringLiteral("release_dates")).toArray();
    // FRED release dates are US (Eastern) calendar dates. Anchor "today" to
    // US/Eastern so a user in a timezone ahead of the US (e.g. Asia, whose local
    // date has already rolled over) doesn't filter out the current US release
    // day — that would silently drop the nearest market-moving event. Fall back
    // to local-date-minus-one-day (over-include by a day, never drop) if the
    // zone database is unavailable.
    const QTimeZone us_east("America/New_York");
    const QDate today_date = us_east.isValid()
                                 ? QDateTime::currentDateTimeUtc().toTimeZone(us_east).date()
                                 : QDate::currentDate().addDays(-1);
    const QString today = today_date.toString(Qt::ISODate);
    for (const auto& v : arr) {
        const auto o = v.toObject();
        const QString date = o.value(QStringLiteral("date")).toString();
        if (date < today)  // ISO dates sort lexically — keep upcoming only
            continue;
        const QString name = o.value(QStringLiteral("release_name")).toString();
        // Heuristic importance from the release name (FRED has no impact field).
        const QString n = name.toLower();
        int imp = 1;
        if (n.contains("employment") || n.contains("consumer price") || n.contains("gross domestic") ||
            n.contains("payroll") || n.contains("fomc") || n.contains("federal funds") ||
            n.contains("personal income") || n.contains("retail sales") || n.contains("pce") ||
            n.contains("producer price") || n.contains("gdp"))
            imp = 3;
        else if (n.contains("housing") || n.contains("manufactur") || n.contains("trade") ||
                 n.contains("industrial production") || n.contains("sentiment") || n.contains("claims") ||
                 n.contains("durable") || n.contains("confidence") || n.contains("ism") ||
                 n.contains("job openings") || n.contains("beige") || n.contains("treasury budget"))
            imp = 2;
        // Skip the high-frequency minor noise (daily index/quote releases) so the
        // calendar surfaces market-moving events, not "Dow Jones Averages" daily.
        if (imp < 2)
            continue;

        QJsonObject e;
        e["event"] = name;
        e["country"] = QStringLiteral("US");
        e["date"] = date;
        e["time"] = QString();
        e["actual"] = QString();
        e["forecast"] = QString();
        e["previous"] = QString();
        e["sentiment"] = QString();
        e["importance"] = imp;
        out.append(e);
        if (out.size() >= 25)
            break;
    }
    return out;
}
} // namespace

MacroCalendarService& MacroCalendarService::instance() {
    static MacroCalendarService s;
    return s;
}

MacroCalendarService::MacroCalendarService(QObject* parent) : QObject(parent) {}

void MacroCalendarService::ensure_registered_with_hub() {
    if (hub_registered_)
        return;
    auto& hub = openmarketterminal::datahub::DataHub::instance();
    hub.register_producer(this);

    openmarketterminal::datahub::TopicPolicy policy;
    policy.ttl_ms = 5 * 60 * 1000;   // 5 min — macro events refresh slowly
    policy.min_interval_ms = 60 * 1000; // 60 s
    policy.refresh_timeout_ms = 30 * 1000;
    hub.set_policy(QString::fromLatin1(kTopic), policy);

    hub_registered_ = true;
    LOG_INFO("MacroCalendarService", "Registered with DataHub (econ:openmarketterminal:upcoming_events)");
}

QStringList MacroCalendarService::topic_patterns() const {
    return {QString::fromLatin1(kTopic)};
}

void MacroCalendarService::refresh(const QStringList& topics) {
    // Single-topic producer — the hub may pass the topic list redundantly,
    // but there's only ever one fetch to do.
    if (!topics.contains(QString::fromLatin1(kTopic)))
        return;

    auto& hub = openmarketterminal::datahub::DataHub::instance();
    const QString key = fred_key();
    if (key.isEmpty()) {
        hub.publish_error(
            QString::fromLatin1(kTopic),
            QStringLiteral("FRED API key required — get a free key at "
                           "fredaccount.stlouisfed.org and set connectors.fred_api_key in Settings."));
        return;
    }

    // FRED /releases/dates: upcoming US economic-data release schedule. Direct,
    // free, transparent — no vendor cloud involved.
    const QString today = QDate::currentDate().toString(Qt::ISODate);
    // realtime_end must extend into the future, otherwise FRED excludes
    // future-scheduled release dates (whose realtime period starts later).
    // include_release_dates_with_no_data=true surfaces those scheduled dates.
    const QString url =
        QStringLiteral("https://api.stlouisfed.org/fred/releases/dates"
                       "?api_key=%1&file_type=json&sort_order=asc"
                       "&include_release_dates_with_no_data=true"
                       "&realtime_start=%2&realtime_end=9999-12-31&limit=400")
            .arg(key, today);

    QPointer<MacroCalendarService> self = this;
    HttpClient::instance().get(
        url,
        [self](Result<QJsonDocument> result) {
            if (!self)
                return;
            auto& hub = openmarketterminal::datahub::DataHub::instance();
            if (!result.is_ok()) {
                const QString msg = QString::fromStdString(result.error());
                LOG_WARN("MacroCalendarService", msg);
                hub.publish_error(QString::fromLatin1(kTopic), msg);
                return;
            }
            const QJsonArray events = parse_fred_releases(result.value());
            LOG_INFO("MacroCalendarService",
                     QStringLiteral("FRED: published %1 upcoming release events").arg(events.size()));
            hub.publish(QString::fromLatin1(kTopic), QVariant::fromValue(events));
        },
        this);
}

} // namespace openmarketterminal::services
