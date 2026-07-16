#pragma once
// AiRunCommand.h — `ai run strategy <name> --mode paper …` (Paper Strategy-Loop
// Driver, Task 4).
//
// Drives the StrategyRunner over the existing CLI transport (headless in-process
// runtime OR an attached serve daemon). PAPER ONLY: the command refuses
// --mode live. It builds a real ToolCaller, instantiates the named strategy
// (`meanrev` → MeanReversionStrategy, `claude` → LlmStrategy wired to the real
// LlmService), runs the loop, prints decisions + a summary, and stops cleanly on
// SIGINT / max-iters / duration / kill switch.
#include "cli/CommandDispatch.h"  // GlobalOpts

#include <QStringList>

namespace openmarketterminal::cli {

// Run `ai run strategy`. `rest` is the args AFTER `ai run strategy`, i.e. it
// begins with the positional <name>. Returns a process exit code.
int ai_run_strategy(const GlobalOpts& opts, const QStringList& rest);

// Run `ai ctx <symbol> [--json] [--market prediction|equity]` (ai ctx
// decision-packet Task 3). `rest` is the args AFTER `ai ctx`, i.e. it begins
// with the positional <symbol>. READ-ONLY: calls DecisionContext::assess +
// to_json and emits the packet — never places an order, writes a gate
// setting, or otherwise mutates the DB. Missing <symbol> -> usage error on
// stderr, exit 2. A symbol with no edge_decision_journal row still exits 0
// with a packet carrying has_edge_signal=false. Returns a process exit code.
int ai_ctx_command(const GlobalOpts& opts, const QStringList& rest);

// Run `ai screen [--market prediction|equity|crypto] [--limit N] [--json]`
// (ai screen shortlist Task 2). `rest` is the args AFTER `ai screen`.
// READ-ONLY: calls ai_decision::screen + screen_to_json and emits the
// resulting JSON array (or a readable table) — never places an order, writes
// a gate setting, or otherwise mutates the DB. An unrecognized --market or a
// non-positive/garbage --limit -> usage error on stderr, exit 2. No matching
// candidates still exits 0 with an empty array/table. Returns a process exit
// code.
int ai_screen_command(const GlobalOpts& opts, const QStringList& rest);

// Run `ai positions [--handler H] [--json]` (AI paper ledger Task 5).
// READ-ONLY: folds ai_fill rows via ai_ledger::positions_of and emits a JSON
// array of {handler, symbol, net_qty, avg_entry_price, realized_pnl} — never
// writes an ai_fill row, a cli.* gate setting, or any other DB row. Empty
// --handler = all handlers. Returns a process exit code.
int ai_positions_command(const GlobalOpts& opts, const QStringList& rest);

// Run `ai pnl [--handler H] [--json]` (AI paper ledger Task 5). READ-ONLY:
// emits a JSON object {realized_pnl, open_positions: [...]}  sourced from
// ai_ledger::realized_total + positions_of — never writes any DB row. Empty
// --handler = all handlers. Returns a process exit code.
int ai_pnl_command(const GlobalOpts& opts, const QStringList& rest);

// Run `ai ledger [--handler H] [--symbol S] [--limit N] [--json]` (AI paper
// ledger Task 5). READ-ONLY: lists raw fills (recent first) via
// AiFillRepository::list — never writes any DB row. Empty --handler/--symbol
// = no filter on that column; default --limit is 50. Returns a process exit
// code.
int ai_ledger_command(const GlobalOpts& opts, const QStringList& rest);

// Run `ai record-fill --handler H --symbol S --side buy|sell --qty Q --price P
// [--fee F] [--draft-id D] [--json]` (AI paper ledger Task 6). WRITE, but
// paper-only: the ONLY mutation is a single ai_fill row appended via
// ai_ledger::record_fill — never a cli.* gate setting, never a live order.
// Invalid/missing args (empty handler/symbol, side not buy|sell, non-positive
// qty/price, negative fee) -> usage error on stderr, exit 2, no row written.
// On success emits {fill: {...}, position: {...}} sourced from the fresh
// AiFill and ai_ledger::position_of. Returns a process exit code.
int ai_record_fill_command(const GlobalOpts& opts, const QStringList& rest);

}  // namespace openmarketterminal::cli
