#pragma once

#include <QStringList>

namespace openmarketterminal::cli {

struct GlobalOpts;
int provenance_command(const GlobalOpts& opts, QStringList args);

} // namespace openmarketterminal::cli
