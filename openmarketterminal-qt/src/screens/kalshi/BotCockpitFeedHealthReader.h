#pragma once

// Pure core of BOT COCKPIT feed-health parsing over an already-loaded
// kalshi-ws-engine.json object. Split out of KalshiBotCockpitView.cpp so it
// is unit-testable without pulling in QWidget/QPainter (issue: HARVEST must
// key off a MARKET-data timestamp, not the CF-Benchmarks-conflated
// `last_event_at`).
//
// `last_event_at` is bumped by BOTH `observe_market_event` (real Kalshi
// market data) AND `ws_cf_benchmark_event` (the CF-Benchmarks BRTI index), so
// a silent tradeable feed sitting beside a still-ticking BRTI stream would
// report a falsely fresh age if HARVEST read it. `last_market_event_at` is
// written ONLY from the market-data path, so it is the honest clock for
// HARVEST freshness.

#include "screens/kalshi/BotCockpitPresentation.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace openmarketterminal::screens::kalshi {

/// `readable` is whether the caller could parse the engine file as JSON at
/// all (false => the whole feed reads UNKNOWN/grey; no I/O happens here).
/// `fallback_age_ms` is the age to use when the engine carries no
/// `last_market_event_at` of its own (e.g. an older engine build, or one that
/// has connected but not yet ticked) — the caller supplies the
/// kalshi-tickers.jsonl tail age, or -1 when neither is available.
inline BotCockpitFeedHealth parse_bot_cockpit_feed_health(const QJsonObject& engine, bool readable,
                                                           qint64 now_ms, qint64 fallback_age_ms) {
    BotCockpitFeedHealth feed;
    feed.readable = readable;
    if (!readable) return feed;

    feed.ws_connected = engine.value(QStringLiteral("connected")).toBool();
    feed.credentials_ok = engine.value(QStringLiteral("credentials")).toBool();
    feed.last_error = engine.value(QStringLiteral("last_error")).toString();

    const QDateTime last_market_event = QDateTime::fromString(
        engine.value(QStringLiteral("last_market_event_at")).toString(), Qt::ISODateWithMs);
    feed.newest_event_age_ms = last_market_event.isValid()
        ? qMax<qint64>(0, now_ms - last_market_event.toMSecsSinceEpoch())
        : fallback_age_ms;
    return feed;
}

} // namespace openmarketterminal::screens::kalshi
