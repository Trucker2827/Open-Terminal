#pragma once
#include "cli/CommandDispatch.h"
#include <QStringList>

namespace openmarketterminal::cli {

// `quant` command family: run any AI Quant Lab module (including Deep Agent)
// from the terminal — same scripts, same venv routing, same env as the GUI,
// no running app required. Compiled as its own TU (MSVC front-end capacity;
// see the note in CommandDispatch.cpp).
int quant_lab_command(const GlobalOpts& opts, QStringList args);

} // namespace openmarketterminal::cli
