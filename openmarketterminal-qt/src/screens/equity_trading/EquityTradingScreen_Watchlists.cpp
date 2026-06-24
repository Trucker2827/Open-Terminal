// src/screens/equity_trading/EquityTradingScreen_Watchlists.cpp
//
// Named-watchlist controller for the Equity Trading screen. Backed by the
// shared (global) WatchlistRepository — the same lists the dedicated Watchlist
// screen manages, so they stay unified and benefit from cloud sync. The active
// list's symbols drive the live stream subscriptions + hub quote topics.
//
// Part of the partial-class split of EquityTradingScreen.cpp.

#include "screens/equity_trading/EquityTradingScreen.h"

#include "core/logging/Logger.h"
#include "screens/equity_trading/EquityTypes.h"
#include "screens/equity_trading/EquityWatchlist.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/repositories/WatchlistRepository.h"
#include "trading/BrokerRegistry.h"
#include "trading/DataStreamManager.h"

#include <QPair>
#include <QSet>
#include <QVector>

namespace openmarketterminal::screens {

namespace {
const QString WL_TAG = "EquityTrading";

// One-time seeding of the curated preset watchlists (Markets / Leaders /
// Crypto). Gated by a settings sentinel so it runs exactly once: fresh installs
// get all three; existing users get any they're missing by name, leaving their
// own lists untouched. SettingsRepository::get() always returns ok(), so
// value() != "1" reliably means "not yet seeded".
void ensure_curated_watchlists() {
    auto& settings = openmarketterminal::SettingsRepository::instance();
    if (settings.get("watchlists.curated_v1").value() == QLatin1String("1"))
        return;

    auto& repo = openmarketterminal::WatchlistRepository::instance();
    QSet<QString> existing_names;
    if (auto r = repo.list_all(); r.is_ok())
        for (const auto& w : r.value())
            existing_names.insert(w.name);

    const QVector<QPair<QString, QStringList>> presets = {
        {QStringLiteral("Markets"), equity::MARKETS_WATCHLIST},
        {QStringLiteral("Leaders"), equity::DEFAULT_WATCHLIST},
        {QStringLiteral("Crypto"), equity::CRYPTO_WATCHLIST},
    };
    for (const auto& preset : presets) {
        if (existing_names.contains(preset.first))
            continue;
        auto cr = repo.create(preset.first);
        if (cr.is_ok())
            for (const auto& s : preset.second)
                repo.add_stock(cr.value().id, s, {}, {});
    }
    settings.set("watchlists.curated_v1", QStringLiteral("1"), QStringLiteral("watchlist"));
    LOG_INFO(WL_TAG, "Seeded curated watchlists (Markets / Leaders / Crypto)");
}
}  // namespace

void EquityTradingScreen::load_watchlists() {
    ensure_curated_watchlists();  // one-time seed of Markets / Leaders / Crypto presets
    auto& repo = openmarketterminal::WatchlistRepository::instance();
    auto r = repo.list_all();
    QVector<openmarketterminal::Watchlist> lists = r.is_ok() ? r.value() : QVector<openmarketterminal::Watchlist>{};

    // First run anywhere (no lists in the repo) — seed one from the focused
    // broker's default_watchlist so existing behaviour is preserved.
    if (lists.isEmpty()) {
        // Seed a default watchlist. Prefer the focused broker's own default
        // list; otherwise fall back to US/global large caps.
        QString seed_name = QStringLiteral("US Large Caps");
        QStringList seed_syms = equity::DEFAULT_WATCHLIST;
        if (auto* b = trading::BrokerRegistry::instance().get(broker_id_for_focused())) {
            const auto prof = b->profile();
            if (!prof.default_watchlist.empty()) {
                seed_name = QStringLiteral("Watchlist");
                seed_syms = QStringList(prof.default_watchlist.begin(), prof.default_watchlist.end());
            }
        }
        auto cr = repo.create(seed_name);
        if (cr.is_ok()) {
            const QString new_id = cr.value().id;
            for (const auto& s : seed_syms)
                repo.add_stock(new_id, s, {}, selected_exchange_);
            auto r2 = repo.list_all();
            if (r2.is_ok())
                lists = r2.value();
        }
    }

    if (lists.isEmpty()) {
        LOG_WARN(WL_TAG, "No watchlists available (seed failed)");
        return;
    }

    // Pick the active list: keep the current one if still present, else the
    // default-flagged one, else the first.
    bool active_present = false;
    for (const auto& w : lists) {
        if (w.id == active_watchlist_id_) {
            active_present = true;
            break;
        }
    }
    if (!active_present) {
        active_watchlist_id_ = lists.first().id;
        for (const auto& w : lists) {
            if (w.is_default) {
                active_watchlist_id_ = w.id;
                break;
            }
        }
    }

    QVector<QPair<QString, QString>> combo;
    combo.reserve(lists.size());
    for (const auto& w : lists)
        combo.append({w.id, w.name});
    if (watchlist_)
        watchlist_->set_watchlists(combo, active_watchlist_id_);

    // on_account_changed wires the stream itself; standalone callers pass true.
    apply_active_watchlist(false);
}

void EquityTradingScreen::apply_active_watchlist(bool resubscribe) {
    if (active_watchlist_id_.isEmpty())
        return;

    auto r = openmarketterminal::WatchlistRepository::instance().get_stocks(active_watchlist_id_);
    QStringList syms;
    if (r.is_ok()) {
        const auto& stocks = r.value();
        syms.reserve(stocks.size());
        for (const auto& s : stocks)
            syms << s.symbol;
    }
    watchlist_symbols_ = syms;
    const QStringList eff = effective_symbols(); // active list ∪ open positions
    if (watchlist_)
        watchlist_->set_symbols(eff);

    if (!resubscribe)
        return;

    auto* stream = trading::DataStreamManager::instance().stream_for(focused_account_id_);
    if (stream) {
        stream->subscribe_symbols(QStringLiteral("equity:watchlist"), eff);
        stream->fetch_orderbook(selected_symbol_);
    }

    if (isVisible() && hub_active_)
        hub_subscribe_streaming(); // re-point quote topics at the new symbol set
}

void EquityTradingScreen::on_watchlist_selected(const QString& id) {
    if (id.isEmpty() || id == active_watchlist_id_)
        return;
    active_watchlist_id_ = id;
    apply_active_watchlist(true);
}

void EquityTradingScreen::on_watchlist_create(const QString& name) {
    auto cr = openmarketterminal::WatchlistRepository::instance().create(name);
    if (!cr.is_ok())
        return;
    active_watchlist_id_ = cr.value().id;
    load_watchlists();              // refresh combo, selects the new (empty) list
    apply_active_watchlist(true);
}

void EquityTradingScreen::on_watchlist_rename(const QString& id, const QString& name) {
    auto& repo = openmarketterminal::WatchlistRepository::instance();
    auto g = repo.get(id);
    if (g.is_ok()) {
        auto w = g.value();
        w.name = name;
        repo.update(w);
    }
    load_watchlists();
}

void EquityTradingScreen::on_watchlist_delete(const QString& id) {
    auto& repo = openmarketterminal::WatchlistRepository::instance();
    auto r = repo.list_all();
    if (r.is_ok() && r.value().size() <= 1) {
        LOG_INFO(WL_TAG, "Refusing to delete the last watchlist");
        return; // always keep at least one list
    }
    repo.remove(id);
    if (active_watchlist_id_ == id)
        active_watchlist_id_.clear(); // load_watchlists() picks another
    load_watchlists();
    apply_active_watchlist(true);
}

void EquityTradingScreen::on_watchlist_symbol_added(const QString& symbol) {
    const QString sym = symbol.trimmed().toUpper();
    if (active_watchlist_id_.isEmpty() || sym.isEmpty())
        return;
    openmarketterminal::WatchlistRepository::instance().add_stock(active_watchlist_id_, sym, {}, selected_exchange_);
    apply_active_watchlist(true);
}

void EquityTradingScreen::on_watchlist_symbol_removed(const QString& symbol) {
    if (active_watchlist_id_.isEmpty() || symbol.isEmpty())
        return;
    openmarketterminal::WatchlistRepository::instance().remove_stock(active_watchlist_id_, symbol);
    apply_active_watchlist(true);
}

QStringList EquityTradingScreen::effective_symbols() const {
    QStringList eff = watchlist_symbols_;
    for (const auto& s : position_symbols_)
        if (!s.isEmpty() && !eff.contains(s))
            eff << s;
    return eff;
}

void EquityTradingScreen::update_position_symbols(const QStringList& syms) {
    QStringList deduped;
    for (const auto& s : syms)
        if (!s.isEmpty() && !deduped.contains(s))
            deduped << s;
    if (deduped == position_symbols_)
        return; // unchanged — avoid resubscribe churn
    position_symbols_ = deduped;
    LOG_INFO("posdbg", QString("update_position_symbols: [%1]").arg(deduped.join(',')));
    // Re-display + re-subscribe to the new active∪positions union so held symbols
    // get live WebSocket prices. Deferred to the next tick because this can fire
    // from inside a hub callback, and apply_active_watchlist re-subscribes the hub.
    QMetaObject::invokeMethod(this, [this]() { apply_active_watchlist(true); }, Qt::QueuedConnection);
}

} // namespace openmarketterminal::screens
