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

// `ai strategy list [--json]` — enumerate the registered AI strategy plugins
// (StrategyRegistry::list()). Pure: builds a throwaway registry, no DB touch.
int ai_strategy_list_command(const GlobalOpts& opts);

// `ai handler <create|list|show|delete|enable|disable|status|run> ...` — CRUD +
// arm-state readout + paper runner over saved AI trade-handler configs
// (AiHandlerRepository). `action` is the token right after `ai handler`; `rest`
// is everything after that.
//
// PAPER-ONLY / DISARMED. `create` always writes enabled=false. `enable`/`disable`
// flip ONLY the saved row's `enabled` column (AiHandlerRepository::set_enabled) —
// they do NOT arm any live trading path. `status` is strictly READ-ONLY: it READS
// the cli.* SettingsGate flags to report arm-state and NEVER writes any of them
// (`armed` is always false — no live path exists). `run` drives the handler's
// strategy through the StrategyRunner PAPER path only and REFUSES any non-paper
// mode (exit 2). There is no live execution path anywhere in this plugin core.
int ai_handler_command(const GlobalOpts& opts, const QString& action, QStringList rest);

}  // namespace openmarketterminal::cli
