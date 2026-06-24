#pragma once
// Equity Trading types — UI state, enums, constants
// Kept separate from trading engine types to avoid coupling.

#include "ui/theme/ThemeManager.h"

#include <QColor>
#include <QString>
#include <QStringList>
#include <QVector>

namespace openmarketterminal::screens::equity {

// ── Enums ──────────────────────────────────────────────────────────────────

enum class BottomTab { Positions, Holdings, Orders, Funds, Stats };

enum class TradingMode { Paper, Live };

// ── "Leaders" watchlist — liquid US single names ───────────────────────────
// Highly-liquid US-listed tickers that quote cleanly on Alpaca's free IEX feed.
// Thin ADRs (BABA/SAP/NVO/SHEL/…) were dropped — they quote poorly on IEX — in
// favour of the most-traded mega-caps and high-volume movers.
inline const QStringList DEFAULT_WATCHLIST = {
    "AAPL", "MSFT", "NVDA", "GOOGL", "AMZN", "META", "TSLA", "AMD",  "AVGO", "NFLX",
    "CRM",  "ORCL", "JPM",  "V",     "MA",   "BAC",  "WMT",  "COST", "HD",   "UNH",
    "LLY",  "XOM",  "CVX",  "KO",    "PG",   "DIS",  "UBER", "PLTR", "COIN", "MSTR",
};

// ── "Markets" watchlist — cross-asset ETF overview ─────────────────────────
// A markets dashboard rather than single names: index / sector / asset-class
// ETFs Alpaca trades commission-free, plus spot-crypto ETFs (IBIT/ETHA) that
// give BTC/ETH exposure through the equity feed — these quote TODAY, before the
// native crypto data path is wired.
inline const QStringList MARKETS_WATCHLIST = {
    // Index
    "SPY", "QQQ", "DIA", "IWM",
    // Sectors (SPDR)
    "XLK", "XLF", "XLE", "XLV", "XLY", "XLI",
    // Asset classes
    "TLT", "GLD", "SLV", "USO", "UUP",
    // Volatility (the ^VIX index itself is not quotable via Alpaca's stock data)
    "VIXY",
    // Spot-crypto ETFs (trade as US equities)
    "IBIT", "ETHA",
};

// ── "Crypto" watchlist — Alpaca native pairs ───────────────────────────────
// Alpaca US crypto uses BASE/QUOTE symbols (slash-separated). These populate
// once the /v1beta3/crypto/us data path is wired; until then the rows are
// seeded and ready. Liquid core of Alpaca's supported pairs.
inline const QStringList CRYPTO_WATCHLIST = {
    "BTC/USD",  "ETH/USD",  "SOL/USD", "LTC/USD",  "BCH/USD", "DOGE/USD",
    "AVAX/USD", "LINK/USD", "UNI/USD", "AAVE/USD", "DOT/USD", "SHIB/USD",
};

inline const QStringList US_WATCHLIST = {
    "AAPL", "MSFT", "GOOGL", "AMZN", "NVDA", "META", "TSLA", "JPM", "V", "JNJ",
};

// ── Constants ──────────────────────────────────────────────────────────────

inline constexpr double DEFAULT_PAPER_BALANCE = 100000.0; // $100,000 USD
inline constexpr int OHLCV_FETCH_COUNT = 200;
inline constexpr int QUOTE_POLL_MS = 5000;
inline constexpr int PORTFOLIO_POLL_MS = 3000;
inline constexpr int WATCHLIST_POLL_MS = 10000;
inline constexpr int CLOCK_UPDATE_MS = 1000;

// ── Design System Colors — live from ThemeManager ──────────────────────────
// These are inline functions so they reflect the active theme at call time.

inline QColor COLOR_BUY() {
    return QColor(openmarketterminal::ui::ThemeManager::instance().tokens().positive);
}
inline QColor COLOR_SELL() {
    return QColor(openmarketterminal::ui::ThemeManager::instance().tokens().negative);
}
inline QColor COLOR_DIM() {
    return QColor(openmarketterminal::ui::ThemeManager::instance().tokens().text_secondary);
}
inline QColor COLOR_ACCENT() {
    return QColor(openmarketterminal::ui::ThemeManager::instance().tokens().accent);
}

inline QColor BG_BASE() {
    return QColor(openmarketterminal::ui::ThemeManager::instance().tokens().bg_base);
}
inline QColor BG_SURFACE() {
    return QColor(openmarketterminal::ui::ThemeManager::instance().tokens().bg_surface);
}
inline QColor BG_RAISED() {
    return QColor(openmarketterminal::ui::ThemeManager::instance().tokens().bg_raised);
}
inline QColor BG_HOVER() {
    return QColor(openmarketterminal::ui::ThemeManager::instance().tokens().bg_hover);
}

inline QColor BORDER_DIM() {
    return QColor(openmarketterminal::ui::ThemeManager::instance().tokens().border_dim);
}
inline QColor BORDER_MED() {
    return QColor(openmarketterminal::ui::ThemeManager::instance().tokens().border_med);
}
inline QColor BORDER_BRIGHT() {
    return QColor(openmarketterminal::ui::ThemeManager::instance().tokens().border_bright);
}

inline QColor TEXT_PRIMARY() {
    return QColor(openmarketterminal::ui::ThemeManager::instance().tokens().text_primary);
}
inline QColor TEXT_SECONDARY() {
    return QColor(openmarketterminal::ui::ThemeManager::instance().tokens().text_secondary);
}
inline QColor TEXT_TERTIARY() {
    return QColor(openmarketterminal::ui::ThemeManager::instance().tokens().text_tertiary);
}

inline QColor COLOR_WARNING() {
    return QColor(openmarketterminal::ui::ThemeManager::instance().tokens().warning);
}
inline QColor COLOR_INFO() {
    return QColor(openmarketterminal::ui::ThemeManager::instance().tokens().info);
}
inline QColor ROW_ALT() {
    return QColor(openmarketterminal::ui::ThemeManager::instance().tokens().row_alt);
}

// ── Exchange → Currency mapping ────────────────────────────────────────────

inline QString exchange_currency(const QString& exchange) {
    if (exchange == "NSE" || exchange == "BSE" || exchange == "NFO" || exchange == "MCX" || exchange == "CDS")
        return "INR";
    if (exchange == "NYSE" || exchange == "NASDAQ" || exchange == "AMEX" || exchange == "CBOE")
        return "USD";
    if (exchange == "LSE")
        return "GBP";
    if (exchange == "XETRA" || exchange == "EURONEXT")
        return "EUR";
    return "USD";
}

inline QString currency_symbol(const QString& currency) {
    if (currency == "INR")
        return QString::fromUtf8("\u20B9");
    if (currency == "GBP")
        return QString::fromUtf8("\u00A3");
    if (currency == "EUR")
        return QString::fromUtf8("\u20AC");
    if (currency == "JPY")
        return QString::fromUtf8("\u00A5");
    return "$";
}

// \u2500\u2500 Funds / Stats view-models \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
// The bottom-panel Funds and Stats tabs render from these view-models, so the
// panel is decoupled from the data source: live mode fills them from BrokerFunds
// (the subset it knows), paper mode fills the full set from the paper engine.

struct EquityFundsView {
    QString currency = QStringLiteral("\u20B9"); // symbol, e.g. \u20B9 / $
    bool is_paper = false;

    double available = 0.0;       // free cash / available margin
    double used_margin = 0.0;     // locked by open intraday positions + pending orders
    double total_equity = 0.0;    // net worth = available + used + holdings + open P&L
    double opening_balance = 0.0; // initial / start-of-day balance
    double holdings_value = 0.0;  // current value of CNC delivery holdings
    double realized_pnl = 0.0;    // realized P&L (today)
    double unrealized_pnl = 0.0;  // open positions + holdings mark-to-market
    double collateral = 0.0;      // pledged collateral (live brokers)
    double margin_util_pct = 0.0; // used / (used + available) * 100
};

struct EquityStatsView {
    QString currency = QStringLiteral("\u20B9");

    double net_pnl = 0.0;        // realized + unrealized
    double today_pnl = 0.0;      // realized today
    double realized_pnl = 0.0;
    double unrealized_pnl = 0.0;
    double return_pct = 0.0;     // net_pnl / opening_balance * 100
    double win_rate = 0.0;       // 0..1
    long long total_trades = 0;
    long long winning_trades = 0;
    long long losing_trades = 0;
    double profit_factor = 0.0;
    double avg_win = 0.0;
    double avg_loss = 0.0;
    double largest_win = 0.0;
    double largest_loss = 0.0;
    double total_fees = 0.0;
    double turnover = 0.0;
};

} // namespace openmarketterminal::screens::equity
