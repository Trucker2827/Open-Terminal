#pragma once

namespace openmarketterminal::mcp {

/// Headless checks for DataHub peek helpers and hub-first quote path.
/// Invoked via `--selftest-datahub-peek`.
int run_datahub_peek_selftest();

} // namespace openmarketterminal::mcp
