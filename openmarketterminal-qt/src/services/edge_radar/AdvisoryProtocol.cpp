#include "services/edge_radar/AdvisoryProtocol.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

namespace openmarketterminal::adv {

namespace {

QByteArray canonical_value(const QJsonValue& value);

// Escapes a string exactly the way Qt's own JSON writer would, by
// round-tripping it through a single-element array and stripping the
// surrounding brackets. Keeps the escaping rules (unicode, quotes,
// backslashes) in one place instead of reimplementing them.
QByteArray canonical_escaped_string(const QString& s) {
    QJsonArray wrap;
    wrap.append(s);
    const QByteArray compact = QJsonDocument(wrap).toJson(QJsonDocument::Compact);
    // compact is exactly "[" + escaped-string + "]"
    return compact.mid(1, compact.size() - 2);
}

QByteArray canonical_array(const QJsonArray& arr) {
    QByteArray out = "[";
    bool first = true;
    for (const QJsonValue& v : arr) {
        if (!first) out += ",";
        first = false;
        out += canonical_value(v);
    }
    out += "]";
    return out;
}

QByteArray canonical_object_body(const QJsonObject& obj) {
    QByteArray out = "{";
    QStringList keys = obj.keys();
    std::sort(keys.begin(), keys.end());
    bool first = true;
    for (const QString& key : keys) {
        if (!first) out += ",";
        first = false;
        out += canonical_escaped_string(key);
        out += ":";
        out += canonical_value(obj.value(key));
    }
    out += "}";
    return out;
}

QByteArray canonical_value(const QJsonValue& value) {
    switch (value.type()) {
    case QJsonValue::Bool:
        return value.toBool() ? "true" : "false";
    case QJsonValue::Double:
        // Fixed 6-decimal-place formatting: deterministic across locales and
        // across re-serialization, so equal numeric values always hash the
        // same regardless of how they were originally typed (int vs double).
        return QString::number(value.toDouble(), 'f', 6).toUtf8();
    case QJsonValue::String:
        return canonical_escaped_string(value.toString());
    case QJsonValue::Array:
        return canonical_array(value.toArray());
    case QJsonValue::Object:
        return canonical_object_body(value.toObject());
    case QJsonValue::Null:
    case QJsonValue::Undefined:
    default:
        return "null";
    }
}

} // namespace

QByteArray canonical_json(const QJsonObject& obj) {
    return canonical_object_body(obj);
}

QString sha256_hex(const QByteArray& bytes) {
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

TtlPolicy ttl_for(qint64 seconds_left, qint64 configured_max_ms) {
    TtlPolicy policy;

    // Too close to settlement to open a new advisory challenge at all: the
    // forecaster would have no meaningful window to reason before the
    // market resolves.
    if (seconds_left <= 60) {
        policy.may_open = false;
        policy.prediction_ttl_ms = 0;
        policy.execution_relevance_ms = 0;
        return policy;
    }

    policy.may_open = true;

    // Horizon -> base prediction TTL: shorter horizons get shorter TTLs
    // (predictions decay faster the closer to settlement), longer horizons
    // get progressively longer TTLs, capped by configured_max_ms.
    qint64 base_ms;
    if (seconds_left <= 300) {          // 1-5 minutes
        base_ms = 15000;
    } else if (seconds_left <= 900) {   // 5-15 minutes
        base_ms = 30000;
    } else if (seconds_left <= 3600) {  // 15-60 minutes
        base_ms = 45000;
    } else {                            // > 60 minutes
        base_ms = 60000;
    }

    policy.prediction_ttl_ms = std::min(base_ms, configured_max_ms);
    policy.execution_relevance_ms =
        std::min(policy.prediction_ttl_ms / 2, qint64(15000));
    return policy;
}

QStringList kBlindForbiddenKeys() {
    return {
        "yes_bid", "yes_ask", "no_bid", "no_ask", "yes_depth", "no_depth",
        "market_implied_probability", "market_curve_probability", "fair_yes", "fair_no",
        "divergence", "daemon_probability", "calibrated_probability", "model_probability",
        "model_weight", "cost_net_edge", "execution"
    };
}

QJsonObject build_blind_packet(const QJsonObject& snapshot) {
    // Allowlist-only copy: iterate the fields we intend to reveal and pull
    // them from `snapshot` if present. Never iterate `snapshot` and strip
    // forbidden keys — that pattern silently leaks any new field added to
    // the snapshot in the future.
    static const QStringList kAllowlist = {
        "strike_floor", "strike_cap", "distance_bps", "required_move_bps", "seconds_left",
        "settlement_band", "settlement_def", "horizon", "spot", "realized_move_bps",
        "realized_vol", "spot_microstructure"
    };

    QJsonObject blind;
    for (const QString& key : kAllowlist) {
        if (snapshot.contains(key))
            blind.insert(key, snapshot.value(key));
    }
    return blind;
}

} // namespace openmarketterminal::adv
