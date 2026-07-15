#pragma once

#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include "services/crypto_latency/CryptoLatencyService.h"

namespace openmarketterminal::cli {

// Owns exactly one CryptoLatencyService per (normalized) union symbol so that
// multiple daemon engines can share a single upstream WebSocket connection per
// venue instead of each opening their own (which trips exchange rate limits,
// e.g. Kraken HTTP 429). Read-only feeds only — no order/credential paths.
class CryptoFeedHub : public QObject {
    Q_OBJECT
  public:
    using CryptoLatencyService = services::crypto_latency::CryptoLatencyService;
    using CryptoLatencyTick = services::crypto_latency::CryptoLatencyTick;
    using CryptoLatencySnapshot = services::crypto_latency::CryptoLatencySnapshot;

    explicit CryptoFeedHub(QObject* parent = nullptr);
    ~CryptoFeedHub() override;

    // Idempotent. Normalizes `symbol`. Creates a service (parented to the hub)
    // for it if none exists and starts it with `sources`; otherwise merges
    // `sources` into the set already requested for that symbol and only
    // restarts the service when the requested set actually grew.
    void ensure_symbol(const QString& symbol, const QStringList& sources);

    // Snapshot for the (normalized) symbol, or a default-constructed snapshot
    // if the hub owns no service for it.
    CryptoLatencySnapshot snapshot(const QString& symbol) const;

    // One JSON object per (owned symbol, source-state) — each is
    // CryptoLatencyService::source_to_json(state) with an added "symbol" key.
    QJsonArray feed_health() const;

  signals:
    void tick_received(const QString& symbol,
                       const openmarketterminal::services::crypto_latency::CryptoLatencyTick& tick);

  private:
    QHash<QString, CryptoLatencyService*> services_;
    QHash<QString, QStringList> requested_sources_;
};

} // namespace openmarketterminal::cli
