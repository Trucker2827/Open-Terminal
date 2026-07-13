#include "services/news/BtcNewsPulse.h"

#include <QDateTime>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::services::news {
namespace {

QString text_value(const QJsonObject& row, const char* primary, const char* fallback = nullptr) {
    QString value = row.value(QLatin1String(primary)).toString();
    if (value.isEmpty() && fallback) value = row.value(QLatin1String(fallback)).toString();
    return value;
}

QString normalize_paragraph(QString text) {
    text.remove(QRegularExpression(QStringLiteral("<[^>]+>")));
    text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    text = text.trimmed();
    if (text.size() > 640) text = text.left(637).trimmed() + QStringLiteral("...");
    return text;
}

QString signature(QString headline) {
    headline = headline.toLower();
    headline.remove(QRegularExpression(QStringLiteral("[^a-z0-9 ]")));
    static const QSet<QString> stop{QStringLiteral("a"), QStringLiteral("an"), QStringLiteral("and"),
        QStringLiteral("as"), QStringLiteral("at"), QStringLiteral("for"), QStringLiteral("from"),
        QStringLiteral("in"), QStringLiteral("is"), QStringLiteral("of"), QStringLiteral("on"),
        QStringLiteral("the"), QStringLiteral("to"), QStringLiteral("with")};
    QStringList tokens;
    for (const auto& token : headline.split(QLatin1Char(' '), Qt::SkipEmptyParts))
        if (!stop.contains(token)) tokens.append(token);
    std::sort(tokens.begin(), tokens.end());
    return tokens.mid(0, 12).join(QLatin1Char('|'));
}

bool similar_signature(const QString& left, const QString& right) {
    const QStringList left_tokens = left.split(QLatin1Char('|'), Qt::SkipEmptyParts);
    const QStringList right_tokens = right.split(QLatin1Char('|'), Qt::SkipEmptyParts);
    const QSet<QString> a(left_tokens.cbegin(), left_tokens.cend());
    const QSet<QString> b(right_tokens.cbegin(), right_tokens.cend());
    if (a.isEmpty() || b.isEmpty()) return false;
    int overlap = 0;
    for (const auto& token : a)
        if (b.contains(token)) ++overlap;
    const int union_size = a.size() + b.size() - overlap;
    return union_size > 0 && static_cast<double>(overlap) / union_size >= 0.62;
}

qint64 parse_time(const QJsonObject& row, qint64 now_ms) {
    const auto numeric = row.value(QStringLiteral("sort_ts"));
    if (numeric.isDouble() && numeric.toDouble() > 0)
        return static_cast<qint64>(numeric.toDouble()) * 1000;
    const QString raw = text_value(row, "time", "published_date");
    QDateTime parsed = QDateTime::fromString(raw, Qt::ISODate);
    if (!parsed.isValid()) parsed = QDateTime::fromString(raw, Qt::RFC2822Date);
    return parsed.isValid() ? parsed.toMSecsSinceEpoch() : now_ms;
}

bool contains_any(const QString& text, const QStringList& phrases, int* matches = nullptr) {
    int count = 0;
    for (const auto& phrase : phrases)
        if (text.contains(phrase)) ++count;
    if (matches) *matches = count;
    return count > 0;
}

QStringList catalyst_tags(const QString& text) {
    QStringList out;
    const QList<QPair<QString, QStringList>> groups{
        {QStringLiteral("ETF/FLOWS"), {QStringLiteral("etf"), QStringLiteral("inflow"), QStringLiteral("outflow")}},
        {QStringLiteral("REGULATION"), {QStringLiteral("sec "), QStringLiteral("regulat"), QStringLiteral("ban"), QStringLiteral("law")}},
        {QStringLiteral("MACRO"), {QStringLiteral("fed "), QStringLiteral("rates"), QStringLiteral("inflation"), QStringLiteral("liquidity")}},
        {QStringLiteral("SECURITY"), {QStringLiteral("hack"), QStringLiteral("exploit"), QStringLiteral("stolen")}},
        {QStringLiteral("WHALES"), {QStringLiteral("whale"), QStringLiteral("accumulat"), QStringLiteral("exchange wallet")}},
        {QStringLiteral("DERIVATIVES"), {QStringLiteral("liquidat"), QStringLiteral("funding rate"), QStringLiteral("open interest")}},
        {QStringLiteral("TREASURY"), {QStringLiteral("treasury"), QStringLiteral("reserve"), QStringLiteral("balance sheet")}},
        {QStringLiteral("MINING"), {QStringLiteral("miner"), QStringLiteral("hashrate"), QStringLiteral("mining")}},
    };
    for (const auto& group : groups)
        if (contains_any(text, group.second)) out.append(group.first);
    return out;
}

double tier_weight(int tier) {
    if (tier <= 1) return 1.0;
    if (tier == 2) return 0.85;
    if (tier == 3) return 0.65;
    return 0.45;
}

} // namespace

BtcNewsPulseResult BtcNewsPulse::analyze(const QJsonArray& articles, qint64 now_ms,
                                         int max_stories) {
    BtcNewsPulseResult result;
    result.as_of_ms = now_ms;
    result.scanned = articles.size();
    const QStringList bullish_phrases{
        QStringLiteral("etf inflow"), QStringLiteral("record inflow"), QStringLiteral("approval"),
        QStringLiteral("adoption"), QStringLiteral("accumulat"), QStringLiteral("strategic reserve"),
        QStringLiteral("rate cut"), QStringLiteral("liquidity boost"), QStringLiteral("breakout"),
        QStringLiteral("bitcoin rises"), QStringLiteral("bitcoin gains"), QStringLiteral("btc rises"),
        QStringLiteral("btc gains"), QStringLiteral("surge"), QStringLiteral("rally"), QStringLiteral("buy bitcoin"),
        QStringLiteral("adds bitcoin"), QStringLiteral("bullish"), QStringLiteral("short squeeze")};
    const QStringList bearish_phrases{
        QStringLiteral("etf outflow"), QStringLiteral("record outflow"), QStringLiteral("hack"),
        QStringLiteral("exploit"), QStringLiteral("ban"), QStringLiteral("crackdown"),
        QStringLiteral("lawsuit"), QStringLiteral("rate hike"), QStringLiteral("sell-off"),
        QStringLiteral("selloff"), QStringLiteral("plunge"), QStringLiteral("slump"),
        QStringLiteral("bearish"), QStringLiteral("bear market"), QStringLiteral("selling bitcoin"),
        QStringLiteral("sold bitcoin"), QStringLiteral("sold btc"), QStringLiteral("long liquidation"),
        QStringLiteral("stolen")};
    QStringList seen;
    QSet<QString> sources;
    QHash<QString, int> catalyst_counts;
    double weighted_score = 0.0;
    double total_weight = 0.0;
    double directional_weight = 0.0;
    double bullish_weight = 0.0;
    double bearish_weight = 0.0;

    for (const auto& value : articles) {
        const QJsonObject row = value.toObject();
        const QString headline = text_value(row, "headline", "title").trimmed();
        const QString summary = text_value(row, "summary", "description").trimmed();
        const QString tickers = QString::fromUtf8(
            QJsonDocument(row.value(QStringLiteral("tickers")).toArray()).toJson(QJsonDocument::Compact));
        const QString text = (headline + QLatin1Char(' ') + summary + QLatin1Char(' ') + tickers).toLower();
        const bool relevant = contains_any(text, {QStringLiteral("bitcoin"), QStringLiteral("btc"),
                                                   QStringLiteral("xbt"), QStringLiteral("satoshi")});
        if (!relevant || headline.isEmpty()) continue;
        const QString sig = signature(headline);
        const bool duplicate = std::any_of(seen.cbegin(), seen.cend(), [&](const QString& prior) {
            return prior == sig || similar_signature(prior, sig);
        });
        if (sig.isEmpty() || duplicate) { ++result.duplicates_removed; continue; }
        seen.append(sig);

        int positive = 0;
        int negative = 0;
        contains_any(text, bullish_phrases, &positive);
        contains_any(text, bearish_phrases, &negative);
        if (text.contains(QStringLiteral("combined inflow")) ||
            (text.contains(QStringLiteral("outflow streak")) &&
             contains_any(text, {QStringLiteral("snap"), QStringLiteral("end"), QStringLiteral("break")})))
            positive += 2;
        if (QRegularExpression(QStringLiteral("\\bsold\\b.{0,40}\\b(bitcoin|btc)\\b"))
                .match(text).hasMatch())
            ++negative;
        const QString supplied = row.value(QStringLiteral("sentiment")).toString().toUpper();
        // Feed-level sentiment may describe a company share price or the
        // article's tone rather than BTC. It may reinforce explicit Bitcoin
        // language, but it must never create direction by itself.
        if (positive > 0 && supplied == QStringLiteral("BULLISH")) ++positive;
        else if (negative > 0 && supplied == QStringLiteral("BEARISH")) ++negative;
        const double raw = std::clamp((positive - negative) / 3.0, -1.0, 1.0);
        const qint64 published_ts = parse_time(row, now_ms);
        const double age_hours = std::max(0.0, (now_ms - published_ts) / 3'600'000.0);
        const double recency = std::exp(-std::log(2.0) * age_hours / 6.0);
        const int tier = row.value(QStringLiteral("tier")).toInt(3);
        const QString impact = row.value(QStringLiteral("impact")).toString().toUpper();
        const double impact_weight = impact == QStringLiteral("HIGH") ? 1.35
            : impact == QStringLiteral("MEDIUM") ? 1.10 : 0.85;
        const double source_weight = tier_weight(tier);
        const double weight = recency * source_weight * impact_weight;

        BtcNewsStory story;
        story.id = row.value(QStringLiteral("id")).toString();
        story.headline = headline;
        story.source = text_value(row, "source", "publisher");
        story.link = row.value(QStringLiteral("link")).toString();
        story.published = text_value(row, "time", "published_date");
        story.published_ts = published_ts;
        story.score = raw * 100.0;
        story.relevance = std::clamp(0.55 + (text.contains(QStringLiteral("bitcoin")) ? 0.25 : 0.10)
                                     + (!summary.isEmpty() ? 0.10 : 0.0), 0.0, 1.0);
        story.source_weight = source_weight;
        story.direction = raw > 0.12 ? QStringLiteral("UP")
            : raw < -0.12 ? QStringLiteral("DOWN") : QStringLiteral("NEUTRAL");
        story.catalysts = catalyst_tags(text);
        story.horizon = contains_any(text, {QStringLiteral("breaking"), QStringLiteral("liquidat"),
                                             QStringLiteral("hack"), QStringLiteral("etf flow")})
            ? QStringLiteral("1H")
            : contains_any(text, {QStringLiteral("fed"), QStringLiteral("regulat"), QStringLiteral("approval")})
                ? QStringLiteral("24H+") : QStringLiteral("SESSION");
        const QString publisher_paragraph = normalize_paragraph(summary.isEmpty()
            ? headline + QStringLiteral(". The feed supplied no additional article synopsis; treat this headline-only classification with lower confidence.")
            : summary);
        const QString catalyst_reason = story.catalysts.isEmpty()
            ? QStringLiteral("no strong named catalyst") : story.catalysts.join(QStringLiteral(", "));
        story.paragraph = normalize_paragraph(
            publisher_paragraph + QStringLiteral(" Market read: %1 over %2; %3. This is an evidence label, not a guaranteed price forecast.")
                .arg(story.direction, story.horizon, catalyst_reason));
        for (const auto& catalyst : story.catalysts) ++catalyst_counts[catalyst];
        if (!story.source.isEmpty()) sources.insert(story.source.toUpper());
        if (story.direction == QStringLiteral("UP")) { ++result.bullish; bullish_weight += weight; }
        else if (story.direction == QStringLiteral("DOWN")) { ++result.bearish; bearish_weight += weight; }
        else ++result.neutral;
        weighted_score += raw * weight;
        total_weight += weight;
        if (std::abs(raw) > 0.12) directional_weight += weight;
        result.stories.append(story);
    }

    std::sort(result.stories.begin(), result.stories.end(), [](const auto& left, const auto& right) {
        const double left_rank = std::abs(left.score) * left.source_weight + left.relevance * 20.0;
        const double right_rank = std::abs(right.score) * right.source_weight + right.relevance * 20.0;
        return left_rank == right_rank ? left.published_ts > right.published_ts : left_rank > right_rank;
    });
    if (result.stories.size() > max_stories) result.stories.resize(max_stories);
    result.relevant = seen.size();
    result.distinct_sources = sources.size();
    result.score = total_weight > 0.0 ? std::clamp(weighted_score / total_weight * 100.0, -100.0, 100.0) : 0.0;
    result.source_agreement = directional_weight > 0.0
        ? std::abs(bullish_weight - bearish_weight) / directional_weight : 0.0;
    const double breadth = std::min(1.0, result.distinct_sources / 4.0);
    const double sample = std::min(1.0, result.relevant / 8.0);
    const double directional_coverage = result.relevant > 0
        ? static_cast<double>(result.bullish + result.bearish) / result.relevant : 0.0;
    result.confidence = std::clamp(
        (0.40 * result.source_agreement + 0.30 * breadth + 0.30 * sample) *
        (0.60 + 0.40 * directional_coverage), 0.0, 0.92);
    const bool conflict = bullish_weight > 0.0 && bearish_weight > 0.0 && result.source_agreement < 0.45;
    if (result.relevant < 2) result.verdict = QStringLiteral("NOT ENOUGH NEWS");
    else if (conflict) result.verdict = QStringLiteral("CONFLICTED");
    else if (result.score >= 12.0) result.verdict = QStringLiteral("UP");
    else if (result.score <= -12.0) result.verdict = QStringLiteral("DOWN");
    else result.verdict = QStringLiteral("NEUTRAL");

    QList<QPair<QString, int>> ranked_catalysts;
    for (auto it = catalyst_counts.cbegin(); it != catalyst_counts.cend(); ++it)
        ranked_catalysts.append({it.key(), it.value()});
    std::sort(ranked_catalysts.begin(), ranked_catalysts.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });
    for (const auto& item : ranked_catalysts.mid(0, 5)) result.catalysts.append(item.first);
    result.explanation = QStringLiteral("%1 relevant stories from %2 sources: %3 bullish, %4 bearish, %5 neutral; weighted narrative pressure %6/100 with %7% source agreement.")
        .arg(result.relevant).arg(result.distinct_sources).arg(result.bullish).arg(result.bearish)
        .arg(result.neutral).arg(result.score, 0, 'f', 1).arg(result.source_agreement * 100.0, 0, 'f', 0);
    result.caveat = QStringLiteral("Advisory news context only. It is not a calibrated price probability and cannot trigger an order; score it against later BTC returns before promotion.");
    return result;
}

QJsonObject BtcNewsPulse::to_json(const BtcNewsPulseResult& pulse) {
    QJsonArray stories;
    for (const auto& story : pulse.stories) {
        stories.append(QJsonObject{{"id", story.id}, {"headline", story.headline},
            {"summary", story.paragraph}, {"source", story.source}, {"link", story.link},
            {"published", story.published}, {"published_ts", QString::number(story.published_ts)},
            {"direction", story.direction}, {"horizon", story.horizon},
            {"catalysts", QJsonArray::fromStringList(story.catalysts)}, {"score", story.score},
            {"relevance", story.relevance}, {"source_weight", story.source_weight}});
    }
    return QJsonObject{{"verdict", pulse.verdict}, {"score", pulse.score},
        {"confidence", pulse.confidence}, {"source_agreement", pulse.source_agreement},
        {"scanned", pulse.scanned}, {"relevant", pulse.relevant},
        {"duplicates_removed", pulse.duplicates_removed}, {"bullish", pulse.bullish},
        {"bearish", pulse.bearish}, {"neutral", pulse.neutral},
        {"distinct_sources", pulse.distinct_sources},
        {"catalysts", QJsonArray::fromStringList(pulse.catalysts)},
        {"explanation", pulse.explanation}, {"caveat", pulse.caveat},
        {"as_of_ms", QString::number(pulse.as_of_ms)}, {"stories", stories},
        {"model_role", "advisory_only"}, {"can_trigger_order", false}};
}

} // namespace openmarketterminal::services::news
