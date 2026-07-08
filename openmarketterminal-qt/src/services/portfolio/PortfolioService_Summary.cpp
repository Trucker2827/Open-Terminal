// src/services/portfolio/PortfolioService_Summary.cpp
//
// Summary loading: load_summary, refresh_summary, build_summary,
// try_broker_quotes, finalize_summary. These produce the PortfolioSummary
// object the UI binds to.
//
// Part of the partial-class split of PortfolioService.cpp.

#include "services/portfolio/PortfolioService.h"

#include "core/logging/Logger.h"
#include "services/portfolio/AccountSyncTypes.h"
#include "python/PythonRunner.h"
#include "services/portfolio/PortfolioSummaryBuild.h"
#include "services/sectors/SectorResolver.h"
#include "storage/repositories/PortfolioRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "trading/AccountManager.h"
#include "trading/BrokerInterface.h"
#include "trading/BrokerRegistry.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace openmarketterminal::services {

void PortfolioService::load_summary(const QString& portfolio_id) {
    // Check cache first (P11)
    {
        QMutexLocker lock(&cache_mutex_);
        auto it = summary_cache_.find(portfolio_id);
        if (it != summary_cache_.end()) {
            qint64 now = QDateTime::currentSecsSinceEpoch();
            if (now - it->timestamp < kCacheTtlSec) {
                emit summary_loaded(it->summary);
                return;
            }
        }
    }

    // ── Virtual "All Accounts" aggregate — no DB row; synthesize a Portfolio
    // whose assets are the union of every synced portfolio's holdings, then
    // run the SAME quote+FX+build path as a real portfolio (no pricing
    // special-casing).
    if (portfolio_id == QLatin1String(kAllAccountsId)) {
        const auto synced = PortfolioRepository::instance().list_synced();

        portfolio::Portfolio synthetic;
        synthetic.id = QString::fromLatin1(kAllAccountsId);
        synthetic.name = QStringLiteral("All Accounts");
        synthetic.currency = synced.isEmpty() ? QStringLiteral("USD") : synced.first().currency;

        const auto assets = synced.isEmpty() ? QVector<portfolio::PortfolioAsset>{} : aggregate_all_accounts_assets();
        if (assets.isEmpty()) {
            portfolio::PortfolioSummary empty;
            empty.portfolio = synthetic;
            empty.last_updated = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            emit summary_loaded(empty);
            return;
        }

        build_summary(portfolio_id, assets, synthetic);
        return;
    }

    auto portfolio_r = PortfolioRepository::instance().get_portfolio(portfolio_id);
    if (portfolio_r.is_err()) {
        emit summary_error(portfolio_id, QString::fromStdString(portfolio_r.error()));
        return;
    }

    auto assets_r = PortfolioRepository::instance().get_assets(portfolio_id);
    if (assets_r.is_err()) {
        emit summary_error(portfolio_id, QString::fromStdString(assets_r.error()));
        return;
    }

    if (assets_r.value().isEmpty()) {
        // Empty portfolio — emit summary with zero values
        portfolio::PortfolioSummary empty;
        empty.portfolio = portfolio_r.value();
        empty.last_updated = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        emit summary_loaded(empty);
        return;
    }

    build_summary(portfolio_id, assets_r.value(), portfolio_r.value());
}

QVector<portfolio::PortfolioAsset> PortfolioService::aggregate_all_accounts_assets() {
    const auto synced = PortfolioRepository::instance().list_synced();

    QVector<QVector<portfolio::PortfolioAsset>> per_portfolio;
    per_portfolio.reserve(synced.size());
    for (const auto& pf : synced) {
        auto assets_r = PortfolioRepository::instance().get_assets(pf.id);
        if (assets_r.is_ok())
            per_portfolio.append(assets_r.value());
    }

    return portfolio::aggregate_holdings(per_portfolio);
}

void PortfolioService::refresh_summary(const QString& portfolio_id) {
    invalidate_cache(portfolio_id);
    load_summary(portfolio_id);
}

void PortfolioService::build_summary(const QString& portfolio_id, const QVector<portfolio::PortfolioAsset>& assets,
                                     const portfolio::Portfolio& portfolio) {
    // Routing decision: broker-first when the portfolio is linked to a
    // connected broker account (broker_account_id non-empty + creds present
    // + broker exposes get_quotes). yfinance is the universal fallback.
    if (try_broker_quotes(portfolio_id, assets, portfolio))
        return;

    // ── yfinance path (legacy / unlinked portfolios) ─────────────────────────
    auto& mds = MarketDataService::instance();
    const QString base_ccy = portfolio.currency.isEmpty() ? QStringLiteral("USD") : portfolio.currency;

    // `$CASH:<CCY>` pseudo-holdings are not real tickers — never send them to
    // resolve_names/fetch_quotes (yfinance has no such symbol). Only real
    // holdings go into the quote-fetch symbol list.
    QStringList symbols;
    symbols.reserve(assets.size());
    for (const auto& a : assets) {
        if (!a.symbol.startsWith(QLatin1String("$CASH:")))
            symbols.append(a.symbol);
    }

    // Ensure each holding's listing currency is resolved (populates the currency
    // cache read below). Fire-and-forget — a first-ever foreign symbol converts
    // at 1.0 with its snapshot blocked until this lands, then self-corrects.
    mds.resolve_names(symbols, [](const QHash<QString, QString>&) {});

    // Fetch holdings + any FX pairs needed to convert foreign holdings into the
    // base currency, in a single batch. native_ccy is captured so the callback
    // can map each symbol to its resolved rate.
    QHash<QString, QString> native_ccy;
    QStringList fetch = symbols;
    for (const auto& a : assets) {
        // A `$CASH:<CCY>` pseudo-holding's "native currency" is the suffix
        // itself, not a currency_code() lookup (which only knows real
        // tickers and would return empty -> unresolved FX -> blocks the
        // snapshot forever for cash rows). The symbol itself is never sent
        // to the quote fetch above; only its FX pair (if foreign) is.
        const bool is_cash = a.symbol.startsWith(QLatin1String("$CASH:"));
        const QString ccy = is_cash ? a.symbol.mid(6) : mds.currency_code(a.symbol);
        native_ccy.insert(a.symbol, ccy);
        if (!ccy.isEmpty() && ccy != base_ccy) {
            const QString pair = ccy + base_ccy + QStringLiteral("=X");
            if (!fetch.contains(pair))
                fetch.append(pair);
        }
    }

    QPointer<PortfolioService> self = this;
    mds.fetch_quotes(fetch, [self, portfolio_id, assets, portfolio, base_ccy,
                             native_ccy](bool ok, QVector<QuoteData> quotes) {
        if (!self)
            return;
        QHash<QString, QuoteData> quote_map;
        if (ok) {
            for (const auto& q : quotes)
                quote_map[q.symbol] = q;
        }
        // Resolve each holding's native->base FX rate: 1.0 for base-currency,
        // the FX pair's price for foreign, 0.0 (blocks the snapshot) when the
        // currency is unknown or the FX pair didn't load.
        QHash<QString, double> symbol_rates;
        for (const auto& a : assets) {
            const QString ccy = native_ccy.value(a.symbol);
            if (ccy.isEmpty()) {
                symbol_rates.insert(a.symbol, 0.0); // currency not resolved yet
            } else if (ccy == base_ccy) {
                symbol_rates.insert(a.symbol, 1.0);
            } else {
                auto it = quote_map.find(ccy + base_ccy + QStringLiteral("=X"));
                symbol_rates.insert(a.symbol, (it != quote_map.end() && it->price > 0.0) ? it->price : 0.0);
            }
        }
        self->finalize_summary(portfolio_id, assets, portfolio, quote_map, symbol_rates);
    });
}

bool PortfolioService::try_broker_quotes(const QString& portfolio_id,
                                         const QVector<portfolio::PortfolioAsset>& assets,
                                         const portfolio::Portfolio& portfolio) {
    if (portfolio.broker_account_id.isEmpty())
        return false; // unlinked portfolio — yfinance owns it.

    // Resolve broker + creds. Any of these missing → fall back silently.
    auto& acct_mgr = trading::AccountManager::instance();
    const auto account = acct_mgr.get_account(portfolio.broker_account_id);
    if (account.account_id.isEmpty() || account.broker_id.isEmpty()) {
        LOG_WARN("PortfolioSvc",
                 QString("broker_account_id %1 not found — falling back to yfinance for %2")
                     .arg(portfolio.broker_account_id, portfolio_id));
        return false;
    }

    auto* broker = trading::BrokerRegistry::instance().get(account.broker_id);
    if (!broker) {
        LOG_WARN("PortfolioSvc",
                 QString("broker '%1' not registered — falling back to yfinance for %2")
                     .arg(account.broker_id, portfolio_id));
        return false;
    }

    const auto creds = acct_mgr.load_credentials(portfolio.broker_account_id);
    if (creds.access_token.isEmpty()) {
        LOG_WARN("PortfolioSvc",
                 QString("broker account %1 has no access_token — falling back to yfinance for %2")
                     .arg(portfolio.broker_account_id, portfolio_id));
        return false;
    }

    // Build the broker-native key list. Each asset stores broker_symbol +
    // exchange (populated at import time). Skip assets without a broker
    // pair — those slipped in before the v022 link or are non-broker rows
    // mixed into a broker-linked portfolio. They get current_price = 0 from
    // the lookup and the existing fallback (avg_buy_price) kicks in.
    QVector<QString> broker_keys;
    QHash<QString, QString> key_to_yf; // EXCHANGE:SYMBOL → asset.symbol (yfinance)
    broker_keys.reserve(assets.size());
    for (const auto& a : assets) {
        if (a.broker_symbol.isEmpty() || a.exchange.isEmpty())
            continue;
        const QString key = a.exchange + ":" + a.broker_symbol;
        broker_keys.append(key);
        key_to_yf.insert(key, a.symbol);
    }
    if (broker_keys.isEmpty()) {
        // Linked portfolio but no asset has a broker pair — nothing to fetch
        // via broker. Fall back to yfinance so the portfolio still renders.
        LOG_WARN("PortfolioSvc",
                 QString("broker-linked portfolio %1 has no broker_symbol/exchange on any asset — yfinance fallback")
                     .arg(portfolio_id));
        return false;
    }

    // BrokerHttp::execute blocks on QEventLoop (memory: project_broker_http_blocking).
    // Wrap the broker call in QtConcurrent::run so the UI thread stays responsive.
    // P8: capture a QPointer; result is posted back via QMetaObject::invokeMethod.
    // The QFuture return value is intentionally discarded — we don't need to
    // wait or cancel; lifetime is governed by the captured QPointer guards.
    QPointer<PortfolioService> self = this;
    (void)QtConcurrent::run([self, portfolio_id, assets, portfolio, broker, creds, broker_keys, key_to_yf]() {
        const auto resp = broker->get_quotes(creds, broker_keys);

        // Hop back to the UI thread (the one that owns *self) before touching
        // any shared state — finalize_summary writes to the cache mutex and
        // emits a signal that drives UI updates.
        QMetaObject::invokeMethod(self, [self, portfolio_id, assets, portfolio, resp, key_to_yf]() {
            if (!self)
                return;

            QHash<QString, QuoteData> quote_map;
            if (resp.success && resp.data.has_value()) {
                for (const auto& bq : resp.data.value()) {
                    // bq.symbol is the EXCHANGE:SYMBOL key (some brokers echo the
                    // request key in their response). Translate back to the
                    // canonical yfinance key the rest of the pipeline uses.
                    const QString yf_key = key_to_yf.value(bq.symbol);
                    if (yf_key.isEmpty())
                        continue; // unknown key — broker returned a row we didn't ask for, skip.

                    QuoteData q;
                    q.symbol = yf_key;
                    q.name = yf_key; // brokers don't return a friendly name on /quote.
                    q.price = bq.ltp;
                    q.change = bq.change;
                    q.change_pct = bq.change_pct;
                    q.high = bq.high;
                    q.low = bq.low;
                    q.volume = bq.volume;
                    quote_map.insert(yf_key, q);
                }
                LOG_INFO("PortfolioSvc",
                         QString("Broker quotes: %1 of %2 for portfolio %3")
                             .arg(quote_map.size()).arg(key_to_yf.size()).arg(portfolio_id));
            } else {
                // Broker call failed — log and fall back to yfinance for
                // _this_ refresh. The portfolio stays linked; next refresh
                // will retry the broker.
                LOG_WARN("PortfolioSvc",
                         QString("broker get_quotes failed for %1: %2 — falling back to yfinance")
                             .arg(portfolio_id, resp.error.left(200)));
                QStringList symbols;
                symbols.reserve(assets.size());
                for (const auto& a : assets)
                    symbols.append(a.symbol);
                MarketDataService::instance().fetch_quotes(symbols, [self, portfolio_id, assets,
                                                                     portfolio](bool ok, QVector<QuoteData> quotes) {
                    if (!self)
                        return;
                    QHash<QString, QuoteData> qm;
                    if (ok)
                        for (const auto& q : quotes)
                            qm.insert(q.symbol, q);
                    self->finalize_summary(portfolio_id, assets, portfolio, qm);
                });
                return;
            }

            self->finalize_summary(portfolio_id, assets, portfolio, quote_map);
        }, Qt::QueuedConnection);
    });

    return true; // broker path was taken
}

void PortfolioService::finalize_summary(const QString& portfolio_id,
                                        const QVector<portfolio::PortfolioAsset>& assets,
                                        const portfolio::Portfolio& portfolio,
                                        const QHash<QString, QuoteData>& quote_map,
                                        const QHash<QString, double>& symbol_rates) {
    // Per-holding pricing + FX conversion + aggregation is a pure function
    // (unit-tested in tst_portfolio_summary) that reports how many holdings had
    // no live quote and how many had an unresolved FX rate. Prefer the stored
    // sector; fall back to the resolver cache (which may populate async — see the
    // sector_resolved handler in the constructor). symbol_rates converts each
    // holding into the portfolio's base currency (empty on the broker path → 1.0).
    auto built = portfolio::build_summary(
        assets, portfolio, quote_map,
        [](const QString& sym) { return SectorResolver::instance().sector_for(sym); },
        [&symbol_rates](const QString& sym) { return symbol_rates.value(sym, 1.0); });
    portfolio::PortfolioSummary summary = built.summary;
    summary.last_updated = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    // Cache the result (P11)
    {
        QMutexLocker lock(&cache_mutex_);
        summary_cache_[portfolio_id] = {summary, QDateTime::currentSecsSinceEpoch()};
    }

    // Save a snapshot for performance history ONLY when the summary is fully
    // real: every holding priced from a live quote AND converted at a resolved
    // FX rate. A fabricated mark (unpriced fallback) or a guessed FX rate would
    // permanently contaminate the NAV history (the perf chart); snapshots resume
    // next refresh once all symbols quote and their currencies/FX resolve.
    // The virtual "All Accounts" id has no portfolios row (by design — see
    // kAllAccountsId), so it's excluded here too: portfolio_snapshots.portfolio_id
    // has a FK to portfolios(id) and the insert would just fail.
    if (portfolio_id == QLatin1String(kAllAccountsId)) {
        // no-op: nothing to snapshot the virtual aggregate against.
    } else if (built.snapshot_safe()) {
        QString today = QDate::currentDate().toString(Qt::ISODate);
        PortfolioRepository::instance().save_snapshot(portfolio_id, summary.total_market_value,
                                                      summary.total_cost_basis, summary.total_unrealized_pnl,
                                                      summary.total_unrealized_pnl_percent, today);
    } else {
        LOG_INFO("PortfolioSvc",
                 QString("Skipping NAV snapshot for %1: %2/%3 holdings unpriced, %4 with unresolved FX")
                     .arg(portfolio_id)
                     .arg(built.unpriced_count)
                     .arg(summary.holdings.size())
                     .arg(built.fx_unresolved_count));
    }

    emit summary_loaded(summary);
}

// ── Asset operations ─────────────────────────────────────────────────────────

} // namespace openmarketterminal::services
