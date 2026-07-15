#include "cli/CryptoFeedHub.h"

#include <QJsonObject>
#include <QSet>

namespace openmarketterminal::cli {

CryptoFeedHub::CryptoFeedHub(QObject* parent) : QObject(parent) {}

CryptoFeedHub::~CryptoFeedHub() {
    // Services are QObject children (parented to the hub) so Qt deletes them,
    // and ~CryptoLatencyService calls stop(). Stop explicitly here too so feeds
    // tear down deterministically before the child destructors run.
    for (auto it = services_.begin(); it != services_.end(); ++it) {
        if (it.value())
            it.value()->stop();
    }
}

void CryptoFeedHub::ensure_symbol(const QString& symbol, const QStringList& sources) {
    const QString normalized = CryptoLatencyService::normalize_symbol(symbol);

    auto svc_it = services_.find(normalized);
    if (svc_it == services_.end()) {
        // First request for this symbol: dedupe the requested sources, create a
        // service owned by the hub, relay its ticks, and start it.
        QStringList requested;
        QSet<QString> seen;
        for (const QString& raw : sources) {
            const QString src = raw.trimmed().toLower();
            if (src.isEmpty())
                continue;
            if (!seen.contains(src)) {
                seen.insert(src);
                requested.append(src);
            }
        }

        auto* svc = new CryptoLatencyService(this);
        services_.insert(normalized, svc);
        requested_sources_.insert(normalized, requested);

        QObject::connect(svc, &CryptoLatencyService::tick_received, this,
                         [this, normalized](const CryptoLatencyTick& tick) {
                             emit tick_received(normalized, tick);
                         });
        svc->start(normalized, requested);
        return;
    }

    // Service already exists: merge the requested sources with the ones already
    // requested for this symbol. Only restart when the set actually grows.
    QStringList& existing = requested_sources_[normalized];
    QSet<QString> seen(existing.cbegin(), existing.cend());
    bool grew = false;
    for (const QString& raw : sources) {
        const QString src = raw.trimmed().toLower();
        if (src.isEmpty())
            continue;
        if (!seen.contains(src)) {
            seen.insert(src);
            existing.append(src);
            grew = true;
        }
    }

    if (grew && svc_it.value()) {
        // start() calls stop() internally, so re-starting with the union is safe.
        svc_it.value()->start(normalized, existing);
    }
}

CryptoFeedHub::CryptoLatencySnapshot CryptoFeedHub::snapshot(const QString& symbol) const {
    const QString normalized = CryptoLatencyService::normalize_symbol(symbol);
    auto it = services_.constFind(normalized);
    if (it == services_.constEnd() || !it.value())
        return CryptoLatencySnapshot{};
    return it.value()->snapshot();
}

QJsonArray CryptoFeedHub::feed_health() const {
    QJsonArray out;
    for (auto it = services_.constBegin(); it != services_.constEnd(); ++it) {
        if (!it.value())
            continue;
        const CryptoLatencySnapshot snap = it.value()->snapshot();
        for (const auto& state : snap.sources) {
            QJsonObject obj = CryptoLatencyService::source_to_json(state);
            obj.insert(QStringLiteral("symbol"), it.key());
            out.append(obj);
        }
    }
    return out;
}

} // namespace openmarketterminal::cli
