// src/services/ai_quant_lab/ScreenWatchlistFeed.h
//
// Pure mapping from a model-screen payload (+ its IC measurement) to the
// watchlist strings the feed writes: watchlist name, description, per-stock
// notes. Issue #102 — the watchlist must carry the model's measured IC (or an
// explicit "unmeasured" disclaimer); dropping that evidence is a bug, so the
// mapping lives here where tst_screen_watchlist_feed can pin it.
#pragma once

#include <QJsonObject>
#include <QString>

namespace openmarketterminal::services::quant {

/// Exact disclaimer used whenever the model's IC could not be measured.
QString screen_ic_unmeasured_disclaimer();

/// Watchlist name derived from the model id ("Model screen: <model_id>").
QString screen_watchlist_name(const QString& model_id);

/// One-line IC evidence from a get_factor_analysis('ic') payload:
/// "IC_mean 0.0123, Rank_IC_mean 0.0110 over 2025-01-01..2025-06-30 (42 days)".
/// A missing/failed/empty payload yields the unmeasured disclaimer — never "".
QString screen_ic_evidence(const QJsonObject& ic_payload);

/// Watchlist description recording model id, as-of date, universe size and the
/// IC evidence (or the unmeasured disclaimer).
QString screen_watchlist_description(const QJsonObject& screen_payload, const QJsonObject& ic_payload);

/// Per-stock note recording the model's score for one screen row
/// ({"symbol": ..., "score": ...}) plus model id and as-of date.
QString screen_stock_note(const QJsonObject& row, const QJsonObject& screen_payload);

} // namespace openmarketterminal::services::quant
