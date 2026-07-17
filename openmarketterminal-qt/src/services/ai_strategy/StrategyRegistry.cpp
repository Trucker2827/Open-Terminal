#include "services/ai_strategy/StrategyRegistry.h"
#include "services/ai_strategy/LlmStrategy.h"
#include "services/ai_strategy/MeanReversionStrategy.h"

namespace openmarketterminal::ai_strategy {

void StrategyRegistry::register_strategy(QString name, QString description, bool needs_provider,
                                         Factory factory) {
    entries_.push_back(Entry{StrategyInfo{std::move(name), std::move(description), needs_provider},
                             std::move(factory)});
}

QVector<StrategyInfo> StrategyRegistry::list() const {
    QVector<StrategyInfo> out;
    for (const auto& e : entries_) out.push_back(e.info);
    return out;
}

bool StrategyRegistry::has(const QString& name) const {
    for (const auto& e : entries_) if (e.info.name == name) return true;
    return false;
}

std::unique_ptr<Strategy> StrategyRegistry::build(const QString& name,
                                                  const StrategyBuildConfig& cfg) const {
    for (const auto& e : entries_)
        if (e.info.name == name) return e.factory(cfg);
    return nullptr;
}

void register_builtin_strategies(StrategyRegistry& r) {
    r.register_strategy(QStringLiteral("meanrev"), QStringLiteral("Deterministic mean-reversion"),
                        /*needs_provider=*/false,
                        [](const StrategyBuildConfig& c) -> std::unique_ptr<Strategy> {
                            return std::make_unique<MeanReversionStrategy>(c.symbols);
                        });
    r.register_strategy(QStringLiteral("claude"), QStringLiteral("LLM-driven (active provider)"),
                        /*needs_provider=*/true,
                        [](const StrategyBuildConfig& c) -> std::unique_ptr<Strategy> {
                            return std::make_unique<LlmStrategy>(c.symbols, c.completion, 10.0, c.market);
                        });
}

} // namespace openmarketterminal::ai_strategy
