// tst_headless_runtime.cpp — e2e bring-up of HeadlessRuntime under QCoreApplication.
//
// With a QTemporaryDir HOME (so DB/cache land in a throwaway datadir), init()
// must succeed and a real core tool must round-trip through the synchronous
// call_tool() path (which pumps a nested QEventLoop under the QTEST_MAIN app).
//
// Assertion tool: get_app_info — a purely-local "system" core tool. It returns
// version/platform/tool-count/python-availability with NO network and NO
// service dependencies, and it is NOT flagged destructive, so the deny-all gate
// presenter installed by init() never blocks it. That makes it a clean,
// deterministic probe of the full runtime path (no network flakiness).

#include <QtTest>
#include <QTemporaryDir>

#include "core/headless/HeadlessRuntime.h"

using namespace openmarketterminal::headless;

class TstHeadlessRuntime : public QObject {
    Q_OBJECT
  private slots:
    void init_brings_up_db_and_core_tools() {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        qputenv("HOME", home.path().toUtf8());

        HeadlessRuntime rt;
        auto r = rt.init("default");
        QVERIFY2(r.ok, qPrintable(r.error));

        // A pure-compute / no-network core tool round-trips.
        auto res = rt.call_tool("get_app_info", {});
        QVERIFY2(res.success, qPrintable(res.error));

        rt.shutdown();
    }
};

QTEST_MAIN(TstHeadlessRuntime)
#include "tst_headless_runtime.moc"
