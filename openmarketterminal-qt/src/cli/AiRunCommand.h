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

}  // namespace openmarketterminal::cli
