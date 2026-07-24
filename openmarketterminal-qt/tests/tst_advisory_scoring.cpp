#include <QtTest>
#include "services/edge_radar/AdvisoryScoring.h"
using namespace openmarketterminal;

class TstAdvisoryScoring : public QObject {
    Q_OBJECT
private slots:
    void brier_matches_hand_computation() {
        QVector<adv::ScoredRow> rows{{0.9,0.9,0.5,0.5,1,"all"},{0.2,0.2,0.5,0.5,0,"all"}};
        auto r = adv::score_paired(rows, 200);
        QVERIFY(qAbs(r.brier_pre - ((0.01+0.04)/2)) < 1e-9);
        QVERIFY(r.improvement_vs_market_pre > 0);   // pre beats a 0.5 market on these
        QVERIFY(r.ci_low <= r.ci_high);
    }

    void participation_counts_all_states() {
        auto p = adv::participation({"COMMITTED_POST","EXPIRED","ABANDONED","COMMITTED_BLIND"});
        QCOMPARE(p.opened, 4); QCOMPARE(p.expired, 1);
        QVERIFY(qAbs(p.expiration_rate - 0.25) < 1e-9);
    }

    void bootstrap_ci_brackets_point_estimate() {
        QVector<adv::ScoredRow> rows{
            {0.9,0.9,0.5,0.6,1,"all"},
            {0.2,0.2,0.5,0.4,0,"all"},
            {0.8,0.8,0.5,0.6,1,"all"},
            {0.1,0.1,0.5,0.4,0,"all"},
            {0.7,0.7,0.6,0.7,1,"all"},
        };
        auto r = adv::score_paired(rows, 500, 12345);
        QVERIFY(r.ci_low <= r.improvement_vs_daemon_pre);
        QVERIFY(r.improvement_vs_daemon_pre <= r.ci_high);
    }

    void bootstrap_ci_is_deterministic_for_same_seed() {
        QVector<adv::ScoredRow> rows{
            {0.9,0.9,0.5,0.6,1,"all"},
            {0.2,0.2,0.5,0.4,0,"all"},
            {0.8,0.8,0.5,0.6,1,"all"},
            {0.1,0.1,0.5,0.4,0,"all"},
        };
        auto r1 = adv::score_paired(rows, 500, 777);
        auto r2 = adv::score_paired(rows, 500, 777);
        QCOMPARE(r1.ci_low, r2.ci_low);
        QCOMPARE(r1.ci_high, r2.ci_high);
    }

    void empty_rows_do_not_crash() {
        QVector<adv::ScoredRow> rows;
        auto r = adv::score_paired(rows, 200);
        QCOMPARE(r.n, 0);
        QVERIFY(!std::isnan(r.brier_pre));
        QVERIFY(!std::isnan(r.ci_low));
        QVERIFY(!std::isnan(r.ci_high));
    }

    void empty_states_do_not_crash() {
        auto p = adv::participation({});
        QCOMPARE(p.opened, 0);
        QVERIFY(qFuzzyCompare(p.open_to_commit_rate + 1.0, 1.0));
        QVERIFY(qFuzzyCompare(p.expiration_rate + 1.0, 1.0));
    }
};
QTEST_MAIN(TstAdvisoryScoring)
#include "tst_advisory_scoring.moc"
