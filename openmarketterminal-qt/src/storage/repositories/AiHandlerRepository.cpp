#include "storage/repositories/AiHandlerRepository.h"

namespace openmarketterminal {

AiHandlerRepository& AiHandlerRepository::instance() {
    static AiHandlerRepository s;
    return s;
}

AiHandler AiHandlerRepository::map_handler(QSqlQuery& q) {
    AiHandler h;
    h.name = q.value(0).toString();
    h.strategy = q.value(1).toString();
    h.provider = q.value(2).toString();
    h.symbols = q.value(3).toString();
    h.market = q.value(4).toString();
    h.interval_sec = q.value(5).toInt();
    h.allowed_venues = q.value(6).toString();
    h.max_notional = q.value(7).toDouble();
    h.max_position = q.value(8).toDouble();
    h.enabled = q.value(9).toBool();
    h.notes = q.value(10).toString();
    h.created_at = q.value(11).toString();
    return h;
}

Result<void> AiHandlerRepository::save(const AiHandler& h) {
    return exec_write(
        "INSERT OR REPLACE INTO ai_handler "
        "(name, strategy, provider, symbols, market, interval_sec, allowed_venues, max_notional, "
        "max_position, enabled, notes, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, strftime('%s','now'))",
        {h.name, h.strategy, h.provider, h.symbols, h.market, h.interval_sec, h.allowed_venues,
         h.max_notional, h.max_position, h.enabled ? 1 : 0, h.notes});
}

Result<AiHandler> AiHandlerRepository::get(const QString& name) {
    return query_one("SELECT name, strategy, provider, symbols, market, interval_sec, allowed_venues, "
                     "max_notional, max_position, enabled, notes, created_at FROM ai_handler WHERE name = ?",
                     {name}, map_handler);
}

Result<QVector<AiHandler>> AiHandlerRepository::list() {
    return query_list("SELECT name, strategy, provider, symbols, market, interval_sec, allowed_venues, "
                      "max_notional, max_position, enabled, notes, created_at FROM ai_handler ORDER BY name",
                      {}, map_handler);
}

Result<void> AiHandlerRepository::remove(const QString& name) {
    return exec_write("DELETE FROM ai_handler WHERE name = ?", {name});
}

Result<void> AiHandlerRepository::set_enabled(const QString& name, bool enabled) {
    return exec_write("UPDATE ai_handler SET enabled = ? WHERE name = ?", {enabled ? 1 : 0, name});
}

} // namespace openmarketterminal
