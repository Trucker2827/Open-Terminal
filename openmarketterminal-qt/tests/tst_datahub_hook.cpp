#include <QtTest>
#include <QObject>
#include "datahub/DataHub.h"
using namespace openmarketterminal::datahub;
class TstDataHubHook : public QObject {
    Q_OBJECT
private slots:
    void default_hook_treats_all_owners_active() {
        DataHub::set_owner_active_hook(nullptr); // reset to default
        QObject o;
        QVERIFY(DataHub::owner_active_for_test(&o)); // default → true
    }
    void installed_hook_is_honored() {
        DataHub::set_owner_active_hook([](QObject*){ return false; });
        QObject o;
        QVERIFY(!DataHub::owner_active_for_test(&o));
        DataHub::set_owner_active_hook(nullptr);
    }
};
QTEST_MAIN(TstDataHubHook)
#include "tst_datahub_hook.moc"
