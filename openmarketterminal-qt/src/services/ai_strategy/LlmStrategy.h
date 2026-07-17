#pragma once
// LlmStrategy.h — Claude/LLM-driven strategy (Paper Strategy-Loop Driver, Task 3).
//
// A Strategy whose deciding brain is an LLM. Each tick it builds a prompt from
// the market snapshot + portfolio + universe, asks the LLM (via an injected
// completion function) for a JSON array of TYPED ACTIONS {enter|trim|exit|hold}
// (piece 5c; "skip" is still accepted as a hold synonym), parses them
// (parse_actions), and translates each verb + the symbol's current ledger
// position + the deterministic edge's direction (edge_dir_) into a paper
// intent (translate_action). The LLM output is UNTRUSTED: the strategy only
// enforces STRUCTURAL sanity + universe membership (so it can't propose
// disallowed symbols); the model picks the VERB (and, for enter, a
// conviction) — side/size come from the environment (the edge direction and
// caps), not the model, and all risk/caps are enforced downstream by the
// floor + prepare_order. propose() NEVER throws — any LLM output (empty,
// prose, markdown-fenced, malformed, non-array) yields a clean (possibly
// empty) QVector.
//
// The completion seam (CompletionFn) lets tests inject a fake; the real CLI
// (Task 4) injects a lambda wrapping the app's LlmService. The edge-direction
// seam (EdgeDirFn) likewise lets tests inject a fake direction; the default
// resolver reads the real deterministic edge (see LlmStrategy.cpp).
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

    /// The edge-direction seam: given a symbol, return the deterministic edge's
    /// recommended direction (+1 long, -1 short, 0 none/missing/stale/conflicting).
    /// Tests inject a fake; the default (see .cpp) reads assess() + the floor.
    using EdgeDirFn = std::function<int(const QString& symbol)>;

    LlmStrategy(QStringList universe, CompletionFn complete, double max_qty = 10.0,
                QString market = {}, EdgeDirFn edge_dir = {});

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
    QString market_;
    EdgeDirFn edge_dir_;
};

/// The default edge-direction resolver: the deterministic edge's ENDORSED direction
/// for `symbol` (+1 long, -1 short, 0 none/missing/stale/conflicting), scoped to
/// `market` (empty = all venues; reuses assess()'s market_venue_filter, see F1).
/// Reads assess() + floor_verdict + side_direction; never throws. Used as
/// LlmStrategy's default EdgeDirFn (via a market-capturing closure) and by the
/// `ai act` preview so both agree.
int edge_direction_of(const QString& symbol, const QString& market = {});

} // namespace openmarketterminal::ai_strategy
