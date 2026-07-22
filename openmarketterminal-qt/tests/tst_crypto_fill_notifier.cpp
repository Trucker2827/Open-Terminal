#include "screens/crypto_trading/CryptoFillNotifier.h"
#include <QtTest/QtTest>

using namespace openmarketterminal::crypto;

namespace {
QJsonObject ord(const char* id, const char* status) {
    return QJsonObject{{"id", id},        {"symbol", "BTC/USD"}, {"side", "buy"},
                       {"status", status}, {"filled", 0.001},     {"average", 118000.0}};
}
} // namespace

class TstCryptoFillNotifier : public QObject {
    Q_OBJECT
  private slots:
    void notifies_terminal_once() {
        CryptoFillNotifier n;
        const QString first = n.on_order_event(ord("o1", "filled"));
        QVERIFY(!first.isEmpty());
        QVERIFY(first.contains("BTC/USD"));
        QVERIFY(first.contains("filled"));
        // Same (id, status) again — WS and the confirming REST refresh can
        // both report it; second must be suppressed.
        QVERIFY(n.on_order_event(ord("o1", "filled")).isEmpty());
    }
    void open_and_partial_are_silent() {
        CryptoFillNotifier n;
        QVERIFY(n.on_order_event(ord("o2", "open")).isEmpty());
        QVERIFY(n.on_order_event(ord("o2", "partially_filled")).isEmpty());
    }
    void distinct_ids_both_notify() {
        CryptoFillNotifier n;
        QVERIFY(!n.on_order_event(ord("a", "filled")).isEmpty());
        QVERIFY(!n.on_order_event(ord("b", "filled")).isEmpty());
    }
    void canceled_notifies_once() {
        CryptoFillNotifier n;
        QVERIFY(!n.on_order_event(ord("c", "canceled")).isEmpty());
        QVERIFY(n.on_order_event(ord("c", "canceled")).isEmpty());
    }
    void missing_id_never_notifies() {
        CryptoFillNotifier n;
        QJsonObject o = ord("", "filled");
        o.remove("id");
        QVERIFY(n.on_order_event(o).isEmpty());
    }
};

QTEST_MAIN(TstCryptoFillNotifier)
#include "tst_crypto_fill_notifier.moc"
