// tst_ibkr_conid.cpp — Symbol→conid resolution for the IBKR adapter.
//
// Unit-tests the PURE parser IBKRBroker::parse_conid_from_secdef(body, symbol),
// which extracts the stock (STK) contract's conid from a
// /iserver/secdef/search response. No gateway, no network — fixtures only.
//
// The resolver resolve_conid() is intentionally NOT tested here: it depends on
// the BrokerHttp singleton (unmockable). The logic that can be wrong lives in
// the pure parser, which is what we pin down.

#include <QtTest>

#include "trading/brokers/ibkr/IBKRBroker.h"

using namespace openmarketterminal::trading;

class TestIbkrConid : public QObject {
    Q_OBJECT

  private slots:
    // STK match: a normal AAPL response with a string conid.
    void stkMatch() {
        const QByteArray body = R"([
            { "conid": "265598", "symbol": "AAPL", "companyName": "APPLE INC",
              "description": "NASDAQ", "sections": [ {"secType":"STK"}, {"secType":"OPT"} ] }
        ])";
        QCOMPARE(IBKRBroker::parse_conid_from_secdef(body, "AAPL"), QString("265598"));
    }

    // Numeric conid: conid is a JSON number rather than a string.
    void numericConid() {
        const QByteArray body = R"([
            { "conid": 265598, "symbol": "AAPL", "companyName": "APPLE INC",
              "sections": [ {"secType":"STK"} ] }
        ])";
        QCOMPARE(IBKRBroker::parse_conid_from_secdef(body, "AAPL"), QString("265598"));
    }

    // Picks STK among multiple — NEUTER-SENSITIVE fixture.
    // element[0] shares the requested symbol "AAPL" but has NO STK section
    // (OPT/FUT only); the STK element comes SECOND with a different conid.
    // The only path to the correct answer is the STK preference, so breaking
    // STK selection must change this result.
    void picksStkAmongMultiple() {
        const QByteArray body = R"([
            { "conid": "111111", "symbol": "AAPL", "companyName": "APPLE OPTIONS",
              "sections": [ {"secType":"OPT"}, {"secType":"FUT"} ] },
            { "conid": "265598", "symbol": "AAPL", "companyName": "APPLE INC",
              "sections": [ {"secType":"STK"} ] }
        ])";
        QCOMPARE(IBKRBroker::parse_conid_from_secdef(body, "AAPL"), QString("265598"));
    }

    // Case-insensitive request: request "aapl" against symbol "AAPL".
    void caseInsensitive() {
        const QByteArray body = R"([
            { "conid": "265598", "symbol": "AAPL",
              "sections": [ {"secType":"STK"} ] }
        ])";
        QCOMPARE(IBKRBroker::parse_conid_from_secdef(body, "aapl"), QString("265598"));
    }

    // No match: no element's symbol equals the request → "".
    void noMatch() {
        const QByteArray body = R"([
            { "conid": "265598", "symbol": "MSFT", "sections": [ {"secType":"STK"} ] },
            { "conid": "999999", "symbol": "GOOGL", "sections": [ {"secType":"STK"} ] }
        ])";
        QCOMPARE(IBKRBroker::parse_conid_from_secdef(body, "AAPL"), QString(""));
    }

    // Fallback: symbol matches but NO element has an STK section → first
    // symbol-match's conid is returned.
    void fallbackToFirstSymbolMatch() {
        const QByteArray body = R"([
            { "conid": "111111", "symbol": "AAPL", "sections": [ {"secType":"OPT"} ] },
            { "conid": "222222", "symbol": "AAPL", "sections": [ {"secType":"FUT"} ] }
        ])";
        QCOMPARE(IBKRBroker::parse_conid_from_secdef(body, "AAPL"), QString("111111"));
    }

    // Malformed / not array / empty → "" (no crash).
    void malformed() {
        QCOMPARE(IBKRBroker::parse_conid_from_secdef("{}", "AAPL"), QString(""));
        QCOMPARE(IBKRBroker::parse_conid_from_secdef("", "AAPL"), QString(""));
        QCOMPARE(IBKRBroker::parse_conid_from_secdef("garbage", "AAPL"), QString(""));
        QCOMPARE(IBKRBroker::parse_conid_from_secdef("[]", "AAPL"), QString(""));
    }
};

QTEST_MAIN(TestIbkrConid)
#include "tst_ibkr_conid.moc"
