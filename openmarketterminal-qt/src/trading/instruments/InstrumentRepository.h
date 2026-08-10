#pragma once
#include "storage/repositories/BaseRepository.h"
#include "storage/sqlite/SqlResult.h"
#include "trading/instruments/InstrumentTypes.h"

#include <QDateTime>

#include <optional>

namespace openmarketterminal::trading {

class InstrumentRepository : public openmarketterminal::BaseRepository<Instrument> {
  public:
    static InstrumentRepository& instance();

    // ── Write ────────────────────────────────────────────────────────────────

    /// Bulk insert instruments for one broker, replacing all existing rows for that broker.
    /// Runs inside a single transaction for performance.
    openmarketterminal::Result<void> replace_all(const QString& broker_id, const QVector<Instrument>& instruments);

    /// Clear all instruments for a broker.
    openmarketterminal::Result<void> clear(const QString& broker_id);

    // ── Read ─────────────────────────────────────────────────────────────────

    /// Exact lookup: (symbol, exchange, broker_id) → Instrument
    std::optional<Instrument> find(const QString& symbol, const QString& exchange, const QString& broker_id) const;

    /// Lookup by instrument_token + broker_id
    std::optional<Instrument> find_by_token(qint64 instrument_token, const QString& broker_id) const;

    /// Lookup by brsymbol + brexchange + broker_id  (used when mapping order responses)
    std::optional<Instrument> find_by_brsymbol(const QString& brsymbol, const QString& brexchange,
                                               const QString& broker_id) const;

    /// Full-text search for symbol picker UI.
    /// Returns up to `limit` rows matching query against symbol, brsymbol, or name.
    QVector<Instrument> search(const QString& query, const QString& exchange, const QString& broker_id,
                               int limit = 50) const;

    /// Cross-broker search for the unified symbol picker. Searches the union of
    /// `broker_ids` (empty = all brokers), one row per (broker, instrument),
    /// with the first broker in the list sorted first. Same match semantics as
    /// `search` (symbol/brsymbol/name LIKE).
    QVector<Instrument> search_all(const QString& query, const QString& exchange,
                                   const QStringList& broker_ids, int limit = 50) const;

    /// All instruments for a broker+exchange (used for option chains etc.)
    QVector<Instrument> list(const QString& exchange, const QString& broker_id,
                             InstrumentType type = InstrumentType::UNKNOWN) const;

    /// Count rows for a broker (quick freshness check).
    int count(const QString& broker_id) const;

    /// Most recent updated_at across this broker's instruments, or an invalid
    /// QDateTime if no rows exist. Used for age-based refresh freshness checks.
    QDateTime last_updated(const QString& broker_id) const;

    /// Map a positioned row to an Instrument. Public for use by async loaders.
    ///
    /// Templated on the row type because there are two legitimate sources:
    /// Database::execute() hands back a buffered SqlResult, while the async
    /// loaders open their OWN QSqlDatabase on a worker thread (QSqlDatabase is
    /// not thread-safe) and iterate a raw QSqlQuery. Those loaders are not the
    /// read-cursor hazard SqlResult exists to remove -- their cursor lives on a
    /// private connection that no writer shares.
    template <typename Row>
    static Instrument map_row_static(Row& q) { return map_row_impl(q); }

  private:
    InstrumentRepository() = default;
    static Instrument map_row(storage::sqlite::SqlResult& q) { return map_row_impl(q); }

    template <typename Row>
    static Instrument map_row_impl(Row& q) {
        Instrument i;
        i.instrument_token = q.value(0).toLongLong();
        i.exchange_token = q.value(1).toLongLong();
        i.symbol = q.value(2).toString();
        i.brsymbol = q.value(3).toString();
        i.name = q.value(4).toString();
        i.exchange = q.value(5).toString();
        i.brexchange = q.value(6).toString();
        i.expiry = q.value(7).toString();
        i.strike = q.value(8).toDouble();
        i.lot_size = q.value(9).toInt();
        i.instrument_type = parse_instrument_type(q.value(10).toString());
        i.tick_size = q.value(11).toDouble();
        i.broker_id = q.value(12).toString();
        i.broker_token = q.value(13).toString();
        return i;
    }
};

} // namespace openmarketterminal::trading
