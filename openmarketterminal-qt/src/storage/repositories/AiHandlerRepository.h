#pragma once
#include "storage/repositories/BaseRepository.h"

namespace openmarketterminal {

/// A named, saved AI trade-handler recipe: {name, strategy, provider, symbols,
/// interval_sec, allowed_venues, max_notional, max_position, enabled, notes,
/// created_at}. `enabled` defaults to false — the paper-only/disarmed
/// invariant is enforced at the data layer (migration v063's column default).
struct AiHandler {
    QString name;
    QString strategy;
    QString provider;
    QString symbols;
    int interval_sec = 60;
    QString allowed_venues;
    double max_notional = 0.0;
    double max_position = 0.0;
    bool enabled = false;
    QString notes;
    QString created_at;
};

class AiHandlerRepository : public BaseRepository<AiHandler> {
  public:
    static AiHandlerRepository& instance();

    Result<void> save(const AiHandler& h);
    Result<AiHandler> get(const QString& name);
    Result<QVector<AiHandler>> list();
    Result<void> remove(const QString& name);
    Result<void> set_enabled(const QString& name, bool enabled);

  private:
    AiHandlerRepository() = default;
    static AiHandler map_handler(QSqlQuery& q);
};

} // namespace openmarketterminal
