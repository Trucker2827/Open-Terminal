#pragma once

#include "services/prediction/PredictionTypes.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace openmarketterminal::services::prediction::kalshi_ns {

/// Pure analysis and local evidence helpers for Kalshi. None of these methods
/// submit, amend, or cancel orders.
class KalshiEvidenceEngine {
  public:
    static QJsonObject ladder_snapshot(
        const QVector<PredictionMarket>& markets,
        const QHash<QString, PredictionOrderBook>& books,
        const QString& event_ticker,
        qint64 ts_ms);

    static QJsonArray analyze_ladder(
        const QVector<PredictionMarket>& markets,
        const QHash<QString, PredictionOrderBook>& books,
        const QString& event_ticker);

    static int reconcile_forward_labels(const QString& features_path,
                                        const QString& labels_path);

    static QJsonObject settlement_label(const PredictionMarket& market,
                                        const QString& features_path);

    /// What happens to the previous generation when an evidence file reaches
    /// the 64 MB rotation threshold.
    enum class Rotation {
        /// Two generations, the older discarded: the default for the snapshot
        /// and tick evidence whose value is the recent window.
        RecycleOldest,
        /// Every generation kept forever, under a name never used before. For
        /// files that are a RECORD rather than a window — the bot's decision
        /// ledger is the promotion gate's evidence and the order book's only
        /// memory, so deleting a generation of it deletes settled results and
        /// resting orders (issue #152).
        KeepAllGenerations,
    };

    static bool append_jsonl(const QString& path, const QJsonObject& row,
                             Rotation rotation = Rotation::RecycleOldest);

    /// Formats the spot calibrator's per-contract prediction (calibrator.json)
    /// for display. Returns {"state": "ok"|"stale"|"missing", "headline",
    /// "record", "trusted"}. A stale or missing report carries no numbers —
    /// a stale probability must never be presented as live.
    static QJsonObject calibrator_readout(const QJsonObject& report,
                                          const QString& market_ticker,
                                          qint64 now_ms,
                                          qint64 max_age_ms = 15LL * 60'000);

    static double conservative_taker_fee(double price, double contracts = 1.0);
    static double conservative_taker_fee(const PredictionMarket& market, double price,
                                         double contracts = 1.0);
};

} // namespace openmarketterminal::services::prediction::kalshi_ns
