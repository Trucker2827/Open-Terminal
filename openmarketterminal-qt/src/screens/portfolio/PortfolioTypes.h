// src/screens/portfolio/PortfolioTypes.h
#pragma once
#include <QDateTime>
#include <QString>
#include <QVector>

#include <optional>

namespace openmarketterminal::portfolio {

// ── Core entities ────────────────────────────────────────────────────────────

struct Portfolio {
    QString id;
    QString name;
    QString owner;
    QString currency = "USD";
    QString description;
    QString created_at;
    QString updated_at;
    /// Empty for manually-created portfolios. Set when imported from a
    /// connected broker account — links back to broker_accounts.id so
    /// PortfolioService can route live quote fetches through the broker
    /// instead of yfinance. See migration v022.
    QString broker_account_id;
    /// Identifies the connected account this portfolio mirrors (e.g.
    /// "broker:<account_id>"). Empty for manually-created / JSON-imported
    /// portfolios — AccountSyncService only ever touches portfolios with a
    /// non-empty sync_source. See migration v060.
    QString sync_source;
    /// ISO-8601 timestamp of the last successful sync. Empty = never synced.
    QString synced_at;
    /// Last sync error message, if any. Cleared on the next successful sync.
    QString sync_error;
};

struct PortfolioAsset {
    int id = 0;
    QString portfolio_id;
    QString symbol;          // canonical: yfinance-format ("AAPL"). Used by sparklines, replay, news, sectors.
    double quantity = 0;
    double avg_buy_price = 0;
    QString first_purchase_date;
    QString last_updated;
    QString sector;          // empty = not yet resolved; filled from import JSON or SectorResolver
    /// Broker-native ticker (e.g. "AAPL", no exchange suffix). Empty for
    /// manually-imported assets. Combined with `exchange` to form the
    /// EXCHANGE:SYMBOL key some brokers need for /quote calls.
    QString broker_symbol;
    /// Exchange code. Empty for non-broker imports.
    QString exchange;
    /// False for holdings whose cost basis is not known (e.g. a crypto
    /// exchange balance imported with quantity only). market_value still
    /// counts toward NAV; cost_basis / unrealized_pnl are excluded from
    /// portfolio totals. See migration v060.
    bool has_cost_basis = true;
};

struct Transaction {
    QString id;
    QString portfolio_id;
    QString symbol;
    QString transaction_type; // BUY, SELL, DIVIDEND, SPLIT
    double quantity = 0;
    double price = 0;
    double total_value = 0;
    QString transaction_date;
    QString notes;
    QString created_at;
};

// ── Live-enriched data ───────────────────────────────────────────────────────

struct HoldingWithQuote {
    // From PortfolioAsset
    QString symbol;
    double quantity = 0;
    double avg_buy_price = 0;
    QString sector;

    // Live market data
    double current_price = 0;
    double market_value = 0;
    double cost_basis = 0;
    double unrealized_pnl = 0;
    double unrealized_pnl_percent = 0;
    double day_change = 0;
    double day_change_percent = 0;
    double weight = 0; // % of total portfolio
    // False when no live quote was found for this symbol: current_price is a
    // fallback (avg buy price), so market value / P&L / day change are NOT real.
    // Consumers can badge it as unpriced; NAV snapshots skip such summaries.
    bool priced = true;
    // False when the FX rate to the portfolio's base currency was not resolved:
    // values were converted at 1.0 (shown in the native currency), so they are
    // the wrong scale. Consumers badge it like unpriced; NAV snapshots skip it.
    bool fx_resolved = true;
    // False when the source asset has no known cost basis (e.g. a crypto
    // exchange balance): cost_basis / unrealized_pnl are zeroed and excluded
    // from portfolio totals, but market_value still counts toward NAV.
    // Consumers badge it for display.
    bool has_cost_basis = true;
};

struct PortfolioSummary {
    Portfolio portfolio;
    QVector<HoldingWithQuote> holdings;

    double total_market_value = 0;
    double total_cost_basis = 0;
    double total_unrealized_pnl = 0;
    double total_unrealized_pnl_percent = 0;
    double total_day_change = 0;
    double total_day_change_percent = 0;
    int total_positions = 0;
    int gainers = 0;
    int losers = 0;
    QString last_updated;
};

// ── Computed analytics ───────────────────────────────────────────────────────

struct ComputedMetrics {
    std::optional<double> sharpe;
    std::optional<double> sortino;            // annualized, downside-deviation based
    std::optional<double> beta;
    std::optional<double> alpha;              // annualized Jensen's alpha vs benchmark, %
    std::optional<double> volatility;         // annualized %
    std::optional<double> max_drawdown;       // %
    std::optional<double> var_95;             // 1-day VaR in currency
    std::optional<double> cvar_95;            // 1-day CVaR (expected shortfall) in currency
    std::optional<double> risk_score;         // 0-100 composite
    std::optional<double> concentration_top3; // sum of top 3 weights %
};

// ── Snapshot for performance history ─────────────────────────────────────────

struct PortfolioSnapshot {
    int id = 0;
    QString portfolio_id;
    double total_value = 0;
    double total_cost_basis = 0;
    double total_pnl = 0;
    double total_pnl_percent = 0;
    QString snapshot_date;
};

// ── Enums ────────────────────────────────────────────────────────────────────

enum class HeatmapMode { Pnl, Weight, DayChange };

enum class SortColumn { Symbol, Price, Change, Pnl, PnlPct, Weight, MarketValue };

enum class SortDirection { Asc, Desc };

enum class DetailView {
    AnalyticsSectors,
    PerfRisk,
    Optimization,
    QuantStats,
    ReportsPme,
    Indices,
    RiskMgmt,
    Planning,
    Economics
};

// ── Import/Export types ──────────────────────────────────────────────────────

struct PortfolioExportTransaction {
    QString date;
    QString symbol;
    QString type; // BUY, SELL, DIVIDEND, SPLIT
    double quantity = 0;
    double price = 0;
    double total_value = 0;
    QString notes;
};

struct PortfolioExportData {
    QString format_version = "1.0";
    QString portfolio_name;
    QString owner;
    QString currency;
    QString export_date;
    QVector<PortfolioExportTransaction> transactions;
};

enum class ImportMode { New, Merge };

struct ImportResult {
    QString portfolio_id;
    QString portfolio_name;
    int transactions_replayed = 0;
    QStringList errors;
};

} // namespace openmarketterminal::portfolio
