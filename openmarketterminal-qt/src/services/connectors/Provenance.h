#pragma once
// OpenMarketTerminal — connector provenance.
//
// Every datum the terminal shows through a connector carries a Provenance record
// so the UI can label it: WHERE it came from, whether a key was needed, WHEN it
// was fetched, whether it is live or stale, and whether it was served from cache.
//
// This is the transparency contract for the local-first connector framework:
// direct public sources only (Yahoo, Stooq, FRED, World Bank, DBnomics, SEC/
// EDGAR, NASA, HDX) — never an opaque OpenMarketTerminal aggregation API.

#include <QDateTime>
#include <QMetaType>
#include <QString>

namespace openmarketterminal::connectors {

/// Whether a connector needs an API key, and whether the user has supplied one.
enum class KeyRequirement {
    None,       ///< Fully open / keyless public source (Yahoo, Stooq, World Bank, SEC).
    Optional,   ///< Works keyless but a key raises limits (FRED, some providers).
    Required,   ///< Unusable without a key (NASA paid tiers, some HDX endpoints).
};

inline QString key_requirement_label(KeyRequirement k, bool key_present) {
    switch (k) {
        case KeyRequirement::None:     return QStringLiteral("no key required");
        case KeyRequirement::Optional: return key_present ? QStringLiteral("key configured")
                                                          : QStringLiteral("keyless (key optional)");
        case KeyRequirement::Required: return key_present ? QStringLiteral("key configured")
                                                          : QStringLiteral("key required — missing");
    }
    return {};
}

/// Provenance metadata attached to every connector result.
struct Provenance {
    QString source;          ///< Human name, e.g. "Yahoo Finance", "FRED", "SEC EDGAR".
    QString source_url;      ///< The exact public endpoint that was (or would be) hit.
    KeyRequirement key_req = KeyRequirement::None;
    bool    key_present = false;

    qint64  fetched_at_ms = 0; ///< Epoch ms when the underlying data was actually fetched.
    bool    from_cache = false; ///< Served from the local cache rather than a fresh request.
    int     ttl_seconds = 0;    ///< Freshness window used to compute `stale`.

    /// True once the cached datum is older than its TTL (i.e. should be refreshed).
    bool stale(qint64 now_ms = QDateTime::currentMSecsSinceEpoch()) const {
        if (fetched_at_ms <= 0 || ttl_seconds <= 0)
            return false;
        return (now_ms - fetched_at_ms) > static_cast<qint64>(ttl_seconds) * 1000;
    }

    /// Age of the datum in whole seconds.
    qint64 age_seconds(qint64 now_ms = QDateTime::currentMSecsSinceEpoch()) const {
        return fetched_at_ms > 0 ? (now_ms - fetched_at_ms) / 1000 : -1;
    }

    /// Local wall-clock timestamp string, e.g. "15:42:01".
    QString timestamp_label() const {
        if (fetched_at_ms <= 0)
            return QStringLiteral("—");
        return QDateTime::fromMSecsSinceEpoch(fetched_at_ms).toString(QStringLiteral("HH:mm:ss"));
    }

    /// One-line "live / stale / cached" status for the badge.
    QString freshness_label(qint64 now_ms = QDateTime::currentMSecsSinceEpoch()) const {
        if (fetched_at_ms <= 0)
            return QStringLiteral("NO DATA");
        if (stale(now_ms))
            return QStringLiteral("STALE");
        return from_cache ? QStringLiteral("CACHED") : QStringLiteral("LIVE");
    }
};

/// A typed value bundled with its provenance — the standard connector return type.
template <typename T>
struct Provenanced {
    T          data{};
    Provenance provenance;
    bool       ok = false;
    QString    error; ///< Non-empty when ok == false.
};

} // namespace openmarketterminal::connectors

Q_DECLARE_METATYPE(openmarketterminal::connectors::Provenance)
