#include "services/prediction/kalshi/KalshiStrategyGridView.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace openmarketterminal::services::prediction::kalshi_ns {

namespace {

constexpr int kSchemaVersion = 1;

QString cents(double dollars) {
    const double c = dollars * 100.0;
    return QStringLiteral("%1%2c")
        .arg(c >= 0 ? QStringLiteral("+") : QStringLiteral("-"))
        .arg(qAbs(c), 0, 'f', 1);
}

QString band_tag(const QJsonArray& band) {
    if (band.size() < 2) return QString();
    return QStringLiteral("%1-%2c")
        .arg(qRound(band.at(0).toDouble() * 100.0))
        .arg(qRound(band.at(1).toDouble() * 100.0));
}

QString exit_tag(const QJsonObject& exit) {
    const QString kind = exit.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("hold") || exit.value(QStringLiteral("amount")).isNull())
        return kind;
    return QStringLiteral("%1%2").arg(kind).arg(
        qRound(exit.value(QStringLiteral("amount")).toDouble() * 100.0));
}

GridRow parse_row(const QJsonObject& o) {
    GridRow r;
    r.variant_id = o.value(QStringLiteral("variant_id")).toString();
    r.side = o.value(QStringLiteral("side")).toString();
    r.band = band_tag(o.value(QStringLiteral("band")).toArray());
    r.gate = o.value(QStringLiteral("gate")).toString();
    r.exit = exit_tag(o.value(QStringLiteral("exit")).toObject());
    r.trust = o.value(QStringLiteral("trust")).toString();
    r.blocked_by = o.value(QStringLiteral("blocked_by")).toString();
    r.delta_vs_hold = o.value(QStringLiteral("delta_vs_hold")).toDouble();
    r.delta_vs_market = o.value(QStringLiteral("delta_vs_market")).toDouble();
    r.effective_n = o.value(QStringLiteral("effective_n")).toDouble();
    return r;
}

QVector<GridRow> parse_rows(const QJsonArray& a) {
    QVector<GridRow> out;
    out.reserve(a.size());
    for (const auto& v : a) out.append(parse_row(v.toObject()));
    return out;
}

QString desc(const GridRow& r) {
    return QStringLiteral("%1 %2 + %3").arg(r.side, r.band, r.exit);
}

QString stale_suffix(const GridLatest& g) {
    if (!g.stale || g.age_ms < 0) return QString();
    return QStringLiteral(" (STALE %1h)").arg(g.age_ms / (3600LL * 1000LL));
}

}  // namespace

GridLatest parse_grid_latest(const QByteArray& json, qint64 now_ms) {
    GridLatest g;  // fails closed: available=false by default
    if (json.isEmpty()) return g;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return g;
    const QJsonObject root = doc.object();
    const int version = root.value(QStringLiteral("schema_version")).toInt(-1);
    if (version != kSchemaVersion) return g;   // unknown schema -> unavailable

    g.available = true;
    g.schema_version = version;
    g.headline = root.value(QStringLiteral("headline")).toString();
    g.survivors = parse_rows(root.value(QStringLiteral("survivors")).toArray());
    g.candidates = parse_rows(root.value(QStringLiteral("candidates")).toArray());

    const QString as_of = root.value(QStringLiteral("as_of_utc")).toString();
    const QDateTime dt = QDateTime::fromString(as_of, Qt::ISODate);
    if (dt.isValid() && now_ms > 0) {
        g.age_ms = now_ms - dt.toMSecsSinceEpoch();
        g.stale = g.age_ms > kGridStaleMs;
    }
    return g;
}

QString grid_cockpit_line(const GridLatest& g) {
    if (!g.available) return QStringLiteral("GRID: UNAVAILABLE");
    QString body;
    if (!g.survivors.isEmpty()) {
        const GridRow& r = g.survivors.first();
        body = QStringLiteral("GRID ADVISORY: %1 %2 vs mkt (n_eff %3)")
                   .arg(desc(r), cents(r.delta_vs_market)).arg(qRound(r.effective_n));
    } else if (!g.candidates.isEmpty()) {
        const GridRow& r = g.candidates.first();
        body = QStringLiteral("GRID: forming — %1 %2 vs mkt, %3 (n_eff %4)")
                   .arg(desc(r), cents(r.delta_vs_market), r.blocked_by)
                   .arg(qRound(r.effective_n));
    } else {
        body = QStringLiteral("GRID: no measured edge");
    }
    return body + stale_suffix(g);
}

QString grid_cli_report(const GridLatest& g, bool as_json, const QByteArray& raw) {
    if (as_json) return QString::fromUtf8(raw);
    if (!g.available) return QStringLiteral("strategy-grid: UNAVAILABLE (no readable "
                                            "kalshi-strategy-grid-latest.json)");
    QStringList out;
    out << QStringLiteral("strategy-grid — %1%2").arg(g.headline, stale_suffix(g));
    if (!g.survivors.isEmpty()) {
        out << QStringLiteral("  survivors (measured):");
        for (const GridRow& r : g.survivors)
            out << QStringLiteral("    %1  %2 vs hold, %3 vs mkt  (n_eff %4)")
                       .arg(desc(r), cents(r.delta_vs_hold), cents(r.delta_vs_market))
                       .arg(qRound(r.effective_n));
    }
    if (!g.candidates.isEmpty()) {
        out << QStringLiteral("  closest candidates (not yet measured):");
        for (const GridRow& r : g.candidates)
            out << QStringLiteral("    %1  %2 vs hold, %3 vs mkt  (n_eff %4) — %5")
                       .arg(desc(r), cents(r.delta_vs_hold), cents(r.delta_vs_market))
                       .arg(qRound(r.effective_n)).arg(r.blocked_by);
    }
    if (g.survivors.isEmpty() && g.candidates.isEmpty())
        out << QStringLiteral("  (no survivors, no positive candidates)");
    return out.join(QLatin1Char('\n'));
}

}  // namespace openmarketterminal::services::prediction::kalshi_ns
