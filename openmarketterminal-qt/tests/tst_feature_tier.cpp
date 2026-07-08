#include "python/FeatureTier.h"
#include <QtTest>
using namespace openmarketterminal::python;

class TestFeatureTier : public QObject {
    Q_OBJECT
  private slots:
    void coreAlwaysReady();
    void aiReadyOnlyWhenFullyInstalled();
    void aiInstallingWins();
    void forecastingNeedsAiPlusChronos();
};

static SetupStatus mk(bool needs_setup, bool v1, bool v2) {
    SetupStatus s;
    s.needs_setup = needs_setup;
    s.venv_numpy1_ready = v1;
    s.venv_numpy2_ready = v2;
    s.install_dir = "/tmp/x";
    return s;
}

void TestFeatureTier::coreAlwaysReady() {
    QCOMPARE(tier_state_from(Tier::Core, mk(true, false, false), false, false, false), TierState::Ready);
    QCOMPARE(tier_state_from(Tier::Core, mk(false, true, true), true, true, true), TierState::Ready);
}

void TestFeatureTier::aiReadyOnlyWhenFullyInstalled() {
    // Fully installed + python available -> Ready
    QCOMPARE(tier_state_from(Tier::Ai, mk(false, true, true), true, false, false), TierState::Ready);
    // Any missing piece -> NotInstalled
    QCOMPARE(tier_state_from(Tier::Ai, mk(true, true, true), true, false, false), TierState::NotInstalled);
    QCOMPARE(tier_state_from(Tier::Ai, mk(false, false, true), true, false, false), TierState::NotInstalled);
    QCOMPARE(tier_state_from(Tier::Ai, mk(false, true, true), false, false, false), TierState::NotInstalled);
}

void TestFeatureTier::aiInstallingWins() {
    // installing flag overrides everything for Ai
    QCOMPARE(tier_state_from(Tier::Ai, mk(true, false, false), false, false, true), TierState::Installing);
}

void TestFeatureTier::forecastingNeedsAiPlusChronos() {
    auto ready = mk(false, true, true);
    // Ai ready but no chronos -> NotInstalled
    QCOMPARE(tier_state_from(Tier::Forecasting, ready, true, false, false), TierState::NotInstalled);
    // Ai ready + chronos -> Ready
    QCOMPARE(tier_state_from(Tier::Forecasting, ready, true, true, false), TierState::Ready);
    // chronos true but Ai not ready -> NotInstalled
    QCOMPARE(tier_state_from(Tier::Forecasting, mk(true, false, false), false, true, false), TierState::NotInstalled);
    // installing overrides
    QCOMPARE(tier_state_from(Tier::Forecasting, ready, true, true, true), TierState::Installing);
}

QTEST_APPLESS_MAIN(TestFeatureTier)
#include "tst_feature_tier.moc"
