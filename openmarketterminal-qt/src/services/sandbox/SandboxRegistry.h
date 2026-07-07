#pragma once
// SandboxRegistry.h — immutable, param-hashed strategy books for the Strategy
// Sandbox (migration v056's sandbox_strategy table).
//
// A "strategy" here is a (kind, symbols, params) triple, content-addressed by
// a stable sha256-derived id: registering the exact same triple twice is a
// no-op that returns the existing id, and changing even one param field
// yields a brand-new id (and a brand-new row) rather than mutating history.
// This lets later position/score rows reference strategy_id and trust that
// the params behind that id never change out from under them.
//
// Nothing in this file ever UPDATEs or REPLACEs an existing row's
// params_json — register_strategy is INSERT OR IGNORE, full stop.

#include "core/result/Result.h"

#include <QJsonObject>
#include <QList>
#include <QString>

namespace openmarketterminal::services::sandbox {

/// One row of sandbox_strategy.
struct StrategyRow {
    QString strategy_id;
    QString kind;
    QString symbols;
    QString params_json;
    QString status;
    QString notes;
    qint64 created_at = 0;
};

// sha256("kind|symbols_csv|params_json") truncated to 16 hex chars, where
// params_json is QJsonDocument(params).toJson(QJsonDocument::Compact).
// This is canonical (stable for equal param sets) only because QJsonObject
// stores its keys in sorted order internally, so Qt always serializes a
// given set of key/value pairs the same way regardless of insertion order —
// do not build params_json by hand elsewhere and expect the same hash.
QString strategy_id_for(const QString& kind, const QString& symbols_csv, const QJsonObject& params);

// Inserts a new sandbox_strategy row keyed by strategy_id_for(kind, symbols_csv,
// params) if no row with that id exists yet. If a row already exists (i.e. this
// exact kind/symbols/params triple was registered before), it is left
// untouched — notes and created_at from the original registration are kept,
// not overwritten. Returns the strategy_id either way.
Result<QString> register_strategy(const QString& kind, const QString& symbols_csv, const QJsonObject& params,
                                   const QString& notes = {});

// Lists sandbox_strategy rows ordered by created_at. status_filter empty => all
// statuses; otherwise restricts to rows with that exact status.
Result<QList<StrategyRow>> list_strategies(const QString& status_filter = {});

// Updates status in place (this is metadata, not params — mutable by design).
// status must be one of "active" | "paused" | "retired"; anything else errs
// without touching the row.
Result<void> set_status(const QString& strategy_id, const QString& status);

// Registers the season-1 default strategy books (scalp, spot, btc5m, kalshi,
// long_short, chronos2) via register_strategy — idempotent, safe to call on
// every startup. Returns the strategy_ids in registration order.
Result<QList<QString>> seed_default_strategies();

} // namespace openmarketterminal::services::sandbox
