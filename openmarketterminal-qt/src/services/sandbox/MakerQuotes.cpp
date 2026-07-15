#include "services/sandbox/MakerQuotes.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace openmarketterminal::services::sandbox {

MakerQuotePair build_maker_quotes(double mid, double half_spread_bps) {
    MakerQuotePair pair;
    if (mid <= 0.0)
        return pair; // valid stays false
    pair.valid = true;
    pair.bid = MakerQuote{QStringLiteral("buy"), mid * (1.0 - half_spread_bps / 1e4)};
    pair.ask = MakerQuote{QStringLiteral("sell"), mid * (1.0 + half_spread_bps / 1e4)};
    return pair;
}

namespace {
QJsonObject maker_row(const QString& symbol, const QString& venue, const MakerQuote& q,
                      double freshest_age_ms, int live_sources, qint64 ts_ms) {
    return QJsonObject{
        {QStringLiteral("engine"), QStringLiteral("daemon-maker")},
        {QStringLiteral("paper"), true},
        {QStringLiteral("symbol"), symbol},
        {QStringLiteral("venue"), venue},
        {QStringLiteral("side"), q.side},
        {QStringLiteral("liquidity"), QStringLiteral("maker")},
        {QStringLiteral("action"), QStringLiteral("PAPER_MAKER_QUOTE")},
        {QStringLiteral("reference_price"), q.limit_price},
        {QStringLiteral("ts_ms"), QString::number(ts_ms)},
        {QStringLiteral("freshest_age_ms"), freshest_age_ms},
        {QStringLiteral("live_sources"), live_sources}};
}
} // namespace

QVector<QJsonObject> maker_decision_rows(const QString& symbol, const QString& venue, double mid,
                                         double half_spread_bps, double freshest_age_ms,
                                         int live_sources, qint64 ts_ms) {
    const MakerQuotePair pair = build_maker_quotes(mid, half_spread_bps);
    if (!pair.valid)
        return {}; // non-positive mid -> nothing to quote; consumers never see a bad-mid row
    return {maker_row(symbol, venue, pair.bid, freshest_age_ms, live_sources, ts_ms),
            maker_row(symbol, venue, pair.ask, freshest_age_ms, live_sources, ts_ms)};
}

void append_maker_decisions(const QString& path, const QString& symbol, const QString& venue,
                            double mid, double half_spread_bps, double freshest_age_ms,
                            int live_sources, qint64 ts_ms) {
    const QVector<QJsonObject> rows =
        maker_decision_rows(symbol, venue, mid, half_spread_bps, freshest_age_ms, live_sources, ts_ms);
    if (rows.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append))
        return;
    for (const QJsonObject& row : rows) {
        f.write(QJsonDocument(row).toJson(QJsonDocument::Compact));
        f.write("\n");
    }
}

} // namespace openmarketterminal::services::sandbox
