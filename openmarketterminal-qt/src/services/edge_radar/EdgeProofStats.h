#pragma once
// Cumulative paper-proof scoreboard for `edge crypto-recommend` journal rows:
// the ONE copy of the sample tiers, verdict thresholds and per-row stats fold
// shared by the CLI `edge journal proof-loop` command and the GUI SPOT/SCALP
// cockpit. Keeping both consumers on this module is the guarantee that the
// cockpit can never disagree with the CLI about what "proving" means.

#include <QString>

namespace openmarketterminal::services::edge_radar {

struct EdgeProofStats {
    int signal_count = 0;
    int resolved = 0;
    int waiting = 0;
    int wins = 0;
    int buy_signals = 0;
    int buy_resolved = 0;
    int buy_wins = 0;
    int no_trade_signals = 0;
    int no_trade_resolved = 0;
    int no_trade_correct = 0;
    int matched_orders = 0;
    double paper_pnl = 0.0;
    double avoided_value = 0.0;
};

double edge_proof_rate(int wins, int resolved);
QString edge_proof_sample_status(int resolved);
int edge_proof_next_milestone(int resolved);
QString edge_proof_verdict(const EdgeProofStats& s);
bool edge_proof_is_buy_call(const QString& call, const QString& side);

// The scored facts of one journal row. Unresolved rows (no future tick yet)
// carry resolved=false and contribute only to the waiting count.
struct EdgeProofRowOutcome {
    bool resolved = false;
    int outcome = -1;
    double move = 0.0;
    double breakeven = 0.0;
};

// Fold one row into the running stats exactly as proof-loop does. Returns the
// row's paper P&L contribution (non-zero only for a resolved buy call).
double edge_proof_accumulate(EdgeProofStats& s, bool buy_call,
                             const EdgeProofRowOutcome& row, double amount_usd);

// Extract the LAST persisted "scored: ... move=<x> ..." value from a journal
// row's reasons text (resolution appends one segment per scoring pass; the
// last one is the latest). Returns false when no such segment exists, so a
// row resolved outside the scorer reads as unpriced rather than as zero.
bool edge_proof_parse_scored_move(const QString& reasons, double* move_out);

} // namespace openmarketterminal::services::edge_radar
