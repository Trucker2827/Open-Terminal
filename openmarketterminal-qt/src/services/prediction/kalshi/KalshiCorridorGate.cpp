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

QString canonical_sha256(const QJsonObject& object) {
    const auto canonicalize = [](const auto& self, const QJsonValue& value) -> QJsonValue {
        if (value.isObject()) {
            const QJsonObject source = value.toObject();
            QStringList keys = source.keys();
            keys.sort(Qt::CaseSensitive);
            QJsonObject sorted;
            for (const QString& key : keys) sorted.insert(key, self(self, source.value(key)));
            return sorted;
        }
        if (value.isArray()) {
            QJsonArray array;
            for (const QJsonValue& item : value.toArray()) array.append(self(self, item));
            return array;
        }
        return value;
    };
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(canonicalize(canonicalize, object).toObject())
            .toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
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
        QStringLiteral("max_bundles_per_opportunity"),
        QStringLiteral("max_cost_per_opportunity_usd"),
        QStringLiteral("max_scan_age_ms")};
    QStringList unknown;
    for (auto it = raw.constBegin(); it != raw.constEnd(); ++it)
        if (!allowed.contains(it.key())) unknown.append(it.key());
    if (!unknown.isEmpty()) {
        unknown.sort();
        return fail(QStringLiteral("unknown corridor gate params [%1]")
                        .arg(unknown.join(QStringLiteral(", "))));
    }
    const auto integer = [&](const char* key, int floor, int ceiling) -> int {
        const QJsonValue value = raw.value(QString::fromLatin1(key));
        if (!value.isDouble() || value.toDouble() != std::floor(value.toDouble()) ||
            value.toInt() < floor || value.toInt() > ceiling)
            return -1;
        return value.toInt();
    };
    const int max_bundles =
        integer("max_bundles_per_opportunity", 1, kMaxBundlesCeiling);
    if (max_bundles < 0)
        return fail(QStringLiteral("max_bundles_per_opportunity must be an integer in [1, %1]")
                        .arg(kMaxBundlesCeiling));
    const auto bounded_money = [&](const char* key, double ceiling) -> double {
        const QJsonValue value = raw.value(QString::fromLatin1(key));
        return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() > 0.0 &&
                       value.toDouble() <= ceiling
                   ? value.toDouble()
                   : -1.0;
    };
    const double max_cost =
        bounded_money("max_cost_per_opportunity_usd", kMaxOpportunityCostCeiling);
    if (max_cost < 0.0)
        return fail(QStringLiteral("max_cost_per_opportunity_usd must be in (0, %1]")
                        .arg(kMaxOpportunityCostCeiling));
    const int max_age = integer("max_scan_age_ms", kMinScanAgeMs, kMaxScanAgeMs);
    if (max_age < 0)
        return fail(QStringLiteral("max_scan_age_ms must be an integer in [%1, %2]")
                        .arg(kMinScanAgeMs).arg(kMaxScanAgeMs));

    if (error) error->clear();
    return QJsonObject{{QStringLiteral("max_bundles_per_opportunity"), max_bundles},
                       {QStringLiteral("max_cost_per_opportunity_usd"), max_cost},
                       {QStringLiteral("max_scan_age_ms"), max_age}};
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
        criterion(QStringLiteral("sealed_paper_risk_envelope"),
                  QStringLiteral("immutable paper-only risk limits are valid"), 1, 1,
                  QStringLiteral("=="), true)};

    return QJsonObject{{QStringLiteral("schema"), 1},
                       {QStringLiteral("event"), QString::fromLatin1(kVerdictEvent)},
                       {QStringLiteral("strategy_family"), QString::fromLatin1(kFamily)},
                       {QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
                       {QStringLiteral("ts"), iso(now_ms)},
                       {QStringLiteral("verdict"), QString::fromLatin1(kVerdictPass)},
                       {QStringLiteral("evaluated"), true},
                       {QStringLiteral("authority"), QStringLiteral("paper_only")},
                       {QStringLiteral("paper_bids_authorized"), true},
                       {QStringLiteral("live_orders_authorized"), false},
                       {QStringLiteral("gate_id"), record.value(QStringLiteral("gate_id"))},
                       {QStringLiteral("sealed_params"), record},
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
                        QStringLiteral("sealed paper-risk envelope is active; each simulated bid still requires a fresh certified opportunity; live orders remain unavailable")}};
}

bool KalshiCorridorGate::permits_paper_bid(const QJsonObject& verdict,
                                           const QJsonObject& scan, int pair_index,
                                           int requested_bundles, qint64 now_ms,
                                           QString* reason) {
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
        return refuse(QStringLiteral("corridor paper gate is not PASS"));
    const QJsonObject sealed_params = verdict.value(QStringLiteral("sealed_params")).toObject();
    if (!seal_valid(sealed_params) ||
        sealed_params.value(QStringLiteral("strategy_family")).toString() != QLatin1String(kFamily) ||
        sealed_params.value(QStringLiteral("gate_id")) != verdict.value(QStringLiteral("gate_id")) ||
        sealed_params.value(QStringLiteral("params")) != verdict.value(QStringLiteral("params")))
        return refuse(QStringLiteral("corridor verdict does not carry its intact sealed risk envelope"));
    QString params_error;
    const QJsonObject params =
        parse_params(verdict.value(QStringLiteral("params")).toObject(), &params_error);
    if (params.isEmpty()) return refuse(QStringLiteral("corridor risk limits are invalid: %1").arg(params_error));
    if (scan.value(QStringLiteral("event")).toString() != QLatin1String(kScanEvent) ||
        scan.value(QStringLiteral("family")).toString() != QLatin1String(kFamily))
        return refuse(QStringLiteral("proposal is not a BTC corridor scan"));
    const QJsonObject certificate = scan.value(QStringLiteral("certificate")).toObject();
    const QString certificate_sha = scan.value(QStringLiteral("certificate_sha256")).toString();
    if (certificate.value(QStringLiteral("event_ticker")).toString().isEmpty() ||
        certificate_sha.isEmpty())
        return refuse(QStringLiteral("proposal has no reviewed certificate identity"));
    if (canonical_sha256(certificate) != certificate_sha)
        return refuse(QStringLiteral("proposal certificate content does not match its SHA-256"));
    const qint64 received_ms = QDateTime::fromString(
        scan.value(QStringLiteral("received_at")).toString(), Qt::ISODateWithMs).toMSecsSinceEpoch();
    if (received_ms <= 0 || received_ms > now_ms + 5'000)
        return refuse(QStringLiteral("proposal scan timestamp is invalid"));
    if (received_ms < static_cast<qint64>(verdict.value(QStringLiteral("sealed_at_ms")).toDouble()))
        return refuse(QStringLiteral("proposal predates the sealed paper experiment"));
    if (now_ms - received_ms > params.value(QStringLiteral("max_scan_age_ms")).toInt())
        return refuse(QStringLiteral("proposal scan is stale"));
    if (requested_bundles < 1 ||
        requested_bundles > params.value(QStringLiteral("max_bundles_per_opportunity")).toInt())
        return refuse(QStringLiteral("requested paper quantity exceeds the sealed limit"));
    const double quoted_quantity = number(scan.value(QStringLiteral("quantity")), -1.0);
    if (quoted_quantity != static_cast<double>(requested_bundles))
        return refuse(QStringLiteral("paper quantity was not evaluated against this book depth"));
    const QJsonArray pairs =
        scan.value(QStringLiteral("evaluation")).toObject().value(QStringLiteral("pairs")).toArray();
    if (pair_index < 0 || pair_index >= pairs.size())
        return refuse(QStringLiteral("paper opportunity index is outside the certified scan"));
    const QJsonObject pair = pairs.at(pair_index).toObject();
    const QJsonObject evaluation = pair.value(QStringLiteral("evaluation")).toObject();
    if (evaluation.value(QStringLiteral("state")).toString() != QLatin1String("opportunity"))
        return refuse(QStringLiteral("selected pair is not a certified net-positive opportunity"));
    const double acquisition = number(evaluation.value(QStringLiteral("acquisition_cost")), -1.0);
    const double fees = number(evaluation.value(QStringLiteral("fees")), -1.0);
    const double buffer = number(evaluation.value(QStringLiteral("execution_buffer")), -1.0);
    const double cost = acquisition + fees + buffer;
    if (!std::isfinite(cost) || acquisition < 0.0 || fees < 0.0 || buffer < 0.0 ||
        cost > params.value(QStringLiteral("max_cost_per_opportunity_usd")).toDouble())
        return refuse(QStringLiteral("paper opportunity exceeds its sealed cost limit"));
    if (reason) reason->clear();
    return true;
}

QJsonObject KalshiCorridorGate::parse_micro_live_params(const QJsonObject& raw,
                                                        QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return QJsonObject{};
    };
    static const QSet<QString> allowed{
        QStringLiteral("max_bundles_per_opportunity"),
        QStringLiteral("max_all_in_per_leg_usd"),
        QStringLiteral("max_scan_age_ms"),
        QStringLiteral("max_executions_per_hour"),
        QStringLiteral("series_policy_sha256")};
    QStringList unknown;
    for (auto it = raw.constBegin(); it != raw.constEnd(); ++it)
        if (!allowed.contains(it.key())) unknown.append(it.key());
    if (!unknown.isEmpty()) {
        unknown.sort();
        return fail(QStringLiteral("unknown micro-live corridor params [%1]")
                        .arg(unknown.join(QStringLiteral(", "))));
    }
    const auto integer = [&](const char* key, int floor, int ceiling) -> int {
        const QJsonValue value = raw.value(QString::fromLatin1(key));
        if (!value.isDouble() || value.toDouble() != std::floor(value.toDouble()) ||
            value.toInt() < floor || value.toInt() > ceiling)
            return -1;
        return value.toInt();
    };
    const int bundles = integer("max_bundles_per_opportunity", 1, kMaxBundlesCeiling);
    const int age = integer("max_scan_age_ms", kMinScanAgeMs, kMaxScanAgeMs);
    const int hourly = integer("max_executions_per_hour", 1,
                               kMicroLiveMaxExecutionsPerHour);
    const QJsonValue leg_cap_value = raw.value(QStringLiteral("max_all_in_per_leg_usd"));
    const double leg_cap = leg_cap_value.isDouble() ? leg_cap_value.toDouble() : -1.0;
    const QString policy_sha = raw.value(QStringLiteral("series_policy_sha256"))
                                   .toString().trimmed().toLower();
    if (bundles < 0)
        return fail(QStringLiteral("max_bundles_per_opportunity must be a bounded positive integer"));
    if (!std::isfinite(leg_cap) || leg_cap <= 0.0 ||
        leg_cap > kMicroLiveMaxAllInPerLegUsd)
        return fail(QStringLiteral("max_all_in_per_leg_usd must be in (0, 2]"));
    if (age < 0)
        return fail(QStringLiteral("max_scan_age_ms is outside the corridor freshness bounds"));
    if (hourly < 0)
        return fail(QStringLiteral("max_executions_per_hour must be in [1, 5]"));
    if (policy_sha.size() != 64 ||
        std::any_of(policy_sha.cbegin(), policy_sha.cend(), [](QChar c) {
            return !((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                     (c >= QLatin1Char('a') && c <= QLatin1Char('f')));
        }))
        return fail(QStringLiteral("series_policy_sha256 must be lowercase SHA-256"));
    if (error) error->clear();
    return QJsonObject{{QStringLiteral("max_bundles_per_opportunity"), bundles},
                       {QStringLiteral("max_all_in_per_leg_usd"), leg_cap},
                       {QStringLiteral("max_scan_age_ms"), age},
                       {QStringLiteral("max_executions_per_hour"), hourly},
                       {QStringLiteral("series_policy_sha256"), policy_sha}};
}

QJsonObject KalshiCorridorGate::preregister_micro_live(
    const QString& path, const QJsonObject& raw_params, qint64 now_ms,
    QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return QJsonObject{};
    };
    if (QFileInfo::exists(path))
        return fail(QStringLiteral("%1 already exists; micro-live corridor criteria are immutable")
                        .arg(path));
    QString parse_error;
    const QJsonObject params = parse_micro_live_params(raw_params, &parse_error);
    if (params.isEmpty()) return fail(parse_error);
    QJsonObject record{{QStringLiteral("schema"), 1},
                       {QStringLiteral("event"), QString::fromLatin1(kMicroLiveParamsEvent)},
                       {QStringLiteral("strategy_family"), QString::fromLatin1(kFamily)},
                       {QStringLiteral("authority"), QStringLiteral("micro_live_only")},
                       {QStringLiteral("production_live_authorized"), false},
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

bool KalshiCorridorGate::permits_micro_live(const QJsonValue& params_record,
                                            const QJsonObject& scan, int pair_index,
                                            int requested_bundles, qint64 now_ms,
                                            QString* reason) {
    const auto refuse = [reason](const QString& why) {
        if (reason) *reason = why;
        return false;
    };
    if (!params_record.isObject() || !seal_valid(params_record.toObject()))
        return refuse(QStringLiteral("micro-live corridor seal is missing, unreadable, or tampered"));
    const QJsonObject record = params_record.toObject();
    if (record.value(QStringLiteral("event")).toString() !=
            QLatin1String(kMicroLiveParamsEvent) ||
        record.value(QStringLiteral("strategy_family")).toString() != QLatin1String(kFamily) ||
        record.value(QStringLiteral("authority")).toString() != QLatin1String("micro_live_only") ||
        record.value(QStringLiteral("production_live_authorized")).toBool())
        return refuse(QStringLiteral("seal does not carry BTC corridor micro-live-only authority"));
    QString params_error;
    const QJsonObject params =
        parse_micro_live_params(record.value(QStringLiteral("params")).toObject(), &params_error);
    if (params.isEmpty()) return refuse(params_error);

    if (scan.value(QStringLiteral("event")).toString() != QLatin1String(kScanEvent) ||
        scan.value(QStringLiteral("family")).toString() != QLatin1String(kFamily))
        return refuse(QStringLiteral("proposal is not a BTC corridor scan"));
    const QJsonObject certificate = scan.value(QStringLiteral("certificate")).toObject();
    const QString certificate_sha = scan.value(QStringLiteral("certificate_sha256")).toString();
    if (certificate.value(QStringLiteral("event_ticker")).toString().isEmpty() ||
        certificate_sha.isEmpty() || canonical_sha256(certificate) != certificate_sha)
        return refuse(QStringLiteral("proposal has no intact reviewed certificate identity"));
    const QJsonObject policy = scan.value(QStringLiteral("series_policy")).toObject();
    const QString policy_sha = scan.value(QStringLiteral("series_policy_sha256"))
                                   .toString().trimmed().toLower();
    const QJsonObject derivation = certificate.value(QStringLiteral("derivation")).toObject();
    if (policy.isEmpty() || canonical_sha256(policy) != policy_sha ||
        policy_sha != params.value(QStringLiteral("series_policy_sha256")).toString() ||
        policy.value(QStringLiteral("authority")).toString() !=
            QLatin1String("reviewed_series_policy") ||
        policy.value(QStringLiteral("family")).toString() != QLatin1String(kFamily) ||
        policy.value(QStringLiteral("series_ticker")).toString() != QLatin1String("KXBTCD") ||
        policy.value(QStringLiteral("cadence")).toString() != QLatin1String("hourly") ||
        policy.value(QStringLiteral("rules_template")).toString() !=
            QLatin1String("kxbtcd_cf_brti_60s_above_v1") ||
        !policy.value(QStringLiteral("rules_reviewed")).toBool() ||
        certificate.value(QStringLiteral("schema_version")).toInt() != 3 ||
        certificate.value(QStringLiteral("series_ticker")).toString() != QLatin1String("KXBTCD") ||
        derivation.value(QStringLiteral("kind")).toString() !=
            QLatin1String("reviewed_series_policy") ||
        derivation.value(QStringLiteral("series_policy_sha256")).toString() != policy_sha ||
        derivation.value(QStringLiteral("rules_template")).toString() !=
            policy.value(QStringLiteral("rules_template")).toString())
        return refuse(QStringLiteral("proposal is not derived from the sealed reviewed KXBTCD hourly policy"));
    const qint64 received_ms = QDateTime::fromString(
        scan.value(QStringLiteral("received_at")).toString(), Qt::ISODateWithMs).toMSecsSinceEpoch();
    if (received_ms <= 0 || received_ms > now_ms + 5'000 ||
        received_ms < static_cast<qint64>(record.value(QStringLiteral("sealed_at_ms")).toDouble()) ||
        now_ms - received_ms > params.value(QStringLiteral("max_scan_age_ms")).toInt())
        return refuse(QStringLiteral("proposal scan is stale, future-dated, or predates the micro-live seal"));
    if (requested_bundles < 1 ||
        requested_bundles > params.value(QStringLiteral("max_bundles_per_opportunity")).toInt() ||
        number(scan.value(QStringLiteral("quantity")), -1.0) != requested_bundles)
        return refuse(QStringLiteral("micro-live quantity was not certified inside the sealed bound"));
    const QJsonArray pairs =
        scan.value(QStringLiteral("evaluation")).toObject().value(QStringLiteral("pairs")).toArray();
    if (pair_index < 0 || pair_index >= pairs.size())
        return refuse(QStringLiteral("micro-live opportunity index is outside the certified scan"));
    const QJsonObject pair = pairs.at(pair_index).toObject();
    const QJsonObject evaluation = pair.value(QStringLiteral("evaluation")).toObject();
    if (evaluation.value(QStringLiteral("state")).toString() != QLatin1String("opportunity"))
        return refuse(QStringLiteral("selected pair is not a certified net-positive opportunity"));
    const QJsonArray legs = pair.value(QStringLiteral("evaluation")).toObject()
                                .value(QStringLiteral("legs")).toArray();
    if (legs.size() != 2)
        return refuse(QStringLiteral("certified opportunity does not carry exactly two executable legs"));
    const double buffer_total = number(pair.value(QStringLiteral("evaluation")).toObject()
                                           .value(QStringLiteral("execution_buffer")), -1.0);
    if (!std::isfinite(buffer_total) || buffer_total < 0.0)
        return refuse(QStringLiteral("certified execution buffer is invalid"));
    double pair_all_in = 0.0;
    for (const QJsonValue& value : legs) {
        const QJsonObject leg = value.toObject();
        const double cost = number(leg.value(QStringLiteral("cost")), -1.0);
        const double fee = number(leg.value(QStringLiteral("fee")), -1.0);
        const double all_in = cost + fee + buffer_total / 2.0;
        if (!std::isfinite(all_in) || cost < 0.0 || fee < 0.0 ||
            all_in > params.value(QStringLiteral("max_all_in_per_leg_usd")).toDouble() + 1e-9)
            return refuse(QStringLiteral("one corridor leg exceeds its sealed $2 all-in ceiling"));
        pair_all_in += all_in;
    }
    if (pair_all_in > kMicroLiveMaxPairAllInUsd + 1e-9)
        return refuse(QStringLiteral("corridor pair exceeds the $4 micro-live backstop"));
    if (reason) reason->clear();
    return true;
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
