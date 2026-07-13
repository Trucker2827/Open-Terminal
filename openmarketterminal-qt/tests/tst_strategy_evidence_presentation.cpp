#include "screens/algo_trading/StrategyEvidencePresentation.h"

#include <QtTest>

using openmarketterminal::screens::StrategyProofState;
using openmarketterminal::screens::strategy_proof_state;

class StrategyEvidencePresentationTest final : public QObject {
    Q_OBJECT
  private slots:
    void under_sampled_books_collect();
    void mature_losing_books_show_no_edge();
    void mature_positive_but_blocked_books_stay_blocked();
    void hypothetical_books_never_look_promotable();
    void eligibility_is_explicit();
};

void StrategyEvidencePresentationTest::under_sampled_books_collect() {
    QCOMPARE(strategy_proof_state(false, false, 29, -100.0, 30), StrategyProofState::Collecting);
}

void StrategyEvidencePresentationTest::mature_losing_books_show_no_edge() {
    QCOMPARE(strategy_proof_state(false, false, 30, 0.0, 30), StrategyProofState::NoEdge);
    QCOMPARE(strategy_proof_state(false, false, 137, -1250.0, 30), StrategyProofState::NoEdge);
}

void StrategyEvidencePresentationTest::mature_positive_but_blocked_books_stay_blocked() {
    QCOMPARE(strategy_proof_state(false, false, 42, 12.0, 30), StrategyProofState::Blocked);
}

void StrategyEvidencePresentationTest::hypothetical_books_never_look_promotable() {
    QCOMPARE(strategy_proof_state(true, true, 500, 1000.0, 30), StrategyProofState::Hypothetical);
}

void StrategyEvidencePresentationTest::eligibility_is_explicit() {
    QCOMPARE(strategy_proof_state(false, true, 50, 25.0, 30), StrategyProofState::PromotionReady);
}

QTEST_APPLESS_MAIN(StrategyEvidencePresentationTest)
#include "tst_strategy_evidence_presentation.moc"
