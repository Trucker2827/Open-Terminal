#pragma once
// LlmStrategy.h — Claude/LLM-driven strategy (Paper Strategy-Loop Driver, Task 3).
//
// A Strategy whose deciding brain is an LLM. Each tick it builds a prompt from
// the market snapshot + portfolio + universe, asks the LLM (via an injected
// completion function) for a JSON array of trade intents, and parses them. The
// LLM output is UNTRUSTED: the strategy only enforces STRUCTURAL sanity +
// universe membership (so it can't propose disallowed symbols); all risk/caps
// are enforced downstream by prepare_order. propose() NEVER throws — any LLM
// output (empty, prose, markdown-fenced, malformed, non-array) yields a clean
// (possibly empty) QVector.
//
// The completion seam (CompletionFn) lets tests inject a fake; the real CLI
// (Task 4) injects a lambda wrapping the app's LlmService.
#include "services/ai_strategy/Strategy.h"
#include "services/ai_strategy/TypedAction.h"

#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

namespace openmarketterminal::ai_strategy {

class LlmStrategy : public Strategy {
  public:
    /// The LLM seam: given a prompt, return the model's text reply.
    using CompletionFn = std::function<QString(const QString& prompt)>;

    LlmStrategy(QStringList universe, CompletionFn complete, double max_qty = 10.0);

    QString name() const override { return QStringLiteral("claude"); }
    QStringList universe() const override { return universe_; }
    QVector<TradeIntent> propose(const MarketSnapshot& s) override;

    /// Robust, side-effect-free parse of an LLM reply into universe-filtered typed
    /// actions. Exposed for direct unit testing. NEVER throws.
    static QVector<ActionChoice> parse_actions(const QString& reply, const QStringList& universe);

  private:
    QStringList universe_;
    CompletionFn complete_;
    double max_qty_ = 10.0;
};

} // namespace openmarketterminal::ai_strategy
