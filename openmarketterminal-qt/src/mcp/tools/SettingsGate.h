#pragma once
// SettingsGate.h — the two persistent CLI capability gates.
//
// Both settings default `false` and are the authoritative control surface the
// human owns in GUI Settings; the CLI/headless runtime *reads* them and obeys.
// Enforced server-side (headless auth-checker, attach bridge) so a raw client
// cannot bypass them. Lifting a gate only lifts the CLI's own gate — it never
// touches the app's deterministic risk limits / kill-switch.

#include <QString>

namespace openmarketterminal::mcp {

/// `cli.allow_trading` == "true" — gates destructive / trading-execution tools
/// (live orders, Python exec, workflow/agent mutation). Default false.
bool cli_trading_allowed();

/// `cli.allow_settings_write` == "true" — gates the `settings`-category WRITE
/// tools (e.g. set_setting). Settings READS are never gated. Default false.
bool cli_settings_write_allowed();

/// True iff `name` resolves to a registered `settings`-category WRITE tool
/// (category == "settings" && is_destructive). The headless auth-checker uses
/// this to route a tool to the cli_settings_write_allowed() gate instead of the
/// trading gate. Settings READ tools are non-destructive and never match.
bool is_settings_write_tool(const QString& name);

} // namespace openmarketterminal::mcp
