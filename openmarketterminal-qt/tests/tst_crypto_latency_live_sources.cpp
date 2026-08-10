// The "2 live sources" bar must mean two sources you could actually trade on.
//
// The old rule counted a source live purely because its last tick was recent.
// A venue emitting a bare last-price every few seconds — no book, or a book
// with no size — was indistinguishable from one streaming a real two-sided
// quote, so the scalp gate's "2 live sources" precondition passed on data
// nobody could execute against.

#include <QtTest/QtTest>

#include "services/crypto_latency/CryptoLatencyService.h"

using namespace openmarketterminal;
using openmarketterminal::services::crypto_latency::CryptoLatencySourceState;
using openmarketterminal::services::crypto_latency::CryptoLatencyTick;

namespace {

constexpr qint64 kNow = 1'700'000'000'000;

CryptoLatencySourceState state(const QString& source, qint64 last_tick_ms) {
    CryptoLatencySourceState s;
    s.source = source;
    s.last_tick_ms = last_tick_ms;
    return s;
}

CryptoLatencyTick quote(const QString& source, double bid, double ask,
                        double bid_size, double ask_size) {
    CryptoLatencyTick t;
    t.source = source;
    t.price = (bid + ask) / 2.0;
    t.best_bid = bid;
    t.best_ask = ask;
    t.bid_size = bid_size;
    t.ask_size = ask_size;
    t.received_ts_ms = kNow - 100;
    return t;
}

int count(const QVector<CryptoLatencySourceState>& s, const QVector<CryptoLatencyTick>& t) {
    return openmarketterminal::services::crypto_latency::crypto_latency_count_live_sources(s, t, kNow);
}

}  // namespace

class CryptoLatencyLiveSourcesTest : public QObject {
    Q_OBJECT

private slots:
    void two_book_quoting_sources_count() {
        const QVector<CryptoLatencySourceState> s{state("coinbase", kNow - 200),
                                                  state("kraken", kNow - 400)};
        const QVector<CryptoLatencyTick> t{quote("coinbase", 100.0, 100.2, 5.0, 4.0),
                                           quote("kraken", 99.9, 100.3, 2.0, 3.0)};
        QCOMPARE(count(s, t), 2);
    }

    // ---- the bug: thin feeds that used to pass ------------------------
    void a_source_with_no_book_does_not_count() {
        // Ticks a last-price, never quotes. Recent, and useless to trade on.
        const QVector<CryptoLatencySourceState> s{state("coinbase", kNow - 200),
                                                  state("gemini", kNow - 200)};
        const QVector<CryptoLatencyTick> t{quote("coinbase", 100.0, 100.2, 5.0, 4.0),
                                           quote("gemini", 0.0, 0.0, 0.0, 0.0)};
        QCOMPARE(count(s, t), 1);
    }

    void a_one_sided_book_does_not_count() {
        const QVector<CryptoLatencySourceState> s{state("coinbase", kNow - 200),
                                                  state("kraken", kNow - 200)};
        const QVector<CryptoLatencyTick> t{quote("coinbase", 100.0, 100.2, 5.0, 4.0),
                                           quote("kraken", 99.9, 0.0, 2.0, 0.0)};
        QCOMPARE(count(s, t), 1);
    }

    void a_book_quoted_with_no_size_does_not_count() {
        // Prices on both sides but nothing behind them — the shape a stale or
        // synthetic quote takes. top_book_sources already rejects this.
        const QVector<CryptoLatencySourceState> s{state("coinbase", kNow - 200),
                                                  state("kraken", kNow - 200)};
        const QVector<CryptoLatencyTick> t{quote("coinbase", 100.0, 100.2, 5.0, 4.0),
                                           quote("kraken", 99.9, 100.3, 0.0, 0.0)};
        QCOMPARE(count(s, t), 1);
    }

    void a_source_with_no_tick_at_all_does_not_count() {
        const QVector<CryptoLatencySourceState> s{state("coinbase", kNow - 200),
                                                  state("kraken", kNow - 200)};
        const QVector<CryptoLatencyTick> t{quote("coinbase", 100.0, 100.2, 5.0, 4.0)};
        QCOMPARE(count(s, t), 1);
    }

    // ---- freshness is still required ----------------------------------
    void a_stale_source_does_not_count_even_with_a_good_book() {
        const QVector<CryptoLatencySourceState> s{state("coinbase", kNow - 200),
                                                  state("kraken", kNow - 60'000)};
        const QVector<CryptoLatencyTick> t{quote("coinbase", 100.0, 100.2, 5.0, 4.0),
                                           quote("kraken", 99.9, 100.3, 2.0, 3.0)};
        QCOMPARE(count(s, t), 1);
    }

    void a_source_that_never_ticked_does_not_count() {
        const QVector<CryptoLatencySourceState> s{state("coinbase", 0)};
        const QVector<CryptoLatencyTick> t{quote("coinbase", 100.0, 100.2, 5.0, 4.0)};
        QCOMPARE(count(s, t), 0);
    }

    void a_tick_stamped_in_the_future_does_not_count() {
        const QVector<CryptoLatencySourceState> s{state("coinbase", kNow + 10'000)};
        const QVector<CryptoLatencyTick> t{quote("coinbase", 100.0, 100.2, 5.0, 4.0)};
        QCOMPARE(count(s, t), 0);
    }

    // ---- shape / safety ------------------------------------------------
    void source_names_match_case_insensitively() {
        const QVector<CryptoLatencySourceState> s{state("Coinbase", kNow - 200)};
        const QVector<CryptoLatencyTick> t{quote("COINBASE", 100.0, 100.2, 5.0, 4.0)};
        QCOMPARE(count(s, t), 1);
    }

    void empty_inputs_are_zero_not_a_crash() {
        QCOMPARE(count({}, {}), 0);
        QCOMPARE(count({state("coinbase", kNow - 200)}, {}), 0);
    }

    void the_rule_can_only_tighten() {
        // Whatever the inputs, the new count never exceeds the old
        // recency-only count. This gate may only ever become harder to pass.
        const QVector<CryptoLatencySourceState> s{state("a", kNow - 100), state("b", kNow - 100),
                                                  state("c", kNow - 100)};
        const QVector<CryptoLatencyTick> t{quote("a", 1.0, 1.1, 1.0, 1.0),
                                           quote("b", 0.0, 0.0, 0.0, 0.0)};
        int recency_only = 0;
        for (const auto& st : s)
            if (st.last_tick_ms > 0 && kNow >= st.last_tick_ms && kNow - st.last_tick_ms <= 5000)
                ++recency_only;
        QCOMPARE(recency_only, 3);
        QVERIFY(count(s, t) <= recency_only);
        QCOMPARE(count(s, t), 1);
    }
};

QTEST_MAIN(CryptoLatencyLiveSourcesTest)
#include "tst_crypto_latency_live_sources.moc"
