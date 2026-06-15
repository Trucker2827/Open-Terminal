#pragma once
// SettingsGate.h — the two persistent CLI capability gates.
//
// Both settings default `false` and are the authoritative control surface the
// human owns in GUI Settings; the CLI/headless runtime *reads* them and obeys.
// Enforced server-side (headless auth-checker, attach bridge) so a raw client
// cannot bypass them. Lifting a gate only lifts the CLI's own gate — it never
// touches the app's deterministic risk limits / kill-switch.

#include <QString>
#include <QStringList>

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

/// `cli.allow_paper_trading` == "true" — gates submit_order mode=paper. Default false.
bool cli_paper_trading_allowed();

/// `cli.live_trading_armed` == "true" — the Phase-C live-arming flag. Default false.
bool cli_live_armed();

/// True iff `key` is a CLI/agent-control knob that MUST be GUI-only — the
/// settings-WRITE path refuses to change these even when cli.allow_settings_write
/// is on, so a CLI/AI agent can never arm/enable its own trading OR raise its own
/// risk caps. Implemented as the `cli.` prefix (covers the trading toggles AND the
/// cli.risk.* caps in one rule).
bool is_gui_only_setting(const QString& key);

/// The venues an AI/CLI agent is allowed to trade, read from `cli.allowed_venues`
/// (default ""). The setting is a comma-separated list; each non-empty part is
/// trimmed and lowercased. Empty/unset → empty list (default-deny: no venue is
/// allowed until a human names it in GUI Settings).
QStringList cli_allowed_venues();

/// True iff `venue` (case-insensitively, ignoring surrounding whitespace) is in
/// cli_allowed_venues(). Default-deny: false when the list is empty.
bool cli_venue_allowed(const QString& venue);

/// `cli.kill_switch` == "true" — the PANIC button (Phase C). When engaged it
/// halts ALL AI trading (paper AND live): every order-flow handler checks this
/// FIRST and short-circuits to a recorded refusal before any drafting or
/// execution. GUI-only (a human owns it; the CLI/agent can never write it).
/// Default false.
bool cli_kill_switch_engaged();

/// The single account the AI may LIVE-trade, read from `cli.allowed_account`
/// (default ""), trimmed. Empty/unset → "" (default-deny: no account is allowed
/// until a human names one in GUI Settings). Consumed by later live-trade tasks.
QString cli_allowed_account();

} // namespace openmarketterminal::mcp
