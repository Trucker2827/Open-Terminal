#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace openmarketterminal {
namespace ai_ledger {

/// Per-symbol realized track record (a slice of a Scorecard).
struct SymbolScore {
    QString symbol;
    int trades = 0;              // realized closes for this symbol
    int wins = 0;
    int losses = 0;
    double hit_rate = 0.0;       // wins / trades (0 when trades == 0)
    double realized_total = 0.0; // sum of realized_pnl over this symbol's closes
};

/// The AI handler's realized track record, over "trades" = realized closes
/// (fills with realized_pnl != 0). Read-only, derived from the ai_fill ledger.
struct Scorecard {
    QString handler;             // "" = all handlers
    QString symbol;              // "" = all symbols
    int trades = 0;              // wins + losses
    int wins = 0;
    int losses = 0;
    double hit_rate = 0.0;       // wins / trades (0 when trades == 0)
    double realized_total = 0.0; // sum of realized_pnl over counted closes
    double avg_realized = 0.0;   // realized_total / trades (0 when trades == 0)
    double best = 0.0;           // max realized_pnl among closes (0 when none)
    double worst = 0.0;          // min realized_pnl among closes (0 when none)
    QVector<SymbolScore> per_symbol;  // populated only when `symbol` filter is empty
};

/// Reads the handler+symbol's fills (recent-first), keeps only realized closes,
/// windows to the most recent `limit` closes (limit <= 0 = all), and computes the
/// aggregate + per-symbol breakdown. Read-only. Returns all-zero on a read error.
Scorecard scorecard_of(const QString& handler = {}, const QString& symbol = {}, int limit = 0);

/// Compact JSON of a Scorecard (+ nested per_symbol array).
QJsonObject scorecard_to_json(const Scorecard& s);

} // namespace ai_ledger
} // namespace openmarketterminal
