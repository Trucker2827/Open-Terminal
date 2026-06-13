#pragma once
// OpenMarketTerminal — connector fetch core.
//
// Shared, header-only helper that every direct-public-source connector uses to
// turn an ABSOLUTE public URL into a Provenanced<QJsonDocument>. It:
//   1. Consults CacheManager(cache_key) first; on hit, reconstructs the original
//      fetch timestamp from a stored envelope so the UI can show true age.
//   2. On miss, issues GET <absolute url> via HttpClient. Because the URL is
//      absolute and points at a public host (not api.example.com), HttpClient
//      sends NO OpenMarketTerminal credentials — the request is fully independent.
//   3. Wraps the result with a Provenance record (source, key status, timestamp,
//      from_cache, ttl) for the ProvenanceBadge.
//
// Concrete connectors (YahooConnector, FredConnector, ...) are QObject and own
// their typed signals; they call ConnectorFetch::json() and map the JSON to
// their domain type, carrying the Provenance through.

#include "services/connectors/Provenance.h"
#include "storage/cache/CacheManager.h"
#include "network/http/HttpClient.h"
#include "core/result/Result.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariant>

#include <functional>

namespace openmarketterminal::connectors {

class ConnectorFetch {
  public:
    /// Fetch `url` (absolute, public) with provenance + timestamped caching.
    /// `ctx` scopes the network callback lifetime (pass the calling QObject).
    static void json(QObject* ctx, const QString& source, KeyRequirement key_req, bool key_present,
                     const QString& url, const QString& cache_key, int ttl_seconds,
                     std::function<void(Provenanced<QJsonDocument>)> cb) {
        Provenance prov;
        prov.source = source;
        prov.source_url = url;
        prov.key_req = key_req;
        prov.key_present = key_present;
        prov.ttl_seconds = ttl_seconds;

        // ── Cache hit: reconstruct payload + original fetch time ──
        if (!cache_key.isEmpty()) {
            const QVariant cached = openmarketterminal::CacheManager::instance().get(cache_key);
            if (cached.isValid()) {
                const QJsonDocument env = QJsonDocument::fromJson(cached.toByteArray());
                if (env.isObject() && env.object().contains(QStringLiteral("d"))) {
                    const QJsonObject obj = env.object();
                    Provenanced<QJsonDocument> out;
                    out.data = QJsonDocument::fromJson(obj.value(QStringLiteral("d")).toString().toUtf8());
                    prov.fetched_at_ms = static_cast<qint64>(obj.value(QStringLiteral("t")).toDouble());
                    prov.from_cache = true;
                    out.provenance = prov;
                    out.ok = true;
                    cb(std::move(out));
                    return;
                }
            }
        }

        // ── Cache miss: fetch direct from the public source ──
        QPointer<QObject> guard(ctx);
        openmarketterminal::HttpClient::instance().get(
            url,
            [cb = std::move(cb), prov, cache_key, ttl_seconds, guard](openmarketterminal::Result<QJsonDocument> result) mutable {
                if (!guard)
                    return;
                Provenanced<QJsonDocument> out;
                if (result.is_err()) {
                    out.ok = false;
                    out.error = QString::fromStdString(result.error());
                    prov.fetched_at_ms = QDateTime::currentMSecsSinceEpoch();
                    out.provenance = prov;
                    cb(std::move(out));
                    return;
                }
                out.data = result.value();
                out.ok = true;
                prov.fetched_at_ms = QDateTime::currentMSecsSinceEpoch();
                prov.from_cache = false;
                out.provenance = prov;

                // Store a timestamped envelope so later cache hits report true age.
                if (!cache_key.isEmpty() && ttl_seconds > 0) {
                    QJsonObject env;
                    env[QStringLiteral("t")] = static_cast<double>(prov.fetched_at_ms);
                    env[QStringLiteral("d")] = QString::fromUtf8(out.data.toJson(QJsonDocument::Compact));
                    openmarketterminal::CacheManager::instance().put(
                        cache_key, QVariant(QJsonDocument(env).toJson(QJsonDocument::Compact)), ttl_seconds,
                        QStringLiteral("connectors"));
                }
                cb(std::move(out));
            },
            ctx);
    }

    /// Fetch `url` (absolute, public) as raw TEXT — for CSV/plain sources like
    /// Stooq that HttpClient (JSON-only) can't handle. Same provenance + cache
    /// contract as json(). Uses an app-lifetime NAM, sends a descriptive UA.
    static void text(QObject* ctx, const QString& source, KeyRequirement key_req, bool key_present,
                     const QString& url, const QString& cache_key, int ttl_seconds,
                     std::function<void(Provenanced<QString>)> cb,
                     const QString& user_agent = QStringLiteral("OpenMarketTerminal/0.1.0")) {
        Provenance prov;
        prov.source = source;
        prov.source_url = url;
        prov.key_req = key_req;
        prov.key_present = key_present;
        prov.ttl_seconds = ttl_seconds;

        if (!cache_key.isEmpty()) {
            const QVariant cached = openmarketterminal::CacheManager::instance().get(cache_key);
            if (cached.isValid()) {
                const QJsonDocument env = QJsonDocument::fromJson(cached.toByteArray());
                if (env.isObject() && env.object().contains(QStringLiteral("d"))) {
                    const QJsonObject obj = env.object();
                    Provenanced<QString> out;
                    out.data = obj.value(QStringLiteral("d")).toString();
                    prov.fetched_at_ms = static_cast<qint64>(obj.value(QStringLiteral("t")).toDouble());
                    prov.from_cache = true;
                    out.provenance = prov;
                    out.ok = true;
                    cb(std::move(out));
                    return;
                }
            }
        }

        static QNetworkAccessManager nam; // app-lifetime
        QNetworkRequest req{QUrl(url)};
        req.setHeader(QNetworkRequest::UserAgentHeader, user_agent);
        QNetworkReply* reply = nam.get(req);
        QObject* receiver = ctx ? ctx : reply;
        QPointer<QObject> guard(ctx);
        QObject::connect(reply, &QNetworkReply::finished, receiver,
                         [reply, cb = std::move(cb), prov, cache_key, ttl_seconds, guard, ctx]() mutable {
                             reply->deleteLater();
                             if (ctx && !guard)
                                 return;
                             Provenanced<QString> out;
                             prov.fetched_at_ms = QDateTime::currentMSecsSinceEpoch();
                             if (reply->error() != QNetworkReply::NoError) {
                                 out.ok = false;
                                 out.error = reply->errorString();
                                 out.provenance = prov;
                                 cb(std::move(out));
                                 return;
                             }
                             out.data = QString::fromUtf8(reply->readAll());
                             out.ok = true;
                             prov.from_cache = false;
                             out.provenance = prov;
                             if (!cache_key.isEmpty() && ttl_seconds > 0) {
                                 QJsonObject env;
                                 env[QStringLiteral("t")] = static_cast<double>(prov.fetched_at_ms);
                                 env[QStringLiteral("d")] = out.data;
                                 openmarketterminal::CacheManager::instance().put(
                                     cache_key, QVariant(QJsonDocument(env).toJson(QJsonDocument::Compact)),
                                     ttl_seconds, QStringLiteral("connectors"));
                             }
                             cb(std::move(out));
                         });
    }
};

} // namespace openmarketterminal::connectors
