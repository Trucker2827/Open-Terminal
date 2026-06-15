#pragma once
// Strategy.h — strategy-loop seams (Paper Strategy-Loop Driver, Task 1).
//
// Header-only interfaces + value types shared by StrategyRunner and any
// concrete strategy. Strategy-agnostic by design: the runner only knows these
// abstractions, so it can be driven by fakes in tests and by the live MCP
// substrate in production. NO execution logic lives here.
#include "mcp/McpTypes.h"

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace openmarketterminal::ai_strategy {

/// One snapshot of market + account state handed to a strategy each tick.
struct MarketSnapshot {
    qint64 ts = 0;                 ///< QDateTime::currentMSecsSinceEpoch() at build time.
    QMap<QString, double> quotes;  ///< symbol → last price (omitted when unavailable / zero).
    QJsonObject portfolio;         ///< best-effort pm_paper_portfolio() data (may be empty).
};

/// A trade intent is just the prepare_order argument object — the runner passes
/// it straight through to prepare_order, so strategies own the order shape.
using TradeIntent = QJsonObject;

/// Seam over the substrate MCP tools. The runner calls ONLY reads
/// (get_quote / pm_paper_portfolio) + prepare_order + submit_order(mode:paper).
class ToolCaller {
  public:
    virtual ~ToolCaller() = default;
    virtual mcp::ToolResult call(const QString& name, const QJsonObject& args) = 0;
};

/// A strategy: declares the symbols it cares about, proposes trade intents from
/// a snapshot, and is notified of fills. on_fill defaults to a no-op.
class Strategy {
  public:
    virtual ~Strategy() = default;
    virtual QString name() const = 0;
    virtual QStringList universe() const = 0;             ///< symbols/markets to fetch each tick.
    virtual QVector<TradeIntent> propose(const MarketSnapshot&) = 0;
    virtual void on_fill(const TradeIntent& /*intent*/,
                         const QJsonObject& /*submit_result*/) {}
};

} // namespace openmarketterminal::ai_strategy
