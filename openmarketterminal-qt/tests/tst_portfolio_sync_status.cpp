// Coverage for the pure account-sync status text builder
// (screens/portfolio/PortfolioSyncStatus). The wording must (a) clearly show the
// "no accounts connected" empty state instead of a misleading "synced" message,
// (b) say "accounts" so it never implies the current manual portfolio was
// synced, and (c) report how many accounts + how long ago.

#include "screens/portfolio/PortfolioSyncStatus.h"

#include <QtTest>

using namespace openmarketterminal::portfolio;

class TestPortfolioSyncStatus : public QObject {
    Q_OBJECT
  private slots:
    void noAccountsConnected();
    void accountsButNeverSynced();
    void countAndRelativeTime();
    void pluralization();
};

// The empty state the user actually hit: zero connected accounts must NOT read
// as "Synced just now".
void TestPortfolioSyncStatus::noAccountsConnected() {
    const QDateTime now = QDateTime::fromString("2026-07-08T04:40:00Z", Qt::ISODate);
    QCOMPARE(sync_status_text(0, now, now), QString("No accounts connected"));
    QCOMPARE(sync_status_text(0, QDateTime(), now), QString("No accounts connected"));
}

void TestPortfolioSyncStatus::accountsButNeverSynced() {
    const QDateTime now = QDateTime::fromString("2026-07-08T04:40:00Z", Qt::ISODate);
    QCOMPARE(sync_status_text(2, QDateTime(), now), QString("Accounts not yet synced"));
}

void TestPortfolioSyncStatus::countAndRelativeTime() {
    const QDateTime now = QDateTime::fromString("2026-07-08T04:40:00Z", Qt::ISODate);
    // just now (< 60s)
    QCOMPARE(sync_status_text(2, now.addSecs(-30), now), QString("2 accounts synced just now"));
    // minutes
    QCOMPARE(sync_status_text(2, now.addSecs(-5 * 60), now), QString("2 accounts synced 5m ago"));
    // hours
    QCOMPARE(sync_status_text(3, now.addSecs(-2 * 3600), now), QString("3 accounts synced 2h ago"));
    // days
    QCOMPARE(sync_status_text(3, now.addSecs(-3 * 86400), now), QString("3 accounts synced 3d ago"));
}

void TestPortfolioSyncStatus::pluralization() {
    const QDateTime now = QDateTime::fromString("2026-07-08T04:40:00Z", Qt::ISODate);
    QCOMPARE(sync_status_text(1, now.addSecs(-10), now), QString("1 account synced just now"));
    QCOMPARE(sync_status_text(2, now.addSecs(-10), now), QString("2 accounts synced just now"));
}

QTEST_APPLESS_MAIN(TestPortfolioSyncStatus)
#include "tst_portfolio_sync_status.moc"
