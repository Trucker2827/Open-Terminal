// LlmStrategy.cpp — see LlmStrategy.h.
#include "services/ai_strategy/LlmStrategy.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

namespace openmarketterminal::ai_strategy {

LlmStrategy::LlmStrategy(QStringList universe, CompletionFn complete)
    : universe_(std::move(universe)), complete_(std::move(complete)) {}

QVector<TradeIntent> LlmStrategy::parse_intents(const QString& reply, const QStringList& universe) {
    QVector<TradeIntent> intents;

    // Robust array extraction: tolerate prose / markdown fences around the JSON
    // by slicing from the first '[' to the last ']' (inclusive).
    const int start = reply.indexOf(QLatin1Char('['));
    const int end = reply.lastIndexOf(QLatin1Char(']'));
    if (start < 0 || end < 0 || end < start)
        return intents;  // no array delimiters (covers empty / pure-prose replies).

    const QString slice = reply.mid(start, end - start + 1);
    const QJsonDocument doc = QJsonDocument::fromJson(slice.toUtf8());
    if (!doc.isArray())
        return intents;  // not a JSON array ⇒ nothing.

    const QJsonArray arr = doc.array();
    for (const QJsonValue& v : arr) {
        if (!v.isObject())
            continue;  // skip non-object elements.
        const QJsonObject obj = v.toObject();

        // Structural sanity + universe membership only — the substrate validates
        // the rest (order_type, limit_price, risk/caps). Pass the object through
        // UNCHANGED so strategies own the order shape.
        const QJsonValue symv = obj.value(QStringLiteral("symbol"));
        if (!symv.isString())
            continue;
        const QString sym = symv.toString();
        if (sym.isEmpty() || !universe.contains(sym))
            continue;  // drop out-of-universe / empty symbols.

        const QString side = obj.value(QStringLiteral("side")).toString();
        if (side != QLatin1String("buy") && side != QLatin1String("sell"))
            continue;

        if (obj.value(QStringLiteral("quantity")).toDouble() <= 0.0)
            continue;  // missing / non-positive / non-numeric ⇒ 0 ⇒ dropped.

        intents.append(obj);
    }
    return intents;
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
            "You are a paper-trading assistant. Given the market snapshot and current "
            "portfolio, respond with ONLY a JSON array of order intents and nothing else. "
            "Each intent: {\"symbol\":..., \"side\":\"buy\"|\"sell\", \"quantity\":<number>, "
            "\"order_type\":\"limit\", \"limit_price\":<number>}. Return [] to do nothing. "
            "Only trade symbols in the allowed universe: %1. A deterministic risk system "
            "enforces all limits — oversized or invalid intents are rejected, so propose "
            "conservatively.\n\n")
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

    return parse_intents(reply, universe_);
}

} // namespace openmarketterminal::ai_strategy
