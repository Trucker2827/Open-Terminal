#pragma once

#include <QJsonObject>
#include <QString>

namespace openmarketterminal::services::sandbox {

// Field presence for the data_quality rule: a freshness field counts as
// PRESENT only when the key exists AND its value is a number or a
// numeric string (some journal producers write freshest_age_ms both ways).
// A key holding garbage (non-numeric string, null, object) is treated as
// absent -- there is no usable signal in it.
inline bool freshness_field(const QJsonObject& o, const QString& key, double* out) {
    if (!o.contains(key))
        return false;
    const QJsonValue v = o.value(key);
    if (v.isDouble()) {
        *out = v.toDouble();
        return true;
    }
    if (v.isString()) {
        bool ok = false;
        const double parsed = v.toString().toDouble(&ok);
        if (ok)
            *out = parsed;
        return ok;
    }
    return false;
}

// data_quality, three-way (controller decision, task-5 review fix 4):
//   - 'unknown'  when the row provides NEITHER freshest_age_ms NOR
//     live_sources -- absence of freshness telemetry is not evidence of
//     degradation (prediction journal rows, e.g. btc5m/kalshi, carry
//     neither and would otherwise be permanently 'degraded').
//   - 'degraded' iff (freshest_age_ms present AND > 5000) OR (live_sources
//     present AND < 2) -- each field is only evaluated when present, so a
//     row with live_sources:1 and no age field is correctly degraded (the
//     old unconditional-|| logic defaulted a missing age to 0 and a missing
//     live_sources to 0 as well, which happened to work for missing
//     live_sources but silently passed a missing-age/bad-sources mix).
//   - 'ok' otherwise.
// Works over a freshness_json object (spot/btc5m/kalshi/long_short journal
// rows) or a scalp decision row's top level -- both use the same key names.
inline QString data_quality_from_freshness(const QJsonObject& freshness) {
    double freshest_age_ms = 0;
    double live_sources = 0;
    const bool has_age = freshness_field(freshness, QStringLiteral("freshest_age_ms"), &freshest_age_ms);
    const bool has_sources = freshness_field(freshness, QStringLiteral("live_sources"), &live_sources);
    if (!has_age && !has_sources)
        return QStringLiteral("unknown");
    if ((has_age && freshest_age_ms > 5000.0) || (has_sources && live_sources < 2.0))
        return QStringLiteral("degraded");
    return QStringLiteral("ok");
}

} // namespace openmarketterminal::services::sandbox
