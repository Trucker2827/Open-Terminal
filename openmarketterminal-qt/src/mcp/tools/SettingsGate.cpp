#include "mcp/tools/SettingsGate.h"

#include "mcp/McpProvider.h"
#include "storage/repositories/SettingsRepository.h"

namespace openmarketterminal::mcp {

namespace {
// Default-deny: a missing/unset/empty key reads back as the "false" default
// (SettingsRepository::get returns ok(default) on miss and on empty value), so
// only a literal "true" lifts a gate. Read live each call — the per-process
// headless host re-reads at dispatch time, and the per-thread cloned DB
// connection (Database::connection()) makes this safe off the main thread.
bool flag_true(const QString& key) {
    auto r = SettingsRepository::instance().get(key, QStringLiteral("false"));
    return r.is_ok() && r.value() == QLatin1String("true");
}
} // namespace

bool cli_trading_allowed() {
    return flag_true(QStringLiteral("cli.allow_trading"));
}

bool cli_settings_write_allowed() {
    return flag_true(QStringLiteral("cli.allow_settings_write"));
}

bool cli_paper_trading_allowed() {
    return flag_true(QStringLiteral("cli.allow_paper_trading"));
}

bool cli_live_armed() {
    return flag_true(QStringLiteral("cli.live_trading_armed"));
}

bool is_gui_only_setting(const QString& key) {
    return key.startsWith(QLatin1String("cli."));
}

bool is_settings_write_tool(const QString& name) {
    // The headless auth-checker classifies on the caller-supplied name, which may
    // be a legacy alias, while `is_destructive` comes from the resolved def.
    // Resolve to the canonical name first so an aliased settings-write tool can't
    // dodge the settings-write gate and fall through to the trading gate.
    const QString canonical = McpProvider::instance().resolve_canonical_name(name);
    auto t = McpProvider::instance().find_tool(canonical);
    return t && t->category == QLatin1String("settings") && t->is_destructive;
}

} // namespace openmarketterminal::mcp
