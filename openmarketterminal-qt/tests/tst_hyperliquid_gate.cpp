// Safety interlock for the Hyperliquid live order path.
//
// HyperliquidSigner::ecdsa_sign() hardcodes the ECDSA recovery id to v=27
// (HyperliquidSigner.cpp — "TODO(hyperliquid): derive real recovery id"). That
// value is correct only ~half the time, so HL's ecrecover resolves the WRONG
// signer whenever the true recovery id is 28. It is harmless TODAY only because
// HyperliquidVenue::place_order() refuses to submit, returning
// status="rejected", error="hl_live_path_not_yet_wired".
//
// This test pins that gate shut. If someone wires the live submit path without
// first deriving the real recovery id (and covering it with HL's documented
// known-answer vector), this test fails and CI blocks the merge — forcing the
// signer fix before any order can reach the exchange with a bad signature.
//
// When the recovery id is correctly derived and verified, update this test to
// match the new (un-gated) behaviour — and read the warning above first.

#include "trading/exchanges/hyperliquid/HyperliquidVenue.h"
#include "services/alpha_arena/IExchangeVenue.h"

#include <QtTest>

using openmarketterminal::trading::hyperliquid::HyperliquidVenue;
using openmarketterminal::services::alpha_arena::OrderAck;
using openmarketterminal::services::alpha_arena::OrderRequest;

class TestHyperliquidGate : public QObject {
    Q_OBJECT
  private slots:
    void liveOrderPathStaysGated();
};

void TestHyperliquidGate::liveOrderPathStaysGated() {
    HyperliquidVenue venue;

    OrderRequest req;
    req.agent_id = QStringLiteral("test-agent");
    req.coin = QStringLiteral("ETH");
    req.side = QStringLiteral("buy");
    req.qty = 1.0;
    req.leverage = 1;

    bool acked = false;
    OrderAck got;
    venue.place_order(req, [&](OrderAck a) {
        acked = true;
        got = a;
    });

    // The gate must reject synchronously — the live submit path is not wired and
    // the signer's recovery id is not yet derived (see file header).
    QVERIFY2(acked, "place_order must invoke the ack callback");
    QCOMPARE(got.status, QStringLiteral("rejected"));
    QCOMPARE(got.error, QStringLiteral("hl_live_path_not_yet_wired"));
}

QTEST_GUILESS_MAIN(TestHyperliquidGate)
#include "tst_hyperliquid_gate.moc"
