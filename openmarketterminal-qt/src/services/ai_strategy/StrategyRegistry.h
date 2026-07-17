#pragma once
// StrategyRegistry.h — maps a strategy name to a factory + metadata so AI
// strategies are first-class plugins (ai strategy list) instead of hardcoded
// branches. Pure: no DB, no network.
#include "services/ai_strategy/Strategy.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

namespace openmarketterminal::ai_strategy {

struct StrategyBuildConfig {
    QStringList symbols;
    std::function<QString(const QString&)> completion; // CompletionFn for LLM strategies
};

struct StrategyInfo {
    QString name;
    QString description;
    bool needs_provider = false;
};

class StrategyRegistry {
  public:
    using Factory = std::function<std::unique_ptr<Strategy>(const StrategyBuildConfig&)>;
    void register_strategy(QString name, QString description, bool needs_provider, Factory factory);
    QVector<StrategyInfo> list() const;
    bool has(const QString& name) const;
    std::unique_ptr<Strategy> build(const QString& name, const StrategyBuildConfig& cfg) const;

  private:
    struct Entry { StrategyInfo info; Factory factory; };
    QVector<Entry> entries_;
};

// Registers the built-in strategies (meanrev, claude). Kept separate so tests
// can build an empty registry.
void register_builtin_strategies(StrategyRegistry& r);

} // namespace openmarketterminal::ai_strategy
