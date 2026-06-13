#include "trading/instruments/InstrumentService.h"

#include "core/logging/Logger.h"
#include "storage/sqlite/Database.h"
#include "trading/brokers/BrokerHttp.h"
#include "trading/instruments/InstrumentNormalize.h"
#include "trading/instruments/InstrumentRepository.h"
#include "trading/instruments/InstrumentSources.h"
#include "trading/instruments/SymbolResolver.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QEvent>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMetaObject>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>

namespace openmarketterminal::trading {

namespace {


} // namespace

namespace {

/// Instrument-master sources are registered per broker with SymbolResolver.
/// The kept brokers (Alpaca, IBKR, Tradier, Saxo, MT4) trade plain symbols and
/// need no downloaded token master, so no source is registered here. Left as a
/// seam for any future broker that requires an instrument master.
void ensure_builtin_sources_registered() {
}

} // namespace

InstrumentService& InstrumentService::instance() {
    static InstrumentService inst;
    ensure_builtin_sources_registered();
    return inst;
}

// ── Public API ────────────────────────────────────────────────────────────────

void InstrumentService::refresh(const QString& broker_id, const BrokerCredentials& creds, int max_age_hours) {
    // Age-based freshness gate. max_age_hours <= 0 disables it (always download).
    // Timestamp source: MAX(updated_at) on the instruments table (set on every
    // full refresh via DELETE+INSERT).
    if (max_age_hours > 0) {
        const QDateTime last = InstrumentRepository::instance().last_updated(broker_id);
        if (last.isValid()) {
            const qint64 age_hours = last.secsTo(QDateTime::currentDateTimeUtc()) / 3600;
            if (age_hours >= 0 && age_hours < max_age_hours) {
                LOG_INFO("InstrumentService",
                         QString("%1 instruments are fresh (%2h old < %3h) — skipping download")
                             .arg(broker_id)
                             .arg(age_hours)
                             .arg(max_age_hours));
                // Instruments on disk are fresh but may not be in the in-memory
                // cache yet (e.g. first refresh() of a relaunch within
                // max_age_hours). Without this, is_loaded() stays false forever
                // and every lookup fails. load_from_db() uses the shared
                // main-thread QSqlDatabase connection; refresh() is only ever
                // called from the UI/main thread (ChainSubTab + EquityTradingScreen
                // callbacks), so this is safe here.
                if (!is_loaded(broker_id))
                    load_from_db(broker_id);
                return;
            }
        }
    }
    // For AngelOne we need NSE equities to be present — check specifically for NSE count.
    // A partial cache (e.g. only NFO/MCX) means a previous download was incomplete.
    {
        QMutexLocker lock(&mutex_);
        if (caches_.contains(broker_id) && caches_[broker_id].loaded) {
            const Cache& c = caches_[broker_id];
            // Count NSE equity entries (exchange == "NSE", no expiry = spot equity)
            int nse_count = 0;
            for (auto it = c.by_symbol.constBegin(); it != c.by_symbol.constEnd(); ++it) {
                if (it.key().second == "NSE" && it->expiry.isEmpty())
                    ++nse_count;
            }
            if (nse_count > 100) {
                LOG_INFO("InstrumentService",
                         QString("%1 instruments already loaded (%2 NSE equities), skipping refresh")
                             .arg(broker_id)
                             .arg(nse_count));
                return;
            }
            LOG_WARN(
                "InstrumentService",
                QString("%1 cache incomplete (only %2 NSE equities) — re-downloading").arg(broker_id).arg(nse_count));
        }
    }
    force_refresh(broker_id, creds);
}

void InstrumentService::force_refresh(const QString& broker_id, const BrokerCredentials& creds) {
    {
        QMutexLocker lock(&mutex_);
        if (refreshing_.contains(broker_id)) {
            LOG_INFO("InstrumentService",
                     QString("%1 refresh already in progress — skipping duplicate").arg(broker_id));
            return;
        }
        refreshing_.insert(broker_id);
    }
    emit refresh_started(broker_id);
    QPointer<InstrumentService> self = this;
    (void)QtConcurrent::run([self, broker_id, creds]() {
        if (!self)
            return;
        self->do_refresh(broker_id, creds);
    });
}

void InstrumentService::load_from_db(const QString& broker_id) {
    // Called at startup before QApplication::exec() — runs synchronously on
    // the main thread so that the single QSqlDatabase connection is not
    // accessed concurrently (QSqlDatabase is not thread-safe).
    QVector<Instrument> all;
    for (const QString& exch : QStringList{"NSE", "BSE", "NFO", "CDS", "MCX", "NSE_INDEX", "BSE_INDEX", "BFO", "BCD"}) {
        auto rows = InstrumentRepository::instance().list(exch, broker_id);
        all.append(rows);
    }
    if (all.isEmpty()) {
        LOG_WARN("InstrumentService", QString("No instruments found in DB for %1 — run refresh").arg(broker_id));
        return;
    }
    build_cache(broker_id, all);
    LOG_INFO("InstrumentService", QString("Loaded %1 instruments from DB for %2").arg(all.size()).arg(broker_id));
}

void InstrumentService::load_from_db_async(const QString& broker_id,
                                            std::function<void(int)> callback) {
    // Fast path: if already loaded, fire callback immediately on UI thread.
    if (is_loaded(broker_id)) {
        int n = cached_count(broker_id);
        LOG_INFO("InstrumentService",
                 QString("load_from_db_async: %1 already loaded (%2 instruments)").arg(broker_id).arg(n));
        if (callback)
            callback(n);
        return;
    }

    LOG_INFO("InstrumentService", QString("load_from_db_async: starting background load for %1").arg(broker_id));

    QPointer<InstrumentService> self = this;
    QString db_path = openmarketterminal::Database::instance().path(); // read path on UI thread before going async

    (void)QtConcurrent::run([self, broker_id, db_path, callback]() {
        // Each worker thread needs its own named QSqlDatabase connection.
        const QString conn_name =
            "inst_async_" + broker_id + "_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVector<Instrument> all;

        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn_name);
            db.setDatabaseName(db_path);
            if (!db.open()) {
                LOG_ERROR("InstrumentService",
                          QString("load_from_db_async: failed to open DB for %1: %2")
                              .arg(broker_id, db.lastError().text()));
            } else {
                // Single query for all exchanges — faster than 9 per-exchange queries.
                QSqlQuery q(db);
                q.prepare("SELECT instrument_token, exchange_token, symbol, brsymbol, name, "
                          "exchange, brexchange, expiry, strike, lot_size, instrument_type, "
                          "tick_size, broker_id, broker_token "
                          "FROM instruments WHERE broker_id = ?");
                q.addBindValue(broker_id);
                if (q.exec()) {
                    while (q.next())
                        all.append(InstrumentRepository::map_row_static(q));
                } else {
                    LOG_ERROR("InstrumentService",
                              QString("load_from_db_async: query failed for %1: %2")
                                  .arg(broker_id, q.lastError().text()));
                }
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(conn_name);

        if (!self)
            return;

        const int count = all.size();
        QVector<Instrument> instruments = std::move(all);

        QMetaObject::invokeMethod(
            self,
            [self, broker_id, instruments = std::move(instruments), count, callback]() mutable {
                if (!self)
                    return;
                if (!instruments.isEmpty()) {
                    self->build_cache(broker_id, instruments);
                    LOG_INFO("InstrumentService",
                             QString("load_from_db_async: loaded %1 instruments for %2").arg(count).arg(broker_id));
                } else {
                    LOG_WARN("InstrumentService",
                             QString("load_from_db_async: no instruments found in DB for %1 — run refresh")
                                 .arg(broker_id));
                }
                if (callback)
                    callback(count);
            },
            Qt::QueuedConnection);
    });
}

void InstrumentService::load_from_db_worker(const QString& broker_id) {
    // Synchronous DB load safe to call from a QtConcurrent worker thread. Opens a
    // private named connection (never touches the shared main-thread connection)
    // then build_cache (mutex-guarded). No-op if already loaded.
    if (is_loaded(broker_id))
        return;
    const QString db_path = openmarketterminal::Database::instance().path();
    const QString conn_name =
        "inst_sync_" + broker_id + "_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVector<Instrument> all;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn_name);
        db.setDatabaseName(db_path);
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare("SELECT instrument_token, exchange_token, symbol, brsymbol, name, "
                      "exchange, brexchange, expiry, strike, lot_size, instrument_type, "
                      "tick_size, broker_id FROM instruments WHERE broker_id = ?");
            q.addBindValue(broker_id);
            if (q.exec()) {
                while (q.next())
                    all.append(InstrumentRepository::map_row_static(q));
            } else {
                LOG_ERROR("InstrumentService", QString("load_from_db_worker: query failed for %1: %2")
                                                   .arg(broker_id, q.lastError().text()));
            }
            db.close();
        } else {
            LOG_ERROR("InstrumentService", QString("load_from_db_worker: failed to open DB for %1: %2")
                                               .arg(broker_id, db.lastError().text()));
        }
    }
    QSqlDatabase::removeDatabase(conn_name);
    if (!all.isEmpty())
        build_cache(broker_id, all);
}

// ── Lookups ───────────────────────────────────────────────────────────────────

std::optional<qint64> InstrumentService::instrument_token(const QString& symbol, const QString& exchange,
                                                          const QString& broker_id) const {
    QMutexLocker lock(&mutex_);
    auto it = caches_.find(broker_id);
    if (it == caches_.end())
        return std::nullopt;
    // Cache keys are stored upper-case; normalise so callers passing e.g.
    // "reliance"/"nse" still match.
    auto jt = it->by_symbol.find({symbol.toUpper(), exchange.toUpper()});
    if (jt == it->by_symbol.end())
        return std::nullopt;
    return jt->instrument_token;
}

std::optional<QString> InstrumentService::to_brsymbol(const QString& symbol, const QString& exchange,
                                                      const QString& broker_id) const {
    QMutexLocker lock(&mutex_);
    auto it = caches_.find(broker_id);
    if (it == caches_.end())
        return std::nullopt;
    // Cache keys are stored upper-case; normalise so callers passing e.g.
    // "reliance"/"nse" still match.
    auto jt = it->by_symbol.find({symbol.toUpper(), exchange.toUpper()});
    if (jt == it->by_symbol.end())
        return std::nullopt;
    return jt->brsymbol;
}

std::optional<QString> InstrumentService::from_brsymbol(const QString& brsymbol, const QString& brexchange,
                                                        const QString& broker_id) const {
    QMutexLocker lock(&mutex_);
    auto it = caches_.find(broker_id);
    if (it == caches_.end())
        return std::nullopt;
    auto jt = it->by_brsymbol.find({brsymbol.toUpper(), brexchange.toUpper()});
    if (jt == it->by_brsymbol.end())
        return std::nullopt;
    return jt->symbol;
}

std::optional<Instrument> InstrumentService::find(const QString& symbol, const QString& exchange,
                                                  const QString& broker_id) const {
    QMutexLocker lock(&mutex_);
    auto it = caches_.find(broker_id);
    if (it == caches_.end())
        return std::nullopt;
    // Cache keys are stored upper-case; normalise so callers passing e.g.
    // "reliance"/"nse" still match.
    auto jt = it->by_symbol.find({symbol.toUpper(), exchange.toUpper()});
    if (jt == it->by_symbol.end())
        return std::nullopt;
    return *jt;
}

std::optional<Instrument> InstrumentService::find_by_token(quint32 instrument_token, const QString& broker_id) const {
    QMutexLocker lock(&mutex_);
    auto it = caches_.find(broker_id);
    if (it == caches_.end())
        return std::nullopt;
    auto jt = it->by_token.find(qint64(instrument_token));
    if (jt == it->by_token.end())
        return std::nullopt;
    return *jt;
}

QVector<Instrument> InstrumentService::search(const QString& query, const QString& exchange, const QString& broker_id,
                                              int limit) const {
    return search_all(query, exchange, QStringList{broker_id}, limit);
}

QVector<Instrument> InstrumentService::search_all(const QString& query, const QString& exchange,
                                                  const QStringList& broker_ids, int limit) const {
    const QString q = query.trimmed();
    if (q.isEmpty() || limit <= 0)
        return {};
    const QString exch = exchange.trimmed();

    // Prefer the in-memory cache — it is the hot path, fast (no per-keystroke
    // SQL), and stays correct even when the on-disk catalog is empty (e.g. a
    // failed/partial persist). Fall back to the SQLite repository only for
    // brokers whose cache isn't loaded in this session.
    auto type_rank = [](InstrumentType t) {
        switch (t) {
            case InstrumentType::EQ: return 0;
            case InstrumentType::INDEX: return 1;
            case InstrumentType::FUT: return 2;
            default: return 3;
        }
    };

    QVector<Instrument> out;
    QStringList db_fallback;
    {
        QMutexLocker lock(&mutex_);
        // Broker order: explicit list (first = highest priority) else all cached.
        QStringList order = broker_ids;
        order.removeAll(QString());
        if (order.isEmpty())
            for (auto it = caches_.cbegin(); it != caches_.cend(); ++it)
                order << it.key();

        for (const QString& bid : order) {
            auto cit = caches_.find(bid);
            if (cit == caches_.end() || !cit->loaded) {
                db_fallback << bid; // not in memory this session — try the DB below
                continue;
            }
            QVector<Instrument> hits;
            for (const auto& inst : cit->by_token) {
                if (!exch.isEmpty() && inst.exchange.compare(exch, Qt::CaseInsensitive) != 0)
                    continue;
                if (inst.symbol.contains(q, Qt::CaseInsensitive) ||
                    inst.brsymbol.contains(q, Qt::CaseInsensitive) ||
                    inst.name.contains(q, Qt::CaseInsensitive))
                    hits.append(inst);
            }
            // Rank: symbol-prefix matches first, then EQ→INDEX→FUT→other, then A-Z.
            std::sort(hits.begin(), hits.end(), [&](const Instrument& a, const Instrument& b) {
                const bool ap = a.symbol.startsWith(q, Qt::CaseInsensitive);
                const bool bp = b.symbol.startsWith(q, Qt::CaseInsensitive);
                if (ap != bp) return ap;
                const int ar = type_rank(a.instrument_type), br = type_rank(b.instrument_type);
                if (ar != br) return ar < br;
                return a.symbol < b.symbol;
            });
            out += hits;
            if (out.size() >= limit)
                break;
        }
    }

    if (out.size() < limit && !db_fallback.isEmpty())
        out += InstrumentRepository::instance().search_all(q, exchange, db_fallback, limit - out.size());

    if (out.size() > limit)
        out.resize(limit);
    return out;
}

// ── F&O / Options chain helpers ─────────────────────────────────────────────

QStringList InstrumentService::list_underlyings(const QString& broker_id, const QString& exchange) const {
    QMutexLocker lock(&mutex_);
    auto it = caches_.find(broker_id);
    if (it == caches_.end())
        return {};
    QSet<QString> seen;
    for (const auto& inst : it->by_token) {
        if (inst.exchange != exchange)
            continue;
        if (inst.instrument_type != InstrumentType::CE && inst.instrument_type != InstrumentType::PE
            && inst.instrument_type != InstrumentType::FUT)
            continue;
        if (!inst.name.isEmpty())
            seen.insert(inst.name);
    }
    QStringList out(seen.begin(), seen.end());
    std::sort(out.begin(), out.end());
    return out;
}

QStringList InstrumentService::list_expiries(const QString& broker_id, const QString& underlying,
                                             const QString& exchange) const {
    QMutexLocker lock(&mutex_);
    auto it = caches_.find(broker_id);
    if (it == caches_.end())
        return {};
    // Use a map keyed by parsed date so we can sort chronologically. Display
    // string ("DD-MMM-YY") sorts lexically wrong (e.g. "01-DEC-25" lex-before
    // "01-NOV-25"). The map's value collapses duplicates automatically.
    QMap<QDate, QString> by_date;
    int match_count = 0, empty_expiry = 0;
    for (const auto& inst : it->by_token) {
        if (inst.exchange != exchange || inst.name != underlying)
            continue;
        if (inst.instrument_type != InstrumentType::CE && inst.instrument_type != InstrumentType::PE
            && inst.instrument_type != InstrumentType::FUT)
            continue;
        ++match_count;
        if (inst.expiry.isEmpty()) {
            ++empty_expiry;
            continue;
        }
        // Parse "DD-MMM-YY" (case-insensitive month) or "YYYY-MM-DD".
        QDate d;
        if (inst.expiry.length() >= 9 && inst.expiry[2] == QLatin1Char('-') && inst.expiry[6] == QLatin1Char('-')) {
            const int day = QStringView(inst.expiry).left(2).toInt();
            const QStringView mon = QStringView(inst.expiry).mid(3, 3);
            const int yr = 2000 + QStringView(inst.expiry).mid(7, 2).toInt();
            int month = 0;
            if      (mon.compare(QLatin1String("JAN"), Qt::CaseInsensitive) == 0) month = 1;
            else if (mon.compare(QLatin1String("FEB"), Qt::CaseInsensitive) == 0) month = 2;
            else if (mon.compare(QLatin1String("MAR"), Qt::CaseInsensitive) == 0) month = 3;
            else if (mon.compare(QLatin1String("APR"), Qt::CaseInsensitive) == 0) month = 4;
            else if (mon.compare(QLatin1String("MAY"), Qt::CaseInsensitive) == 0) month = 5;
            else if (mon.compare(QLatin1String("JUN"), Qt::CaseInsensitive) == 0) month = 6;
            else if (mon.compare(QLatin1String("JUL"), Qt::CaseInsensitive) == 0) month = 7;
            else if (mon.compare(QLatin1String("AUG"), Qt::CaseInsensitive) == 0) month = 8;
            else if (mon.compare(QLatin1String("SEP"), Qt::CaseInsensitive) == 0) month = 9;
            else if (mon.compare(QLatin1String("OCT"), Qt::CaseInsensitive) == 0) month = 10;
            else if (mon.compare(QLatin1String("NOV"), Qt::CaseInsensitive) == 0) month = 11;
            else if (mon.compare(QLatin1String("DEC"), Qt::CaseInsensitive) == 0) month = 12;
            if (month > 0 && day > 0 && day <= 31)
                d = QDate(yr, month, day);
        }
        if (!d.isValid())
            d = QDate::fromString(inst.expiry, "yyyy-MM-dd");
        if (d.isValid())
            by_date.insert(d, inst.expiry);
    }
    LOG_INFO("InstrumentService",
             QString("list_expiries('%1','%2','%3'): matched=%4 empty_expiry=%5 parsed=%6")
                 .arg(broker_id, underlying, exchange).arg(match_count).arg(empty_expiry).arg(by_date.size()));
    const auto vals = by_date.values();
    return QStringList(vals.begin(), vals.end());
}

QVector<Instrument> InstrumentService::find_options_for_underlying(const QString& broker_id,
                                                                   const QString& underlying,
                                                                   const QString& expiry,
                                                                   const QString& exchange) const {
    QMutexLocker lock(&mutex_);
    auto it = caches_.find(broker_id);
    if (it == caches_.end())
        return {};
    QVector<Instrument> options;  // CE/PE
    QVector<Instrument> futures;  // FUT (appended last)
    for (const auto& inst : it->by_token) {
        if (inst.exchange != exchange || inst.name != underlying || inst.expiry != expiry)
            continue;
        if (inst.instrument_type == InstrumentType::CE || inst.instrument_type == InstrumentType::PE)
            options.append(inst);
        else if (inst.instrument_type == InstrumentType::FUT)
            futures.append(inst);
    }
    std::sort(options.begin(), options.end(), [](const Instrument& a, const Instrument& b) {
        if (a.strike != b.strike)
            return a.strike < b.strike;
        // Same strike: CE before PE for stable iteration
        return int(a.instrument_type) < int(b.instrument_type);
    });
    options.append(futures);
    return options;
}

int InstrumentService::cached_count(const QString& broker_id) const {
    QMutexLocker lock(&mutex_);
    auto it = caches_.find(broker_id);
    if (it == caches_.end())
        return 0;
    return it->by_symbol.size();
}

bool InstrumentService::is_loaded(const QString& broker_id) const {
    QMutexLocker lock(&mutex_);
    auto it = caches_.find(broker_id);
    return it != caches_.end() && it->loaded;
}

// ── Private ───────────────────────────────────────────────────────────────────

void InstrumentService::build_cache(const QString& broker_id, const QVector<Instrument>& instruments) {
    QMutexLocker lock(&mutex_);
    Cache& cache = caches_[broker_id];
    cache.by_symbol.clear();
    cache.by_token.clear();
    cache.by_brsymbol.clear();

    for (const auto& inst : instruments) {
        cache.by_symbol.insert({inst.symbol, inst.exchange}, inst);
        cache.by_token.insert(inst.instrument_token, inst);
        cache.by_brsymbol.insert({inst.brsymbol, inst.brexchange}, inst);
    }
    cache.loaded = true;
}

void InstrumentService::do_refresh(const QString& broker_id, const BrokerCredentials& creds) {
    LOG_INFO("InstrumentService", "Downloading instruments for " + broker_id);

    const InstrumentSource* src = SymbolResolver::instance().find(broker_id);
    if (!src || !src->download || !src->parse) {
        QMetaObject::invokeMethod(
            this,
            [this, broker_id]() {
                {
                    QMutexLocker lock(&mutex_);
                    refreshing_.remove(broker_id);
                }
                emit refresh_failed(broker_id, "Instrument source not registered for " + broker_id);
            },
            Qt::QueuedConnection);
        return;
    }

    const QByteArray payload = src->download(creds);
    if (payload.isEmpty()) {
        QMetaObject::invokeMethod(
            this,
            [this, broker_id]() {
                {
                    QMutexLocker lock(&mutex_);
                    refreshing_.remove(broker_id);
                }
                emit refresh_failed(broker_id, "Empty instrument data received");
            },
            Qt::QueuedConnection);
        return;
    }

    QVector<Instrument> instruments = src->parse(payload);

    if (instruments.isEmpty()) {
        QMetaObject::invokeMethod(
            this,
            [this, broker_id]() {
                {
                    QMutexLocker lock(&mutex_);
                    refreshing_.remove(broker_id);
                }
                emit refresh_failed(broker_id, "Failed to parse instrument data");
            },
            Qt::QueuedConnection);
        return;
    }

    // DB write + cache build MUST happen on the main thread —
    // QSqlDatabase connections are not thread-safe (Qt docs: use only from owning thread).
    // Worker thread hands off instruments to the main thread here.
    int count = instruments.size();
    QMetaObject::invokeMethod(
        this,
        [this, broker_id, instruments, count]() {
            // Persist to DB (main thread — safe)
            auto r = InstrumentRepository::instance().replace_all(broker_id, instruments);
            if (r.is_err()) {
                QString err = QString::fromStdString(r.error());
                {
                    QMutexLocker lock(&mutex_);
                    refreshing_.remove(broker_id);
                }
                emit refresh_failed(broker_id, err);
                return;
            }
            // Rebuild in-memory cache
            build_cache(broker_id, instruments);
            {
                QMutexLocker lock(&mutex_);
                refreshing_.remove(broker_id);
            }
            LOG_INFO("InstrumentService", QString("Refresh complete: %1 instruments for %2").arg(count).arg(broker_id));
            emit refresh_done(broker_id, count);
        },
        Qt::QueuedConnection);
}

} // namespace openmarketterminal::trading
