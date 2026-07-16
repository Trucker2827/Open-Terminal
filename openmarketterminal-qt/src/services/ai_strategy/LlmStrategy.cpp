// LlmStrategy.cpp — see LlmStrategy.h.
#include "services/ai_strategy/LlmStrategy.h"

#include "services/ai_ledger/AiLedger.h"
#include "services/ai_strategy/TypedAction.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

namespace openmarketterminal::ai_strategy {

LlmStrategy::LlmStrategy(QStringList universe, CompletionFn complete, double max_qty)
    : universe_(std::move(universe)), complete_(std::move(complete)), max_qty_(max_qty) {}

QVector<ActionChoice> LlmStrategy::parse_actions(const QString& reply, const QStringList& universe) {
    QVector<ActionChoice> actions;
    const int start = reply.indexOf(QLatin1Char('['));
    const int end = reply.lastIndexOf(QLatin1Char(']'));
    if (start < 0 || end < 0 || end < start)
        return actions;  // no array delimiters (empty / pure prose).
    const QString slice = reply.mid(start, end - start + 1);
    const QJsonDocument doc = QJsonDocument::fromJson(slice.toUtf8());
    if (!doc.isArray())
        return actions;
    for (const QJsonValue& v : doc.array()) {
        if (!v.isObject())
            continue;
        const QJsonObject obj = v.toObject();
        const QJsonValue symv = obj.value(QStringLiteral("symbol"));
        if (!symv.isString())
            continue;
        const QString sym = symv.toString();
        if (sym.isEmpty() || !universe.contains(sym))
            continue;  // drop out-of-universe / empty.
        const QString a = obj.value(QStringLiteral("action")).toString();
        ActionType type;
        if (a == QLatin1String("skip"))       type = ActionType::Skip;
        else if (a == QLatin1String("enter")) type = ActionType::Enter;
        else if (a == QLatin1String("trim"))  type = ActionType::Trim;
        else if (a == QLatin1String("exit"))  type = ActionType::Exit;
        else continue;  // unknown / missing action -> drop.
        const double conv = obj.value(QStringLiteral("conviction")).toDouble(1.0);
        actions.append(ActionChoice{sym, type, conv});
    }
    return actions;
}

QVector<TradeIntent> LlmStrategy::propose(const MarketSnapshot& s) {
    if (!complete_)
        return {};  // no brain wired ⇒ propose nothing (never invoke a null std::function).

    // Snapshot quotes as a compact JSON object {symbol: price}.
    QJsonObject quotes;
    for (auto it = s.quotes.constBegin(); it != s.quotes.constEnd(); ++it)
        quotes.insert(it.key(), it.value());

    QJsonArray universe_arr;
    for (const QString& sym : universe_)
        universe_arr.append(sym);

    const QString universe_csv = universe_.join(QStringLiteral(", "));

    const QString prompt =
        QStringLiteral(
            "You are a paper-trading assistant. Respond with ONLY a JSON array of typed actions and "
            "nothing else. Each action: {\"symbol\":..., \"action\":\"enter\"|\"trim\"|\"exit\"|\"skip\", "
            "\"conviction\":<0.0-1.0, for enter>}. enter opens/adds a long position sized by conviction; "
            "trim reduces the current position; exit closes it; skip does nothing. Return [] to do nothing. "
            "The environment chooses side and size and enforces every risk limit -- you only pick the verb "
            "and (for enter) a conviction 0..1. Only symbols in the allowed universe: %1.\n\n")
            .arg(universe_csv);

    const auto compact = [](const QJsonObject& o) {
        return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    };
    const auto compact_arr = [](const QJsonArray& a) {
        return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
    };

    const QString full = prompt + QStringLiteral("quotes: ") + compact(quotes) +
                         QStringLiteral("\nportfolio: ") + compact(s.portfolio) +
                         QStringLiteral("\nuniverse: ") + compact_arr(universe_arr);

    const QString reply = complete_(full);
    if (reply.isEmpty())
        return {};

    QVector<TradeIntent> intents;
    for (const ActionChoice& c : parse_actions(reply, universe_)) {
        const double net_qty = ai_ledger::position_of(name(), c.symbol).net_qty;
        if (auto intent = translate_action(c, net_qty, ActionParams{max_qty_, 0.5}))
            intents.append(*intent);
    }
    return intents;
}

} // namespace openmarketterminal::ai_strategy
