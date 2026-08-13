#include "services/prediction/kalshi/KalshiCorridorGate.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QTimeZone>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::services::prediction::kalshi_ns {
namespace {

constexpr auto kSealField = "seal_sha256";
constexpr auto kParamsEvent = "kalshi_btc_threshold_corridor_gate_params";
constexpr auto kVerdictEvent = "kalshi_btc_threshold_corridor_gate";

QString iso(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

QJsonObject refusal(const char* verdict, const QString& reason, qint64 now_ms) {
    return QJsonObject{{QStringLiteral("schema"), 1},
                       {QStringLiteral("event"), QString::fromLatin1(kVerdictEvent)},
                       {QStringLiteral("strategy_family"),
                        QString::fromLatin1(KalshiCorridorGate::kFamily)},
                       {QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
                       {QStringLiteral("ts"), iso(now_ms)},
                       {QStringLiteral("verdict"), QString::fromLatin1(verdict)},
                       {QStringLiteral("evaluated"), false},
                       {QStringLiteral("authority"), QStringLiteral("paper_only")},
                       {QStringLiteral("paper_bids_authorized"), false},
                       {QStringLiteral("live_orders_authorized"), false},
                       {QStringLiteral("reason"), reason}};
}

QJsonObject criterion(const QString& id, const QString& description, double observed,
                      double required, const QString& comparison, bool met) {
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("description"), description},
                       {QStringLiteral("observed"), observed},
                       {QStringLiteral("required"), required},
                       {QStringLiteral("comparison"), comparison},
                       {QStringLiteral("met"), met}};
}

double number(const QJsonValue& value, double fallback = 0.0) {
    if (value.isDouble()) return value.toDouble();
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().toDouble(&ok);
        if (ok) return parsed;
    }
    return fallback;
}

} // namespace

QString KalshiCorridorGate::seal(const QJsonObject& record) {
    QJsonObject canonical = record;
    canonical.remove(QString::fromLatin1(kSealField));
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(canonical).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}

bool KalshiCorridorGate::seal_valid(const QJsonObject& record) {
    const QJsonValue stored = record.value(QString::fromLatin1(kSealField));
    return stored.isString() && !stored.toString().isEmpty() && stored.toString() == seal(record);
}

QJsonObject KalshiCorridorGate::parse_params(const QJsonObject& raw, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return QJsonObject{};
    };
    static const QSet<QString> allowed{
        QStringLiteral("min_scans"), QStringLiteral("min_distinct_events"),
        QStringLiteral("min_opportunity_scans"), QStringLiteral("min_opportunity_events"),
        QStringLiteral("min_best_net_edge_usd"),
        QStringLiteral("max_unavailable_rate")};
    QStringList unknown;
    for (auto it = raw.constBegin(); it != raw.constEnd(); ++it)
        if (!allowed.contains(it.key())) unknown.append(it.key());
    if (!unknown.isEmpty()) {
        unknown.sort();
        return fail(QStringLiteral("unknown corridor gate params [%1]")
                        .arg(unknown.join(QStringLiteral(", "))));
    }
    const auto integer = [&](const char* key, int floor) -> int {
        const QJsonValue value = raw.value(QString::fromLatin1(key));
        if (!value.isDouble() || value.toDouble() != std::floor(value.toDouble()) ||
            value.toInt() < floor)
            return -1;
        return value.toInt();
    };
    const int min_scans = integer("min_scans", kMinScansFloor);
    if (min_scans < 0)
        return fail(QStringLiteral("min_scans must be an integer >= %1").arg(kMinScansFloor));
    const int min_events = integer("min_distinct_events", kMinDistinctEventsFloor);
    if (min_events < 0)
        return fail(QStringLiteral("min_distinct_events must be an integer >= %1")
                        .arg(kMinDistinctEventsFloor));
    const int min_opportunities = integer("min_opportunity_scans", kMinOpportunityScansFloor);
    if (min_opportunities < 0)
        return fail(QStringLiteral("min_opportunity_scans must be an integer >= %1")
                        .arg(kMinOpportunityScansFloor));
    const int min_opportunity_events =
        integer("min_opportunity_events", kMinOpportunityEventsFloor);
    if (min_opportunity_events < 0)
        return fail(QStringLiteral("min_opportunity_events must be an integer >= %1")
                        .arg(kMinOpportunityEventsFloor));
    const QJsonValue edge = raw.value(QStringLiteral("min_best_net_edge_usd"));
    if (!edge.isDouble() || edge.toDouble() < 0.0 || edge.toDouble() > 1.0)
        return fail(QStringLiteral("min_best_net_edge_usd must be a number in [0, 1]"));
    const QJsonValue unavailable = raw.value(QStringLiteral("max_unavailable_rate"));
    if (!unavailable.isDouble() || unavailable.toDouble() < 0.0 ||
        unavailable.toDouble() > kMaxUnavailableRateCeiling)
        return fail(QStringLiteral("max_unavailable_rate must be a number in [0, %1]")
                        .arg(kMaxUnavailableRateCeiling, 0, 'f', 2));

    if (error) error->clear();
    return QJsonObject{{QStringLiteral("min_scans"), min_scans},
                       {QStringLiteral("min_distinct_events"), min_events},
                       {QStringLiteral("min_opportunity_scans"), min_opportunities},
                       {QStringLiteral("min_opportunity_events"), min_opportunity_events},
                       {QStringLiteral("min_best_net_edge_usd"), edge},
                       {QStringLiteral("max_unavailable_rate"), unavailable}};
}

QJsonObject KalshiCorridorGate::preregister(const QString& path,
                                            const QJsonObject& raw_params,
                                            qint64 now_ms, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return QJsonObject{};
    };
    if (QFileInfo::exists(path))
        return fail(QStringLiteral("%1 already exists; corridor gate criteria are immutable")
                        .arg(path));
    QString parse_error;
    const QJsonObject params = parse_params(raw_params, &parse_error);
    if (params.isEmpty()) return fail(parse_error);
    QJsonObject record{{QStringLiteral("schema"), 1},
                       {QStringLiteral("event"), QString::fromLatin1(kParamsEvent)},
                       {QStringLiteral("strategy_family"), QString::fromLatin1(kFamily)},
                       {QStringLiteral("gate_id"),
                        QUuid::createUuid().toString(QUuid::WithoutBraces)},
                       {QStringLiteral("sealed_at_ms"), static_cast<double>(now_ms)},
                       {QStringLiteral("sealed_at"), iso(now_ms)},
                       {QStringLiteral("params"), params}};
    record.insert(QString::fromLatin1(kSealField), seal(record));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return fail(QStringLiteral("cannot write %1: %2").arg(path, file.errorString()));
    file.write(QJsonDocument(record).toJson(QJsonDocument::Indented));
    if (!file.commit())
        return fail(QStringLiteral("cannot write %1: %2").arg(path, file.errorString()));
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadGroup |
                                    QFileDevice::ReadOther);
    if (error) error->clear();
    return record;
}

QJsonValue KalshiCorridorGate::load_params_file(const QString& path) {
    QFile file(path);
    if (!file.exists()) return QJsonValue(QJsonValue::Undefined);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QJsonValue(false);
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? QJsonValue(document.object()) : QJsonValue(false);
}

QJsonObject KalshiCorridorGate::evaluate(const QJsonValue& params_record,
                                         const QJsonArray& evidence_rows,
                                         qint64 now_ms) {
    if (params_record.isUndefined() || params_record.isNull())
        return refusal(kVerdictNotPreregistered,
                       QStringLiteral("no separate corridor gate was preregistered"), now_ms);
    if (!params_record.isObject() || !seal_valid(params_record.toObject()))
        return refusal(kVerdictTampered,
                       QStringLiteral("corridor gate params are unreadable or fail their seal"),
                       now_ms);
    const QJsonObject record = params_record.toObject();
    if (record.value(QStringLiteral("strategy_family")).toString() != QLatin1String(kFamily))
        return refusal(kVerdictTampered,
                       QStringLiteral("sealed gate belongs to another strategy family"), now_ms);
    QString parse_error;
    const QJsonObject params =
        parse_params(record.value(QStringLiteral("params")).toObject(), &parse_error);
    if (params.isEmpty()) return refusal(kVerdictTampered, parse_error, now_ms);
    const qint64 sealed_at_ms =
        static_cast<qint64>(record.value(QStringLiteral("sealed_at_ms")).toDouble());
    if (sealed_at_ms <= 0)
        return refusal(kVerdictTampered, QStringLiteral("sealed_at_ms is missing"), now_ms);

    int scans = 0;
    int unavailable = 0;
    int opportunity_scans = 0;
    int malformed = 0;
    int before_seal = 0;
    int pairs_evaluated = 0;
    double best_edge = 0.0;
    QSet<QString> events;
    QSet<QString> opportunity_events;
    for (const QJsonValue& value : evidence_rows) {
        if (!value.isObject()) { ++malformed; continue; }
        const QJsonObject row = value.toObject();
        if (row.value(QStringLiteral("event")).toString() != QLatin1String(kScanEvent) ||
            row.value(QStringLiteral("family")).toString() != QLatin1String(kFamily))
            continue;
        const qint64 received_ms = QDateTime::fromString(
            row.value(QStringLiteral("received_at")).toString(), Qt::ISODateWithMs).toMSecsSinceEpoch();
        if (received_ms <= 0) { ++malformed; continue; }
        if (received_ms < sealed_at_ms) { ++before_seal; continue; }
        ++scans;
        const QJsonObject certificate = row.value(QStringLiteral("certificate")).toObject();
        const QString certificate_sha =
            row.value(QStringLiteral("certificate_sha256")).toString();
        const QString event = certificate.value(QStringLiteral("event_ticker")).toString();
        if (certificate.isEmpty() || certificate_sha.isEmpty() || event.isEmpty()) {
            ++malformed;
            ++unavailable;
            continue;
        }
        const QJsonObject evaluation = row.value(QStringLiteral("evaluation")).toObject();
        const QString state = evaluation.value(QStringLiteral("state")).toString();
        if (evaluation.isEmpty() || state.isEmpty()) { ++malformed; ++unavailable; continue; }
        if (state == QLatin1String("unavailable")) ++unavailable;
        if (state == QLatin1String("opportunity")) {
            ++opportunity_scans;
            opportunity_events.insert(event);
        }
        pairs_evaluated += evaluation.value(QStringLiteral("pairs_evaluated")).toInt();
        events.insert(event);
        for (const QJsonValue& pair_value : evaluation.value(QStringLiteral("pairs")).toArray()) {
            const QJsonObject pair = pair_value.toObject();
            const QJsonObject result = pair.value(QStringLiteral("evaluation")).toObject();
            if (result.value(QStringLiteral("state")).toString() == QLatin1String("opportunity"))
                best_edge = std::max(best_edge,
                                     number(result.value(QStringLiteral("net_edge_per_bundle"))));
        }
    }
    const double unavailable_rate = scans > 0
        ? static_cast<double>(unavailable) / static_cast<double>(scans) : 1.0;
    const QJsonArray criteria{
        criterion(QStringLiteral("min_scans"), QStringLiteral("certificate-backed scans"), scans,
                  params.value(QStringLiteral("min_scans")).toInt(), QStringLiteral(">="),
                  scans >= params.value(QStringLiteral("min_scans")).toInt()),
        criterion(QStringLiteral("min_distinct_events"),
                  QStringLiteral("distinct reviewed hourly expirations"), events.size(),
                  params.value(QStringLiteral("min_distinct_events")).toInt(),
                  QStringLiteral(">="),
                  events.size() >= params.value(QStringLiteral("min_distinct_events")).toInt()),
        criterion(QStringLiteral("min_opportunity_scans"),
                  QStringLiteral("scans with executable edge after fees and buffer"),
                  opportunity_scans,
                  params.value(QStringLiteral("min_opportunity_scans")).toInt(),
                  QStringLiteral(">="), opportunity_scans >=
                      params.value(QStringLiteral("min_opportunity_scans")).toInt()),
        criterion(QStringLiteral("min_opportunity_events"),
                  QStringLiteral("distinct reviewed expirations with executable edge"),
                  opportunity_events.size(),
                  params.value(QStringLiteral("min_opportunity_events")).toInt(),
                  QStringLiteral(">="), opportunity_events.size() >=
                      params.value(QStringLiteral("min_opportunity_events")).toInt()),
        criterion(QStringLiteral("min_best_net_edge_usd"),
                  QStringLiteral("best certified net edge per bundle"), best_edge,
                  params.value(QStringLiteral("min_best_net_edge_usd")).toDouble(),
                  QStringLiteral(">="), best_edge >=
                      params.value(QStringLiteral("min_best_net_edge_usd")).toDouble()),
        criterion(QStringLiteral("max_unavailable_rate"),
                  QStringLiteral("fraction of scans unavailable"), unavailable_rate,
                  params.value(QStringLiteral("max_unavailable_rate")).toDouble(),
                  QStringLiteral("<="), unavailable_rate <=
                      params.value(QStringLiteral("max_unavailable_rate")).toDouble())};
    bool all_met = true;
    for (const QJsonValue& value : criteria)
        if (!value.toObject().value(QStringLiteral("met")).toBool()) all_met = false;

    return QJsonObject{{QStringLiteral("schema"), 1},
                       {QStringLiteral("event"), QString::fromLatin1(kVerdictEvent)},
                       {QStringLiteral("strategy_family"), QString::fromLatin1(kFamily)},
                       {QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
                       {QStringLiteral("ts"), iso(now_ms)},
                       {QStringLiteral("verdict"),
                        QString::fromLatin1(all_met ? kVerdictPass : kVerdictFail)},
                       {QStringLiteral("evaluated"), true},
                       {QStringLiteral("authority"), QStringLiteral("paper_only")},
                       {QStringLiteral("paper_bids_authorized"), all_met},
                       {QStringLiteral("live_orders_authorized"), false},
                       {QStringLiteral("gate_id"), record.value(QStringLiteral("gate_id"))},
                       {QStringLiteral("sealed_at_ms"),
                        record.value(QStringLiteral("sealed_at_ms"))},
                       {QStringLiteral("seal_sha256"),
                        record.value(QString::fromLatin1(kSealField))},
                       {QStringLiteral("params"), params},
                       {QStringLiteral("criteria"), criteria},
                       {QStringLiteral("evidence"),
                        QJsonObject{{QStringLiteral("scans"), scans},
                                    {QStringLiteral("distinct_events"), events.size()},
                                    {QStringLiteral("opportunity_scans"), opportunity_scans},
                                    {QStringLiteral("opportunity_events"), opportunity_events.size()},
                                    {QStringLiteral("unavailable_scans"), unavailable},
                                    {QStringLiteral("unavailable_rate"), unavailable_rate},
                                    {QStringLiteral("malformed_rows"), malformed},
                                    {QStringLiteral("rows_before_seal"), before_seal},
                                    {QStringLiteral("pairs_evaluated"), pairs_evaluated},
                                    {QStringLiteral("best_net_edge_usd"), best_edge}}},
                       {QStringLiteral("reason"),
                        all_met
                            ? QStringLiteral("paper corridor evidence clears every sealed criterion; live orders remain unavailable")
                            : QStringLiteral("corridor evidence has not cleared every sealed criterion")}};
}

bool KalshiCorridorGate::permits_paper_bid(const QJsonObject& verdict, QString* reason) {
    const auto refuse = [reason](const QString& why) {
        if (reason) *reason = why;
        return false;
    };
    if (verdict.value(QStringLiteral("strategy_family")).toString() != QLatin1String(kFamily))
        return refuse(QStringLiteral("verdict is not the BTC corridor gate"));
    if (verdict.value(QStringLiteral("authority")).toString() != QLatin1String("paper_only"))
        return refuse(QStringLiteral("corridor authority is not paper_only"));
    if (verdict.value(QStringLiteral("live_orders_authorized")).toBool())
        return refuse(QStringLiteral("corridor live-order authority is forbidden"));
    if (!verdict.value(QStringLiteral("paper_bids_authorized")).toBool())
        return refuse(QStringLiteral("corridor paper-bid authority is absent"));
    if (!verdict.value(QStringLiteral("evaluated")).toBool() ||
        verdict.value(QStringLiteral("verdict")).toString() != QLatin1String(kVerdictPass))
        return refuse(QStringLiteral("corridor evidence gate is not PASS"));
    if (reason) reason->clear();
    return true;
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
