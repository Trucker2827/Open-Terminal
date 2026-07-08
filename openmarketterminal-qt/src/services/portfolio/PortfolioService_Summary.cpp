// src/services/portfolio/PortfolioService_Summary.cpp
//
// Summary loading: load_summary, refresh_summary, build_summary,
// try_broker_quotes, finalize_summary. These produce the PortfolioSummary
// object the UI binds to.
//
// Part of the partial-class split of PortfolioService.cpp.

#include "services/portfolio/PortfolioService.h"

#include "core/logging/Logger.h"
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
    QStringList symbols;
    symbols.reserve(assets.size());
    for (const auto& a : assets)
        symbols.append(a.symbol);

    QPointer<PortfolioService> self = this;
    MarketDataService::instance().fetch_quotes(symbols, [self, portfolio_id, assets,
                                                         portfolio](bool ok, QVector<QuoteData> quotes) {
        if (!self)
            return;
        QHash<QString, QuoteData> quote_map;
        if (ok) {
            for (const auto& q : quotes)
                quote_map[q.symbol] = q;
        }
        self->finalize_summary(portfolio_id, assets, portfolio, quote_map);
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
                                        const QHash<QString, QuoteData>& quote_map) {
    // Per-holding pricing + aggregation is a pure function (unit-tested in
    // tst_portfolio_summary) that also reports how many holdings had no live
    // quote. Prefer the stored sector; fall back to the resolver cache (which
    // may populate async — see the sector_resolved handler in the constructor).
    auto built = portfolio::build_summary(
        assets, portfolio, quote_map,
        [](const QString& sym) { return SectorResolver::instance().sector_for(sym); });
    portfolio::PortfolioSummary summary = built.summary;
    summary.last_updated = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    // Cache the result (P11)
    {
        QMutexLocker lock(&cache_mutex_);
        summary_cache_[portfolio_id] = {summary, QDateTime::currentSecsSinceEpoch()};
    }

    // Save a snapshot for performance history ONLY when every holding was priced
    // from a live quote. If any fell back to its average buy price, the total
    // market value contains a fabricated mark; persisting it would permanently
    // contaminate the NAV history (the perf chart). Snapshots resume next refresh
    // once all symbols quote.
    if (built.unpriced_count == 0) {
        QString today = QDate::currentDate().toString(Qt::ISODate);
        PortfolioRepository::instance().save_snapshot(portfolio_id, summary.total_market_value,
                                                      summary.total_cost_basis, summary.total_unrealized_pnl,
                                                      summary.total_unrealized_pnl_percent, today);
    } else {
        LOG_INFO("PortfolioSvc",
                 QString("Skipping NAV snapshot for %1: %2 of %3 holdings unpriced")
                     .arg(portfolio_id)
                     .arg(built.unpriced_count)
                     .arg(summary.holdings.size()));
    }

    emit summary_loaded(summary);
}

// ── Asset operations ─────────────────────────────────────────────────────────

} // namespace openmarketterminal::services
