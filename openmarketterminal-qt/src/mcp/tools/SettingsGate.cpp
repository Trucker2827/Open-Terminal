#include "mcp/tools/SettingsGate.h"

#include "mcp/McpProvider.h"
#include "storage/repositories/SettingsRepository.h"

#include <QSet>

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

bool cli_fast_live_armed() {
    return flag_true(QStringLiteral("cli.fast_live_armed"));
}

bool is_gui_only_setting(const QString& key) {
    // Case-insensitive on purpose: this is the single security chokepoint that
    // keeps a CLI/AI agent from writing any cli.* control knob. Matching must NOT
    // depend on downstream readers normalising the key — a future cli.risk.* cap
    // loader doing a NOCASE/LIKE lookup must not be able to reopen the hole via a
    // case-variant write. (Whitespace is inert: SQLite '=' never trims, so a
    // padded key can't collide with the canonical row under any collation.)
    return key.startsWith(QLatin1String("cli."), Qt::CaseInsensitive);
}

QStringList cli_allowed_venues() {
    auto r = SettingsRepository::instance().get(QStringLiteral("cli.allowed_venues"), QString());
    const QString raw = r.is_ok() ? r.value() : QString();
    QStringList out;
    for (const QString& part : raw.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString v = part.trimmed().toLower();
        if (!v.isEmpty())
            out.append(v);
    }
    return out;
}

bool cli_venue_allowed(const QString& venue) {
    return cli_allowed_venues().contains(venue.trimmed().toLower());
}

bool cli_kill_switch_engaged() {
    // The latched key is written by emergency-stop controls and can only be
    // cleared by an explicit human interaction in Security settings. Keeping
    // it separate prevents a stale, already-open settings form from clearing a
    // panic stop when it later saves unrelated fields.
    return flag_true(QStringLiteral("cli.kill_switch")) ||
           flag_true(QStringLiteral("cli.kill_switch_latched"));
}

QString cli_allowed_account() {
    auto r = SettingsRepository::instance().get(QStringLiteral("cli.allowed_account"), QString());
    return r.is_ok() ? r.value().trimmed() : QString();
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

bool is_fast_live_tool(const QString& name) {
    // The fast-live tool set (Phase D). Membership is decided on the canonical
    // NAME alone — NOT on a registered category — because the auth-checker must
    // gate these even before the tools are registered (the gate ships first, the
    // tools land in later tasks). Resolve the canonical name first so a legacy
    // alias can't dodge the fast-arm gate.
    static const QSet<QString> kFastLiveTools = {
        QStringLiteral("fast_submit_order"), QStringLiteral("cancel_order"),
        QStringLiteral("replace_order"),     QStringLiteral("exit_position"),
        QStringLiteral("get_positions"),     QStringLiteral("get_open_orders"),
        QStringLiteral("get_fills"),
        QStringLiteral("crypto_submit_order"), QStringLiteral("crypto_cancel_order"),
    };
    const QString canonical = McpProvider::instance().resolve_canonical_name(name);
    return kFastLiveTools.contains(canonical);
}

bool is_live_execution_tool(const QString& name) {
    // A direct live-broker EXECUTION tool (category "live-trading" + destructive:
    // live_place_order / live_smart_order / live_cancel_* / live_close_*). These
    // place/modify REAL orders and are NOT routed through the Phase-C constitution
    // (kill switch / arm / allowed-account / daily-loss), so the AI-facing hosts
    // must deny them outright — the AI's only live path is the gated submit_order
    // carve-out. (The non-destructive live-trading READ tools are not matched.)
    // Resolve the canonical name first so an alias can't dodge the classifier.
    const QString canonical = McpProvider::instance().resolve_canonical_name(name);
    auto t = McpProvider::instance().find_tool(canonical);
    return t && t->category == QLatin1String("live-trading") && t->is_destructive;
}

} // namespace openmarketterminal::mcp
