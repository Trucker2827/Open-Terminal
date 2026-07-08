// Coverage for the pure paper/live source classifier
// (services/portfolio/PortfolioAccountMode). Paper (fake-money) broker accounts
// must be identifiable so the "All Accounts" real-money aggregate can exclude
// them; crypto exchanges are always live.

#include "services/portfolio/PortfolioAccountMode.h"

#include <QtTest>

using namespace openmarketterminal::portfolio;

class TestPortfolioAccountMode : public QObject {
    Q_OBJECT
  private slots:
    void brokerPaperIsPaper();
    void brokerLiveIsNotPaper();
    void cryptoIsNeverPaper();
    void caseInsensitiveMode();
};

// A broker account whose trading_mode is "paper" is paper.
void TestPortfolioAccountMode::brokerPaperIsPaper() {
    const auto mode = [](const QString& id) {
        return id == "acct1" ? QStringLiteral("paper") : QStringLiteral("live");
    };
    QVERIFY(sync_source_is_paper("broker:acct1", mode));
}

void TestPortfolioAccountMode::brokerLiveIsNotPaper() {
    const auto mode = [](const QString&) { return QStringLiteral("live"); };
    QVERIFY(!sync_source_is_paper("broker:acctLive", mode));
}

// Crypto exchanges have no paper mode — never paper, and the lookup is not even
// consulted (a throwing lookup must not be called).
void TestPortfolioAccountMode::cryptoIsNeverPaper() {
    bool called = false;
    const auto mode = [&](const QString&) {
        called = true;
        return QStringLiteral("paper");
    };
    QVERIFY(!sync_source_is_paper("crypto:coinbase", mode));
    QVERIFY2(!called, "broker mode lookup must not run for a crypto source");
}

void TestPortfolioAccountMode::caseInsensitiveMode() {
    const auto mode = [](const QString&) { return QStringLiteral("PAPER"); };
    QVERIFY(sync_source_is_paper("broker:x", mode));
}

QTEST_APPLESS_MAIN(TestPortfolioAccountMode)
#include "tst_portfolio_account_mode.moc"
