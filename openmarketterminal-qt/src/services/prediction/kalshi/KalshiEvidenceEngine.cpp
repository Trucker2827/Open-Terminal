#include "services/prediction/kalshi/KalshiEvidenceEngine.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::services::prediction::kalshi_ns {

namespace {

double number(const QVariant& value) {
    bool ok = false;
    const double out = value.toString().toDouble(&ok);
    return ok ? out : value.toDouble();
}

double outcome_price(const PredictionMarket& market, int index) {
    return index >= 0 && index < market.outcomes.size() ? market.outcomes[index].price : 0.0;
}

struct Quote {
    double bid = 0.0;
    double ask = 0.0;
    double mid = 0.0;
    double bid_size = 0.0;
    double ask_size = 0.0;
    qint64 ts_ms = 0;
};

Quote quote_for(const PredictionMarket& market,
                const QHash<QString, PredictionOrderBook>& books,
                const QString& side) {
    const bool yes = side == QStringLiteral("yes");
    const QString asset = market.key.market_id +
                          (yes ? QStringLiteral(":yes") : QStringLiteral(":no"));
    Quote out;
    const auto it = books.constFind(asset);
    if (it != books.cend()) {
        if (!it->bids.isEmpty()) {
            out.bid = it->bids.first().price;
            out.bid_size = it->bids.first().size;
        }
        if (!it->asks.isEmpty()) {
            out.ask = it->asks.first().price;
            out.ask_size = it->asks.first().size;
        }
        out.ts_ms = it->last_update_ms;
    }
    if (out.bid <= 0.0) out.bid = outcome_price(market, yes ? 0 : 1);
    if (out.ask <= 0.0)
        out.ask = number(market.extras.value(yes ? QStringLiteral("yes_ask_dollars")
                                                 : QStringLiteral("no_ask_dollars")));
    if (out.bid > 0.0 && out.ask > 0.0) out.mid = (out.bid + out.ask) / 2.0;
    else out.mid = std::max(out.bid, out.ask);
    return out;
}

struct Contract {
    const PredictionMarket* market = nullptr;
    QString kind;
    double floor = 0.0;
    double cap = 0.0;
    Quote yes;
    Quote no;
};

Contract contract_from(const PredictionMarket& market,
                       const QHash<QString, PredictionOrderBook>& books) {
    Contract out;
    out.market = &market;
    out.floor = number(market.extras.value(QStringLiteral("floor_strike")));
    out.cap = number(market.extras.value(QStringLiteral("cap_strike")));
    const QString strike_type = market.extras.value(QStringLiteral("strike_type")).toString().toLower();
    const QString text = (market.question + QStringLiteral(" ") +
                          market.extras.value(QStringLiteral("yes_sub_title")).toString()).toLower();
    if (out.floor > 0.0 && out.cap > out.floor) out.kind = QStringLiteral("range");
    else if (out.floor > 0.0 && (strike_type.contains(QStringLiteral("greater")) ||
                                 text.contains(QStringLiteral("above")) ||
                                 text.contains(QStringLiteral("at least"))))
        out.kind = QStringLiteral("above");
    else if (out.cap > 0.0 && (strike_type.contains(QStringLiteral("less")) ||
                               text.contains(QStringLiteral("below"))))
        out.kind = QStringLiteral("below");
    out.yes = quote_for(market, books, QStringLiteral("yes"));
    out.no = quote_for(market, books, QStringLiteral("no"));
    return out;
}

QJsonObject quote_json(const Quote& quote) {
    return QJsonObject{{QStringLiteral("bid"), quote.bid},
                       {QStringLiteral("ask"), quote.ask},
                       {QStringLiteral("mid"), quote.mid},
                       {QStringLiteral("bid_size"), quote.bid_size},
                       {QStringLiteral("ask_size"), quote.ask_size},
                       {QStringLiteral("ts_ms"), QString::number(quote.ts_ms)}};
}

QVector<QJsonObject> feature_rows(const QString& path) {
    QVector<QJsonObject> rows;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return rows;
    while (!file.atEnd()) {
        const auto doc = QJsonDocument::fromJson(file.readLine());
        if (doc.isObject() && doc.object().value(QStringLiteral("event")).toString() ==
                                  QStringLiteral("kalshi_venue_feature_snapshot"))
            rows.push_back(doc.object());
    }
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
        return left.value(QStringLiteral("ts")).toString() < right.value(QStringLiteral("ts")).toString();
    });
    return rows;
}

qint64 timestamp_ms(const QJsonObject& row) {
    return QDateTime::fromString(row.value(QStringLiteral("ts")).toString(), Qt::ISODate)
        .toMSecsSinceEpoch();
}

const QJsonObject* future_row(const QVector<QJsonObject>& rows, int index, qint64 horizon_ms) {
    const qint64 target = timestamp_ms(rows[index]) + horizon_ms;
    const QString symbol = rows[index].value(QStringLiteral("symbol")).toString();
    const QString market = rows[index].value(QStringLiteral("kalshi_market_id")).toString();
    const qint64 tolerance = horizon_ms <= 15'000 ? 15'000 : (horizon_ms <= 60'000 ? 30'000 : 90'000);
    for (int i = index + 1; i < rows.size(); ++i) {
        const qint64 ts = timestamp_ms(rows[i]);
        if (ts < target) continue;
        if (ts > target + tolerance) return nullptr;
        if (rows[i].value(QStringLiteral("symbol")).toString() != symbol) continue;
        if (!market.isEmpty() && rows[i].value(QStringLiteral("kalshi_market_id")).toString() != market)
            continue;
        return &rows[i];
    }
    return nullptr;
}

} // namespace

double KalshiEvidenceEngine::conservative_taker_fee(double price, double contracts) {
    if (price <= 0.0 || price >= 1.0 || contracts <= 0.0) return 0.0;
    const double dollars = 0.07 * contracts * price * (1.0 - price);
    return std::ceil(dollars * 100.0 - 1e-9) / 100.0;
}

double KalshiEvidenceEngine::conservative_taker_fee(const PredictionMarket& market,
                                                    double price, double contracts) {
    const QString waiver = market.extras.value(QStringLiteral("fee_waiver_expiration_time")).toString();
    const QDateTime waiver_end = QDateTime::fromString(waiver, Qt::ISODate);
    if (waiver_end.isValid() && waiver_end > QDateTime::currentDateTimeUtc()) return 0.0;
    if (market.extras.value(QStringLiteral("fee_type")).toString().compare(
            QStringLiteral("none"), Qt::CaseInsensitive) == 0)
        return 0.0;
    const double multiplier = market.extras.contains(QStringLiteral("fee_multiplier"))
        ? std::max(0.0, number(market.extras.value(QStringLiteral("fee_multiplier"))))
        : 1.0;
    if (price <= 0.0 || price >= 1.0 || contracts <= 0.0) return 0.0;
    const double dollars = 0.07 * multiplier * contracts * price * (1.0 - price);
    return std::ceil(dollars * 100.0 - 1e-9) / 100.0;
}

QJsonArray KalshiEvidenceEngine::analyze_ladder(
    const QVector<PredictionMarket>& markets,
    const QHash<QString, PredictionOrderBook>& books,
    const QString& event_ticker) {
    QVector<Contract> contracts;
    for (const auto& market : markets) {
        if (!event_ticker.isEmpty() && market.key.event_id != event_ticker) continue;
        auto contract = contract_from(market, books);
        if (!contract.kind.isEmpty()) contracts.push_back(contract);
    }

    QVector<Contract> above;
    QVector<Contract> ranges;
    for (const auto& contract : contracts) {
        if (contract.kind == QStringLiteral("above")) above.push_back(contract);
        if (contract.kind == QStringLiteral("range")) ranges.push_back(contract);
    }
    std::sort(above.begin(), above.end(), [](const auto& a, const auto& b) { return a.floor < b.floor; });

    QJsonArray diagnostics;
    const auto add = [&diagnostics](const QString& kind, const QString& severity,
                                    const QString& detail, const QJsonArray& tickers,
                                    double edge = 0.0, double min_depth = 0.0) {
        diagnostics.append(QJsonObject{{QStringLiteral("kind"), kind},
                                       {QStringLiteral("severity"), severity},
                                       {QStringLiteral("detail"), detail},
                                       {QStringLiteral("tickers"), tickers},
                                       {QStringLiteral("net_edge"), edge},
                                       {QStringLiteral("min_depth"), min_depth}});
    };

    for (const auto& contract : contracts) {
        if (contract.yes.ask <= 0.0 || contract.no.ask <= 0.0) continue;
        const double fees = conservative_taker_fee(*contract.market, contract.yes.ask) +
                            conservative_taker_fee(*contract.market, contract.no.ask);
        const double edge = 1.0 - contract.yes.ask - contract.no.ask - fees;
        if (edge > 0.0) {
            const double depth = std::min(contract.yes.ask_size, contract.no.ask_size);
            add(QStringLiteral("complement_portfolio"),
                depth >= 1.0 ? QStringLiteral("opportunity") : QStringLiteral("watch"),
                depth >= 1.0
                    ? QStringLiteral("YES + NO asks cost less than guaranteed $1 payout after fees")
                    : QStringLiteral("Positive price relationship, but displayed ask depth is unavailable"),
                QJsonArray{contract.market->key.market_id}, edge, depth);
        }
    }

    for (int i = 0; i < above.size(); ++i) {
        if (i + 1 < above.size()) {
            const auto& lower = above[i];
            const auto& upper = above[i + 1];
            if (lower.yes.mid > 0.0 && upper.yes.mid > lower.yes.mid + 0.02)
                add(QStringLiteral("monotonicity_violation"), QStringLiteral("warning"),
                    QStringLiteral("Higher strike is priced above lower strike"),
                    QJsonArray{lower.market->key.market_id, upper.market->key.market_id},
                    upper.yes.mid - lower.yes.mid);
            if (lower.yes.ask > 0.0 && upper.no.ask > 0.0) {
                const double fees = conservative_taker_fee(*lower.market, lower.yes.ask) +
                                    conservative_taker_fee(*upper.market, upper.no.ask);
                const double edge = 1.0 - lower.yes.ask - upper.no.ask - fees;
                if (edge > 0.0) {
                    const double depth = std::min(lower.yes.ask_size, upper.no.ask_size);
                    add(QStringLiteral("nested_above_portfolio"),
                        depth >= 1.0 ? QStringLiteral("opportunity") : QStringLiteral("watch"),
                        depth >= 1.0
                            ? QStringLiteral("Lower-strike YES + higher-strike NO guarantees at least $1")
                            : QStringLiteral("Nested price relationship is positive, but displayed ask depth is unavailable"),
                        QJsonArray{lower.market->key.market_id, upper.market->key.market_id}, edge, depth);
                }
            }
        }
        if (i > 0 && i + 1 < above.size()) {
            const auto& lower = above[i - 1];
            const auto& middle = above[i];
            const auto& upper = above[i + 1];
            if (lower.yes.mid <= 0.0 || middle.yes.mid <= 0.0 || upper.yes.mid <= 0.0) continue;
            const double span = upper.floor - lower.floor;
            if (span <= 0.0) continue;
            const double weight = (middle.floor - lower.floor) / span;
            const double interpolated = lower.yes.mid + weight * (upper.yes.mid - lower.yes.mid);
            const double residual = middle.yes.mid - interpolated;
            if (std::abs(residual) >= 0.05)
                add(QStringLiteral("stale_strike_candidate"), QStringLiteral("watch"),
                    QStringLiteral("Middle strike differs from neighboring implied CDF by %1c")
                        .arg(std::abs(residual) * 100.0, 0, 'f', 1),
                    QJsonArray{lower.market->key.market_id, middle.market->key.market_id,
                               upper.market->key.market_id}, std::abs(residual));
        }
    }

    const auto nearest_above = [&above](double strike) -> const Contract* {
        const Contract* best = nullptr;
        double distance = 1e100;
        for (const auto& contract : above) {
            const double d = std::abs(contract.floor - strike);
            if (d < distance) { distance = d; best = &contract; }
        }
        return best && distance <= 0.011 ? best : nullptr;
    };
    for (const auto& range : ranges) {
        const Contract* lower = nearest_above(range.floor);
        const Contract* upper = nearest_above(range.cap);
        if (!lower || !upper) continue;
        if (lower->yes.mid > 0.0 && upper->yes.mid > 0.0 && range.yes.mid > 0.0) {
            const double implied = std::max(0.0, lower->yes.mid - upper->yes.mid);
            const double residual = range.yes.mid - implied;
            if (std::abs(residual) >= 0.04)
                add(QStringLiteral("range_relative_value"), QStringLiteral("watch"),
                    QStringLiteral("Range differs from adjacent above-contract CDF by %1c")
                        .arg(std::abs(residual) * 100.0, 0, 'f', 1),
                    QJsonArray{range.market->key.market_id, lower->market->key.market_id,
                               upper->market->key.market_id}, std::abs(residual));
        }
        if (lower->no.ask > 0.0 && range.yes.ask > 0.0 && upper->yes.ask > 0.0) {
            const double fees = conservative_taker_fee(*lower->market, lower->no.ask) +
                                conservative_taker_fee(*range.market, range.yes.ask) +
                                conservative_taker_fee(*upper->market, upper->yes.ask);
            const double edge = 1.0 - lower->no.ask - range.yes.ask - upper->yes.ask - fees;
            if (edge > 0.0) {
                const double depth = std::min({lower->no.ask_size, range.yes.ask_size,
                                               upper->yes.ask_size});
                add(QStringLiteral("range_partition_portfolio"),
                    depth >= 1.0 ? QStringLiteral("opportunity") : QStringLiteral("watch"),
                    depth >= 1.0
                        ? QStringLiteral("Below lower + in range + above upper partition costs under $1")
                        : QStringLiteral("Partition price relationship is positive, but displayed ask depth is unavailable"),
                    QJsonArray{lower->market->key.market_id, range.market->key.market_id,
                               upper->market->key.market_id}, edge, depth);
            }
        }
    }
    return diagnostics;
}

QJsonObject KalshiEvidenceEngine::ladder_snapshot(
    const QVector<PredictionMarket>& markets,
    const QHash<QString, PredictionOrderBook>& books,
    const QString& event_ticker,
    qint64 ts_ms) {
    QJsonArray contracts;
    for (const auto& market : markets) {
        if (!event_ticker.isEmpty() && market.key.event_id != event_ticker) continue;
        const auto contract = contract_from(market, books);
        contracts.append(QJsonObject{{QStringLiteral("ticker"), market.key.market_id},
                                     {QStringLiteral("kind"), contract.kind},
                                     {QStringLiteral("floor_strike"), contract.floor},
                                     {QStringLiteral("cap_strike"), contract.cap},
                                     {QStringLiteral("yes"), quote_json(contract.yes)},
                                     {QStringLiteral("no"), quote_json(contract.no)},
                                     {QStringLiteral("close_time"), market.end_date_iso},
                                     {QStringLiteral("status"), market.extras.value(QStringLiteral("status")).toString()}});
    }
    return QJsonObject{{QStringLiteral("event"), QStringLiteral("kalshi_ladder_snapshot")},
                       {QStringLiteral("schema_version"), 2},
                       {QStringLiteral("ts"), QDateTime::fromMSecsSinceEpoch(ts_ms).toUTC().toString(Qt::ISODateWithMs)},
                       {QStringLiteral("event_ticker"), event_ticker},
                       {QStringLiteral("contracts"), contracts},
                       {QStringLiteral("diagnostics"), analyze_ladder(markets, books, event_ticker)},
                       {QStringLiteral("live_eligible"), false}};
}

bool KalshiEvidenceEngine::append_jsonl(const QString& path, const QJsonObject& row) {
    constexpr qint64 max_bytes = 64LL * 1024 * 1024;
    if (QFileInfo::exists(path) && QFileInfo(path).size() >= max_bytes) {
        QFile::remove(path + QStringLiteral(".1"));
        if (!QFile::rename(path, path + QStringLiteral(".1")))
            return false;
    }
    QFile file(path);
    QDir().mkpath(QFileInfo(file).absolutePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return false;
    file.write(QJsonDocument(row).toJson(QJsonDocument::Compact));
    file.write("\n");
    return true;
}

int KalshiEvidenceEngine::reconcile_forward_labels(const QString& features_path,
                                                   const QString& labels_path) {
    const auto rows = feature_rows(features_path);
    if (rows.isEmpty()) return 0;
    QSet<QString> existing;
    const auto label_key = [](const QJsonObject& row) {
        return row.value(QStringLiteral("source_ts")).toString() + QLatin1Char('|') +
               row.value(QStringLiteral("symbol")).toString() + QLatin1Char('|') +
               row.value(QStringLiteral("kalshi_market_id")).toString();
    };
    QFile labels(labels_path);
    if (labels.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!labels.atEnd()) {
            const auto doc = QJsonDocument::fromJson(labels.readLine());
            if (doc.isObject()) existing.insert(label_key(doc.object()));
        }
    }
    int written = 0;
    for (int i = 0; i < rows.size(); ++i) {
        const QString source_ts = rows[i].value(QStringLiteral("ts")).toString();
        const QString key = source_ts + QLatin1Char('|') +
                            rows[i].value(QStringLiteral("symbol")).toString() + QLatin1Char('|') +
                            rows[i].value(QStringLiteral("kalshi_market_id")).toString();
        if (source_ts.isEmpty() || existing.contains(key)) continue;
        const QJsonObject* r15 = future_row(rows, i, 15'000);
        const QJsonObject* r60 = future_row(rows, i, 60'000);
        const QJsonObject* r300 = future_row(rows, i, 300'000);
        if (!r300) continue;
        const double base = rows[i].value(QStringLiteral("reference_mid")).toDouble();
        if (base <= 0.0) continue;
        QJsonObject label{{QStringLiteral("event"), QStringLiteral("kalshi_venue_forward_label")},
                          {QStringLiteral("source_ts"), source_ts},
                          {QStringLiteral("symbol"), rows[i].value(QStringLiteral("symbol"))},
                          {QStringLiteral("kalshi_market_id"), rows[i].value(QStringLiteral("kalshi_market_id"))},
                          {QStringLiteral("reference_mid"), base},
                          {QStringLiteral("live_eligible"), false}};
        const auto add_horizon = [&label, base, &rows, i](const QString& suffix, const QJsonObject* future) {
            if (!future) return;
            const double spot = future->value(QStringLiteral("reference_mid")).toDouble();
            label.insert(QStringLiteral("spot_%1_bps").arg(suffix), (spot / base - 1.0) * 10000.0);
            const double y0 = rows[i].value(QStringLiteral("kalshi_yes_price")).toDouble();
            const double y1 = future->value(QStringLiteral("kalshi_yes_price")).toDouble();
            if (y0 > 0.0 && y1 > 0.0)
                label.insert(QStringLiteral("kalshi_yes_%1_change").arg(suffix), y1 - y0);
        };
        add_horizon(QStringLiteral("15s"), r15);
        add_horizon(QStringLiteral("1m"), r60);
        add_horizon(QStringLiteral("5m"), r300);
        if (append_jsonl(labels_path, label)) { existing.insert(key); ++written; }
    }
    return written;
}

QJsonObject KalshiEvidenceEngine::settlement_label(const PredictionMarket& market,
                                                   const QString& features_path) {
    const double expiration = number(market.extras.value(QStringLiteral("expiration_value")));
    const QDateTime close = QDateTime::fromString(market.end_date_iso, Qt::ISODate);
    double proxy_sum = 0.0;
    int samples = 0;
    if (close.isValid()) {
        const qint64 end = close.toMSecsSinceEpoch();
        for (const auto& row : feature_rows(features_path)) {
            if (row.value(QStringLiteral("kalshi_market_id")).toString() != market.key.market_id) continue;
            const qint64 ts = timestamp_ms(row);
            if (ts > end - 60'000 && ts <= end) {
                const double mid = row.value(QStringLiteral("reference_mid")).toDouble();
                if (mid > 0.0) { proxy_sum += mid; ++samples; }
            }
        }
    }
    const double proxy = samples > 0 ? proxy_sum / samples : 0.0;
    QJsonObject out{{QStringLiteral("event"), QStringLiteral("kalshi_settlement_label")},
                    {QStringLiteral("ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                    {QStringLiteral("kalshi_market_id"), market.key.market_id},
                    {QStringLiteral("event_ticker"), market.key.event_id},
                    {QStringLiteral("result"), market.extras.value(QStringLiteral("result")).toString()},
                    {QStringLiteral("expiration_value"), expiration},
                    {QStringLiteral("settlement_value_dollars"), number(market.extras.value(QStringLiteral("settlement_value_dollars")))},
                    {QStringLiteral("settlement_ts"), market.extras.value(QStringLiteral("settlement_ts")).toString()},
                    {QStringLiteral("proxy_final_60s_average"), proxy},
                    {QStringLiteral("proxy_samples"), samples},
                    {QStringLiteral("live_eligible"), false}};
    if (expiration > 0.0 && proxy > 0.0) {
        out.insert(QStringLiteral("basis_error_usd"), expiration - proxy);
        out.insert(QStringLiteral("basis_error_bps"), (expiration / proxy - 1.0) * 10000.0);
    }
    return out;
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
