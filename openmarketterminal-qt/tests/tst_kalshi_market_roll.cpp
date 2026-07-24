#include <QtTest>

#include "screens/kalshi/MarketRollPresentation.h"

using namespace openmarketterminal::screens::kalshi;
namespace pred = openmarketterminal::services::prediction;

namespace {

constexpr qint64 kNow = 1'753'400'000'000;  // fixed clock; no wall time in tests

QString iso(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODate);
}

pred::PredictionMarket market(const QString& ticker, qint64 close_ms,
                              const QString& series = QStringLiteral("KXBTCD"),
                              bool closed = false) {
    pred::PredictionMarket m;
    m.key.exchange_id = QStringLiteral("kalshi");
    m.key.market_id = ticker;
    m.end_date_iso = close_ms > 0 ? iso(close_ms) : QString();
    m.active = !closed;
    m.closed = closed;
    m.extras.insert(QStringLiteral("series_ticker"), series);
    return m;
}

MarketRefreshState visible_state() {
    MarketRefreshState state;
    state.visible = true;
    return state;
}

} // namespace

class KalshiMarketRollTest final : public QObject {
    Q_OBJECT
  private slots:
    // ── decide_market_refresh ────────────────────────────────────────────────

    void hidden_screen_never_refreshes() {
        MarketRefreshState state;  // visible = false
        state.last_fetch_ms = kNow - 10 * kMarketRefreshIntervalMs;
        const auto decision = decide_market_refresh(state, kNow);
        QVERIFY(!decision.refresh);
        QCOMPARE(decision.reason, QStringLiteral("hidden"));
    }

    void periodic_refresh_fires_once_the_interval_elapses() {
        MarketRefreshState state = visible_state();
        state.last_fetch_ms = kNow - kMarketRefreshIntervalMs + 1;
        auto decision = decide_market_refresh(state, kNow);
        QVERIFY(!decision.refresh);
        QCOMPARE(decision.reason, QStringLiteral("waiting"));

        state.last_fetch_ms = kNow - kMarketRefreshIntervalMs;
        decision = decide_market_refresh(state, kNow);
        QVERIFY(decision.refresh);
        QCOMPARE(decision.reason, QStringLiteral("periodic"));
    }

    void an_in_flight_fetch_blocks_the_timer() {
        MarketRefreshState state = visible_state();
        state.last_fetch_ms = kNow - 10 * kMarketRefreshIntervalMs;
        state.list_fetch_in_flight = true;
        state.fetch_started_ms = kNow - 1'000;
        const auto decision = decide_market_refresh(state, kNow);
        QVERIFY(!decision.refresh);
        QCOMPARE(decision.reason, QStringLiteral("in-flight"));
    }

    void an_expired_selection_also_cannot_stack_a_second_fetch() {
        // The expiry path is the one that would otherwise hammer: the
        // condition stays true until the successor is listed.
        MarketRefreshState state = visible_state();
        state.has_selection = true;
        state.selected_end_date_iso = iso(kNow - 60'000);
        state.list_fetch_in_flight = true;
        state.fetch_started_ms = kNow - 1'000;
        // Long past both floors, so only the in-flight guard can hold it.
        state.last_fetch_ms = kNow - 10 * kMarketRefreshIntervalMs;
        const auto decision = decide_market_refresh(state, kNow);
        QVERIFY(!decision.refresh);
        QCOMPARE(decision.reason, QStringLiteral("in-flight"));
    }

    void a_lost_fetch_releases_the_guard_instead_of_wedging_it() {
        MarketRefreshState state = visible_state();
        state.last_fetch_ms = kNow - kMarketRefreshStallMs;
        state.list_fetch_in_flight = true;
        state.fetch_started_ms = kNow - kMarketRefreshStallMs;
        const auto decision = decide_market_refresh(state, kNow);
        QVERIFY(decision.refresh);
        QCOMPARE(decision.reason, QStringLiteral("periodic"));
    }

    void an_expired_selection_refreshes_ahead_of_the_periodic_cadence() {
        MarketRefreshState state = visible_state();
        state.has_selection = true;
        state.selected_end_date_iso = iso(kNow - 10'000);
        state.last_fetch_ms = kNow - kMarketExpiredRetryMs;
        const auto decision = decide_market_refresh(state, kNow);
        QVERIFY(decision.refresh);
        QCOMPARE(decision.reason, QStringLiteral("expired"));
    }

    void the_expiry_path_retries_on_its_own_floor_not_every_tick() {
        MarketRefreshState state = visible_state();
        state.has_selection = true;
        state.selected_end_date_iso = iso(kNow - 60'000);
        state.last_fetch_ms = kNow - 1'000;  // fetched a second ago, no successor yet
        const auto decision = decide_market_refresh(state, kNow);
        QVERIFY(!decision.refresh);
        QCOMPARE(decision.reason, QStringLiteral("waiting"));
    }

    void a_live_selection_does_not_take_the_expiry_path() {
        MarketRefreshState state = visible_state();
        state.has_selection = true;
        state.selected_end_date_iso = iso(kNow + 60'000);
        state.last_fetch_ms = kNow - kMarketExpiredRetryMs;
        QVERIFY(!decide_market_refresh(state, kNow).refresh);
    }

    void a_settled_selection_is_expired_whatever_its_clock_says() {
        MarketRefreshState state = visible_state();
        state.has_selection = true;
        state.selected_end_date_iso = iso(kNow + 60'000);
        state.selected_settled = true;
        state.last_fetch_ms = kNow - kMarketExpiredRetryMs;
        QCOMPARE(decide_market_refresh(state, kNow).reason, QStringLiteral("expired"));
    }

    void a_contract_without_a_stated_close_is_never_called_expired() {
        MarketRefreshState state = visible_state();
        state.has_selection = true;
        state.selected_end_date_iso = QString();
        state.last_fetch_ms = kNow - kMarketExpiredRetryMs;
        QVERIFY(!decide_market_refresh(state, kNow).refresh);

        state.selected_end_date_iso = QStringLiteral("not-a-timestamp");
        QVERIFY(!decide_market_refresh(state, kNow).refresh);
    }

    // ── is_selected_contract (the screen's teardown-skip predicate) ──────────

    void the_selected_contract_predicate_matches_by_ticker_only() {
        const QVector<pred::PredictionMarket> markets{market("KXBTCD-A", kNow + 600'000),
                                                      market("KXBTCD-B", kNow + 600'000)};
        QVERIFY(is_selected_contract(markets, 1, QStringLiteral("KXBTCD-B"), true));
        QVERIFY(!is_selected_contract(markets, 0, QStringLiteral("KXBTCD-B"), true));
        // Nothing selected yet, or a row that is not in the list: never a match,
        // so select_market always runs its full setup for those.
        QVERIFY(!is_selected_contract(markets, 1, QStringLiteral("KXBTCD-B"), false));
        QVERIFY(!is_selected_contract(markets, 1, QString(), true));
        QVERIFY(!is_selected_contract(markets, -1, QStringLiteral("KXBTCD-B"), true));
        QVERIFY(!is_selected_contract(markets, 2, QStringLiteral("KXBTCD-B"), true));
    }

    // ── choose_market_row ────────────────────────────────────────────────────

    void an_empty_list_selects_nothing() {
        const auto decision = choose_market_row({}, QStringLiteral("KXBTCD-A"), iso(kNow), false, kNow);
        QCOMPARE(decision.row, -1);
        QVERIFY(!decision.rolled);
        QCOMPARE(decision.reason, QStringLiteral("empty"));
    }

    void with_nothing_selected_the_top_of_the_list_wins() {
        const QVector<pred::PredictionMarket> markets{market("KXBTCD-A", kNow + 600'000),
                                                      market("KXBTCD-B", kNow + 600'000)};
        const auto decision = choose_market_row(markets, QString(), QString(), false, kNow);
        QCOMPARE(decision.row, 0);
        QCOMPARE(decision.reason, QStringLiteral("no-selection"));
        QVERIFY(!decision.rolled);
    }

    void a_background_refresh_keeps_the_live_selection_even_when_it_moved_rows() {
        // The list re-sorts on every fetch; preservation is by ticker, not row.
        const QVector<pred::PredictionMarket> markets{market("KXBTCD-A", kNow + 600'000),
                                                      market("KXBTCD-B", kNow + 600'000),
                                                      market("KXBTCD-C", kNow + 600'000)};
        const auto decision =
            choose_market_row(markets, QStringLiteral("KXBTCD-C"), iso(kNow + 600'000), false, kNow);
        QCOMPARE(decision.row, 2);
        QVERIFY(decision.same_contract);
        QVERIFY(!decision.rolled);
        QCOMPARE(decision.reason, QStringLiteral("preserved"));
        QCOMPARE(decision.to_ticker, QStringLiteral("KXBTCD-C"));
    }

    void an_expired_selection_rolls_to_the_soonest_successor_in_its_series() {
        const QVector<pred::PredictionMarket> markets{
            market("KXBTCD-1215", kNow - 5'000),           // the one that just died
            market("KXETHD-1230", kNow + 900'000, "KXETHD"),  // wrong series
            market("KXBTCD-1245", kNow + 1'800'000),          // further out
            market("KXBTCD-1230", kNow + 900'000),            // the successor
        };
        const auto decision =
            choose_market_row(markets, QStringLiteral("KXBTCD-1215"), iso(kNow - 5'000), false, kNow);
        QCOMPARE(decision.row, 3);
        QCOMPARE(decision.to_ticker, QStringLiteral("KXBTCD-1230"));
        QCOMPARE(decision.from_ticker, QStringLiteral("KXBTCD-1215"));
        QVERIFY(decision.rolled);
        QVERIFY(!decision.same_contract);
        QCOMPARE(decision.reason, QStringLiteral("rolled"));
    }

    void a_roll_ties_are_broken_by_the_lists_own_ranking() {
        // A whole successor ladder shares one close time; the screen's sort
        // already put the most actionable contract first, so that one wins.
        const QVector<pred::PredictionMarket> markets{market("KXBTCD-1215", kNow - 5'000),
                                                      market("KXBTCD-1230-T1", kNow + 900'000),
                                                      market("KXBTCD-1230-T2", kNow + 900'000)};
        const auto decision =
            choose_market_row(markets, QStringLiteral("KXBTCD-1215"), iso(kNow - 5'000), false, kNow);
        QCOMPARE(decision.to_ticker, QStringLiteral("KXBTCD-1230-T1"));
        QVERIFY(decision.rolled);
    }

    void the_successor_search_skips_contracts_that_are_already_dead() {
        // The list keeps expired contracts around for a few minutes. Rolling
        // onto one of those would hand the operator a second silent pane.
        const QVector<pred::PredictionMarket> markets{market("KXBTCD-1215", kNow - 5'000),
                                                      market("KXBTCD-1200", kNow - 300'000),
                                                      market("KXBTCD-1230", kNow + 900'000)};
        const auto decision =
            choose_market_row(markets, QStringLiteral("KXBTCD-1215"), iso(kNow - 5'000), false, kNow);
        QCOMPARE(decision.to_ticker, QStringLiteral("KXBTCD-1230"));
        QVERIFY(decision.rolled);
    }

    void a_settled_contract_rolls_even_before_its_close_time() {
        const QVector<pred::PredictionMarket> markets{
            market("KXBTCD-1215", kNow + 60'000, QStringLiteral("KXBTCD"), /*closed=*/true),
            market("KXBTCD-1230", kNow + 900'000)};
        const auto decision = choose_market_row(markets, QStringLiteral("KXBTCD-1215"),
                                                iso(kNow + 60'000), false, kNow);
        QVERIFY(decision.rolled);
        QCOMPARE(decision.to_ticker, QStringLiteral("KXBTCD-1230"));
    }

    void an_expired_contract_that_dropped_off_the_list_still_rolls() {
        const QVector<pred::PredictionMarket> markets{market("KXBTCD-1230", kNow + 900'000)};
        const auto decision = choose_market_row(markets, QStringLiteral("KXBTCD-1215"),
                                                iso(kNow - 300'000), false, kNow);
        QCOMPARE(decision.row, 0);
        QVERIFY(decision.rolled);
        QCOMPARE(decision.reason, QStringLiteral("rolled"));
    }

    void a_live_contract_missing_from_the_payload_is_not_a_roll() {
        // A search result set, or a narrowed horizon, is not a rollover — the
        // screen falls back to row 0 but journals nothing.
        const QVector<pred::PredictionMarket> markets{market("KXBTCD-1230", kNow + 900'000)};
        const auto decision = choose_market_row(markets, QStringLiteral("KXBTCD-OTHER"),
                                                iso(kNow + 600'000), false, kNow);
        QCOMPARE(decision.row, 0);
        QVERIFY(!decision.rolled);
        QCOMPARE(decision.reason, QStringLiteral("vanished"));
    }

    void an_expiry_with_no_successor_listed_is_not_claimed_as_a_roll() {
        const QVector<pred::PredictionMarket> markets{
            market("KXBTCD-1215", kNow - 5'000),
            market("KXETHD-1230", kNow + 900'000, QStringLiteral("KXETHD"))};
        const auto decision =
            choose_market_row(markets, QStringLiteral("KXBTCD-1215"), iso(kNow - 5'000), false, kNow);
        QCOMPARE(decision.row, 0);
        QVERIFY(!decision.rolled);
        QCOMPARE(decision.reason, QStringLiteral("expired-no-successor"));
    }

    void the_close_grace_keeps_a_roll_from_firing_early() {
        const QVector<pred::PredictionMarket> markets{market("KXBTCD-1215", kNow),
                                                      market("KXBTCD-1230", kNow + 900'000)};
        auto decision =
            choose_market_row(markets, QStringLiteral("KXBTCD-1215"), iso(kNow), false, kNow);
        QVERIFY(!decision.rolled);
        QCOMPARE(decision.reason, QStringLiteral("preserved"));

        decision = choose_market_row(markets, QStringLiteral("KXBTCD-1215"), iso(kNow), false,
                                     kNow + kMarketCloseGraceMs);
        QVERIFY(decision.rolled);
    }

    void the_refetched_close_time_overrides_what_the_screen_remembered() {
        // Kalshi extends a contract: the screen's stale copy says expired, the
        // fresh payload says live. The payload wins, so no phantom roll.
        const QVector<pred::PredictionMarket> markets{market("KXBTCD-1215", kNow + 600'000),
                                                      market("KXBTCD-1230", kNow + 900'000)};
        const auto decision = choose_market_row(markets, QStringLiteral("KXBTCD-1215"),
                                                iso(kNow - 600'000), false, kNow);
        QVERIFY(!decision.rolled);
        QVERIFY(decision.same_contract);
    }

    void a_series_without_extras_falls_back_to_the_ticker_prefix() {
        QVector<pred::PredictionMarket> markets{market("KXBTCD-1215", kNow - 5'000),
                                                market("KXBTCD-1230", kNow + 900'000),
                                                market("KXETHD-1230", kNow + 900'000)};
        for (auto& m : markets) m.extras.remove(QStringLiteral("series_ticker"));
        const auto decision =
            choose_market_row(markets, QStringLiteral("KXBTCD-1215"), iso(kNow - 5'000), false, kNow);
        QVERIFY(decision.rolled);
        QCOMPARE(decision.to_ticker, QStringLiteral("KXBTCD-1230"));
    }
};

QTEST_MAIN(KalshiMarketRollTest)
#include "tst_kalshi_market_roll.moc"
