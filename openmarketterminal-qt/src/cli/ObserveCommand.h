#pragma once
#include "cli/CommandDispatch.h"   // GlobalOpts
#include <QStringList>

namespace openmarketterminal::cli {

// `observe latest | week | alerts [N]` — read-only views of the headless observer's
// journal via ObserverJournalService. Pure-local (no bridge/headless transport); the
// app only reads and renders what the LaunchAgent wrote. Returns a process exit code.
int observe_command(const GlobalOpts& opts, QStringList args);

} // namespace openmarketterminal::cli
