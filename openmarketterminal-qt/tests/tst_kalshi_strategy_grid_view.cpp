#include <QtTest>
#include "services/prediction/kalshi/KalshiStrategyGridView.h"
namespace k = openmarketterminal::services::prediction::kalshi_ns;

class TstKalshiStrategyGridView : public QObject {
    Q_OBJECT
    static QByteArray sample(const QString& as_of, const QString& body) {
        return QStringLiteral("{\"schema_version\":1,\"as_of_utc\":\"%1\",%2}")
            .arg(as_of, body).toUtf8();
    }
    static qint64 ms(const QString& iso) {
        return QDateTime::fromString(iso, Qt::ISODate).toMSecsSinceEpoch();
    }
private slots:
    void missing_file_is_unavailable() {
        auto g = k::parse_grid_latest(QByteArray(), 0);
        QVERIFY(!g.available);
        QCOMPARE(k::grid_cockpit_line(g), QStringLiteral("GRID: UNAVAILABLE"));
    }
    void garbage_is_unavailable() {
        QVERIFY(!k::parse_grid_latest(QByteArrayLiteral("not json"), 0).available);
    }
    void no_edge_headline() {
        auto g = k::parse_grid_latest(
            sample("2026-07-30T00:00:00+00:00",
                   "\"headline\":\"no variant beats hold+market after correction\","
                   "\"survivors\":[],\"candidates\":[]"),
            ms("2026-07-30T00:00:00Z"));
        QVERIFY(g.available);
        QCOMPARE(k::grid_cockpit_line(g), QStringLiteral("GRID: no measured edge"));
    }
    void candidate_line_when_no_survivor() {
        auto g = k::parse_grid_latest(
            sample("2026-07-30T00:00:00+00:00",
                   "\"headline\":\"no variant beats hold+market after correction\","
                   "\"survivors\":[],\"candidates\":[{\"variant_id\":\"NO|b10-25|mechanical|sl15\","
                   "\"side\":\"NO\",\"band\":[0.1,0.25],\"gate\":\"mechanical\","
                   "\"exit\":{\"kind\":\"sl\",\"amount\":0.15},\"delta_vs_hold\":0.02,"
                   "\"delta_vs_market\":0.008,\"effective_n\":34,\"trust\":\"rejected\","
                   "\"blocked_by\":\"not significant (BH); p=0.110\"}]"),
            ms("2026-07-30T00:00:00Z"));
        const QString line = k::grid_cockpit_line(g);
        QVERIFY(line.contains("forming"));
        QVERIFY(line.contains("n_eff 34"));
        QVERIFY(line.contains("vs mkt"));
    }
    void stale_is_tagged() {
        auto g = k::parse_grid_latest(
            sample("2026-07-30T00:00:00+00:00",
                   "\"headline\":\"no variant beats hold+market after correction\","
                   "\"survivors\":[],\"candidates\":[]"),
            ms("2026-07-30T00:00:00Z") + 7LL * 3600 * 1000);  // 7h later > 6h bound
        QVERIFY(g.stale);
        QVERIFY(k::grid_cockpit_line(g).contains("STALE"));
    }
};
QTEST_MAIN(TstKalshiStrategyGridView)
#include "tst_kalshi_strategy_grid_view.moc"
