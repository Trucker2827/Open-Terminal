// Issue #83 — relmap::merge_peer_data: the pure mapping from the native
// EquityResearchService ratios feed (PeerData) to the relationship map's
// PeerCompany entries. Contract under test:
//   1. The subject company's own row (fetch_peers prepends it) is dropped.
//   2. Ratio fields carried by the feed are mapped through verbatim.
//   3. Fields the feed does NOT carry stay at zero/empty defaults —
//      honestly missing, never backfilled.
//   4. Blank symbols are dropped.
#include <QtTest>

#include "services/relationship_map/RelmapPeerMerge.h"

using openmarketterminal::relmap::merge_peer_data;
using openmarketterminal::services::equity::PeerData;

class TstRelmapPeerMerge : public QObject {
    Q_OBJECT
  private slots:
    void self_row_is_dropped() {
        PeerData self;
        self.symbol = "aapl"; // case-insensitive match
        self.pe_ratio = 30.0;
        PeerData peer;
        peer.symbol = "MSFT";
        peer.pe_ratio = 35.0;

        const auto out = merge_peer_data("AAPL", {self, peer});
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].ticker, QString("MSFT"));
    }

    void ratio_fields_map_verbatim() {
        PeerData p;
        p.symbol = "msft";
        p.pe_ratio = 35.1;
        p.forward_pe = 28.2;
        p.price_to_book = 11.5;
        p.roe = 0.38;
        p.profit_margin = 0.36;
        p.gross_margin = 0.69;

        const auto out = merge_peer_data("AAPL", {p});
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].ticker, QString("MSFT")); // uppercased
        QCOMPARE(out[0].pe_ratio, 35.1);
        QCOMPARE(out[0].forward_pe, 28.2);
        QCOMPARE(out[0].price_to_book, 11.5);
        QCOMPARE(out[0].roe, 0.38);
        QCOMPARE(out[0].profit_margins, 0.36);
        QCOMPARE(out[0].gross_margins, 0.69);
    }

    void uncarried_fields_stay_missing() {
        PeerData p;
        p.symbol = "MSFT";
        p.pe_ratio = 35.0;

        const auto out = merge_peer_data("AAPL", {p});
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].market_cap, 0.0);
        QCOMPARE(out[0].current_price, 0.0);
        QCOMPARE(out[0].beta, 0.0);
        QCOMPARE(out[0].ev_to_ebitda, 0.0);
        QCOMPARE(out[0].week52_change, 0.0);
        QCOMPARE(out[0].revenue_growth, 0.0);
        QVERIFY(out[0].name.isEmpty());
        QVERIFY(out[0].sector.isEmpty());
        QVERIFY(out[0].recommendation.isEmpty());
    }

    void blank_symbols_are_dropped() {
        PeerData blank;
        blank.symbol = "   ";
        const auto out = merge_peer_data("AAPL", {blank});
        QVERIFY(out.isEmpty());
    }

    void empty_input_yields_empty_output() {
        QVERIFY(merge_peer_data("AAPL", {}).isEmpty());
    }
};

QTEST_APPLESS_MAIN(TstRelmapPeerMerge)
#include "tst_relmap_peer_merge.moc"
