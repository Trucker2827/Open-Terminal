#include <QtTest>

#include "screens/algo_trading/StrategyCockpitNavigation.h"

using namespace openmarketterminal::screens;

class StrategyCockpitNavigationTest final : public QObject {
    Q_OBJECT

  private slots:
    void hud_chips_have_specific_contexts() {
        const QSizeF size(1200, 700);
        QCOMPARE(strategy_cockpit_hit({25, 80}, size).view, StrategyCockpitView::EvidenceAll);
        QCOMPARE(strategy_cockpit_hit({18 + 1 * 134 + 10.0, 80}, size).view,
                 StrategyCockpitView::DecisionEnvelopes);
        QCOMPARE(strategy_cockpit_hit({18 + 3 * 134 + 10.0, 80}, size).view,
                 StrategyCockpitView::EvidenceOpen);
        QCOMPARE(strategy_cockpit_hit({18 + 6 * 134 + 10.0, 80}, size).view,
                 StrategyCockpitView::EvidenceNoEdge);
    }

    void fixed_flow_nodes_route_to_safe_drilldowns() {
        const QSizeF size(1200, 700);
        QCOMPARE(strategy_cockpit_hit({0.82 * 1200, 0.34 * 700}, size).view,
                 StrategyCockpitView::RiskSafety);
        QCOMPARE(strategy_cockpit_hit({0.86 * 1200, 0.52 * 700}, size).view,
                 StrategyCockpitView::PaperHandlers);
        QCOMPARE(strategy_cockpit_hit({0.80 * 1200, 0.70 * 700}, size).view,
                 StrategyCockpitView::Outcomes);
        QCOMPARE(strategy_cockpit_hit({0.18 * 1200, 0.34 * 700}, size).view,
                 StrategyCockpitView::EvidenceCoinbase);
        QCOMPARE(strategy_cockpit_hit({0.20 * 1200, 0.70 * 700}, size).view,
                 StrategyCockpitView::ResearchInputs);
    }

    void decoration_does_not_navigate() {
        QCOMPARE(strategy_cockpit_hit({5, 5}, {1200, 700}).view,
                 StrategyCockpitView::None);
    }

    void orbiting_proof_books_route_to_evidence() {
        const QSizeF size(1200, 700);
        const QPointF center(size.width() * 0.5, size.height() * 0.5 + 42.0);
        const qreal radius = qMin(size.width(), size.height()) * 0.31;
        const QPointF first_book(center.x(), center.y() - radius * 0.68);
        const auto hit = strategy_cockpit_hit(first_book, size, 0.0, 4);
        QCOMPARE(hit.view, StrategyCockpitView::EvidenceBook);
        QCOMPARE(hit.book_index, 0);
    }

    void evidence_cohorts_are_specific() {
        QVERIFY(strategy_evidence_matches(StrategyCockpitView::EvidenceChronos,
                                          "chronos2_1h", "coinbase", 12, 0, "COLLECTING"));
        QVERIFY(!strategy_evidence_matches(StrategyCockpitView::EvidenceChronos,
                                           "kalshi", "kalshi", 12, 0, "COLLECTING"));
        QVERIFY(strategy_evidence_matches(StrategyCockpitView::EvidenceOpen,
                                          "kalshi", "kalshi", 0, 3, "COLLECTING"));
        QVERIFY(strategy_evidence_matches(StrategyCockpitView::EvidenceNoEdge,
                                          "maker", "kraken", 377, 0, "NO EDGE"));
        QVERIFY(strategy_evidence_matches(StrategyCockpitView::EvidenceCoinbase,
                                          "spot", "Coinbase ticks", 9, 0, "COLLECTING"));
        QVERIFY(strategy_evidence_matches(StrategyCockpitView::EvidenceBook,
                                          "kalshi", "edge", 9, 0, "COLLECTING", "kalshi"));
        QVERIFY(!strategy_evidence_matches(StrategyCockpitView::EvidenceBook,
                                           "maker", "edge", 9, 0, "COLLECTING", "kalshi"));
    }
};

QTEST_GUILESS_MAIN(StrategyCockpitNavigationTest)
#include "tst_strategy_cockpit_navigation.moc"
