#include <QtTest>

#include "screens/kalshi/AutoCockpitPresentation.h"

using namespace openmarketterminal::screens::kalshi;

namespace {

constexpr qint64 kNow = 1'800'000'000'000;

/// A fully live cockpit: a fresh contract list and a fully quoted ladder.
/// Every test below starts from this and breaks exactly one thing, so a
/// failure names the input it is about.
AutoCockpitInputs live_inputs() {
    AutoCockpitInputs inputs;
    inputs.has_selection = true;
    inputs.event_ticker = QStringLiteral("KXBTCD-26JUL2419");
    inputs.markets_total = 636;
    inputs.markets_listed_at_ms = kNow - 12'000;
    inputs.legs_total = 14;
    inputs.legs_with_book = 14;
    inputs.newest_leg_quote_ms = kNow - 2'000;
    inputs.oldest_leg_quote_ms = kNow - 9'000;
    inputs.legs_quote_past_bound = 0;
    return inputs;
}

} // namespace

class KalshiAutoCockpitTest final : public QObject {
    Q_OBJECT
  private slots:
    // ── The live baseline ───────────────────────────────────────────────────

    void fresh_inputs_read_live_and_the_ladder_is_trustworthy() {
        const auto view = present_auto_cockpit(live_inputs(), kNow);
        QCOMPARE(view.state, QStringLiteral("live"));
        QCOMPARE(view.color_role, QStringLiteral("green"));
        QVERIFY(view.ladder_trustworthy);
        QVERIFY(view.ladder_notice.isEmpty());
        QVERIFY(view.headline.contains(QStringLiteral("AUTO COCKPIT LIVE")));
        QVERIFY(view.headline.contains(QStringLiteral("KXBTCD-26JUL2419")));
    }

    void the_live_header_states_both_input_ages() {
        const auto view = present_auto_cockpit(live_inputs(), kNow);
        QVERIFY(view.markets_line.contains(QStringLiteral("MARKETS LIVE")));
        QVERIFY(view.markets_line.contains(QStringLiteral("636 listed")));
        QVERIFY(view.markets_line.contains(QStringLiteral("12s ago")));
        QVERIFY(view.books_line.contains(QStringLiteral("BOOKS LIVE")));
        QVERIFY(view.books_line.contains(QStringLiteral("14/14 legs quoted")));
        QVERIFY(view.books_line.contains(QStringLiteral("newest 2s ago")));
        QVERIFY(view.books_line.contains(QStringLiteral("oldest 9s ago")));
    }

    // ── Market-list staleness (issue #137's dead path, made visible) ────────

    void a_market_list_past_the_bound_reads_stale_amber() {
        auto inputs = live_inputs();
        inputs.markets_listed_at_ms = kNow - (kCockpitMarketsStaleMs + 1'000);
        const auto view = present_auto_cockpit(inputs, kNow);
        QCOMPARE(view.state, QStringLiteral("stale"));
        QCOMPARE(view.color_role, QStringLiteral("amber"));
        QVERIFY(!view.ladder_trustworthy);
        QVERIFY(view.markets_line.contains(QStringLiteral("MARKETS STALE")));
        QVERIFY(view.headline.contains(QStringLiteral("AUTO COCKPIT STALE")));
        QVERIFY(view.headline.contains(QStringLiteral("the contract list is")));
    }

    /// The boundary straddles kCockpitMarketsStaleMs, so a threshold that
    /// never trips turns this red.
    void the_market_staleness_boundary_is_the_stated_constant() {
        auto fresh = live_inputs();
        fresh.markets_listed_at_ms = kNow - kCockpitMarketsStaleMs;
        QCOMPARE(present_auto_cockpit(fresh, kNow).state, QStringLiteral("live"));

        auto stale = live_inputs();
        stale.markets_listed_at_ms = kNow - (kCockpitMarketsStaleMs + 1);
        QCOMPARE(present_auto_cockpit(stale, kNow).state, QStringLiteral("stale"));
    }

    void a_market_list_stamped_in_the_future_is_stale_not_fresh() {
        auto inputs = live_inputs();
        inputs.markets_listed_at_ms = kNow + 60'000;
        const auto view = present_auto_cockpit(inputs, kNow);
        QCOMPARE(view.state, QStringLiteral("stale"));
        QVERIFY(!view.ladder_trustworthy);
    }

    // ── Book-feed staleness ─────────────────────────────────────────────────

    void a_dead_book_feed_reads_stale_amber() {
        auto inputs = live_inputs();
        inputs.newest_leg_quote_ms = kNow - (kCockpitBooksStaleMs + 5'000);
        inputs.oldest_leg_quote_ms = kNow - 120'000;
        inputs.legs_quote_past_bound = 14;
        const auto view = present_auto_cockpit(inputs, kNow);
        QCOMPARE(view.state, QStringLiteral("stale"));
        QCOMPARE(view.color_role, QStringLiteral("amber"));
        QVERIFY(!view.ladder_trustworthy);
        QVERIFY(view.books_line.contains(QStringLiteral("BOOKS STALE")));
        QVERIFY(view.headline.contains(QStringLiteral("the newest leg book is")));
    }

    void the_book_staleness_boundary_is_the_stated_constant() {
        auto fresh = live_inputs();
        fresh.newest_leg_quote_ms = kNow - kCockpitBooksStaleMs;
        fresh.oldest_leg_quote_ms = fresh.newest_leg_quote_ms;
        QCOMPARE(present_auto_cockpit(fresh, kNow).state, QStringLiteral("live"));

        auto stale = live_inputs();
        stale.newest_leg_quote_ms = kNow - (kCockpitBooksStaleMs + 1);
        stale.oldest_leg_quote_ms = stale.newest_leg_quote_ms;
        QCOMPARE(present_auto_cockpit(stale, kNow).state, QStringLiteral("stale"));
    }

    /// A far-out-of-the-money leg legitimately goes minutes without a book
    /// event. That is a number to report, not grounds to condemn a feed that
    /// is demonstrably still delivering.
    void one_quiet_leg_does_not_condemn_a_delivering_feed() {
        auto inputs = live_inputs();
        inputs.oldest_leg_quote_ms = kNow - 300'000;
        inputs.legs_quote_past_bound = 1;
        const auto view = present_auto_cockpit(inputs, kNow);
        QCOMPARE(view.state, QStringLiteral("live"));
        QVERIFY(view.ladder_trustworthy);
        QVERIFY(view.books_line.contains(QStringLiteral("oldest 5m ago")));
        QVERIFY(view.books_line.contains(QStringLiteral("1 quoted longer than 30s ago")));
    }

    // ── Absent inputs (grey), which outrank stale ───────────────────────────

    void no_selection_reads_absent_grey() {
        AutoCockpitInputs inputs;
        inputs.has_selection = false;
        inputs.markets_total = 636;
        inputs.markets_listed_at_ms = kNow - 1'000;
        const auto view = present_auto_cockpit(inputs, kNow);
        QCOMPARE(view.state, QStringLiteral("absent"));
        QCOMPARE(view.color_role, QStringLiteral("grey"));
        QVERIFY(!view.ladder_trustworthy);
        QVERIFY(view.headline.contains(QStringLiteral("no contract is selected")));
        QVERIFY(view.books_line.contains(QStringLiteral("no contract selected")));
    }

    void a_contract_list_that_never_arrived_reads_absent_not_stale() {
        auto inputs = live_inputs();
        inputs.markets_listed_at_ms = 0;
        inputs.markets_total = 0;
        const auto view = present_auto_cockpit(inputs, kNow);
        QCOMPARE(view.state, QStringLiteral("absent"));
        QCOMPARE(view.color_role, QStringLiteral("grey"));
        QVERIFY(view.markets_line.contains(QStringLiteral("MARKETS NOT LISTED YET")));
        QVERIFY(view.headline.contains(QStringLiteral("no contract list has arrived")));
    }

    void an_engine_that_produced_no_legs_reads_absent() {
        auto inputs = live_inputs();
        inputs.legs_total = 0;
        inputs.legs_with_book = 0;
        inputs.newest_leg_quote_ms = 0;
        inputs.oldest_leg_quote_ms = 0;
        const auto view = present_auto_cockpit(inputs, kNow);
        QCOMPARE(view.state, QStringLiteral("absent"));
        QVERIFY(view.books_line.contains(QStringLiteral("no ladder legs")));
        QVERIFY(view.headline.contains(QStringLiteral("the engine produced no ladder legs")));
    }

    // ── The silent cached-snapshot fallback (KalshiAutoEngine::quote_for) ────

    void a_ladder_with_no_books_at_all_says_so_instead_of_looking_priced() {
        auto inputs = live_inputs();
        inputs.legs_with_book = 0;
        inputs.newest_leg_quote_ms = 0;
        inputs.oldest_leg_quote_ms = 0;
        const auto view = present_auto_cockpit(inputs, kNow);
        QCOMPARE(view.state, QStringLiteral("absent"));
        QVERIFY(!view.ladder_trustworthy);
        QVERIFY(view.books_line.contains(QStringLiteral("BOOKS ABSENT")));
        QVERIFY(view.books_line.contains(QStringLiteral("0/14 legs quoted")));
        QVERIFY(view.books_line.contains(QStringLiteral("cached market snapshot")));
        QVERIFY(view.headline.contains(QStringLiteral("no leg has a live book")));
    }

    void legs_priced_without_a_book_are_counted_on_the_header() {
        auto inputs = live_inputs();
        inputs.legs_with_book = 9;
        const auto view = present_auto_cockpit(inputs, kNow);
        QCOMPARE(view.state, QStringLiteral("live"));
        QVERIFY(view.books_line.contains(QStringLiteral("9/14 legs quoted")));
        QVERIFY(view.books_line.contains(
            QStringLiteral("5 priced from the cached market snapshot (no book)")));
    }

    void a_fully_quoted_ladder_makes_no_cached_snapshot_claim() {
        const auto view = present_auto_cockpit(live_inputs(), kNow);
        QVERIFY(!view.books_line.contains(QStringLiteral("cached market snapshot")));
        QVERIFY(!view.books_line.contains(QStringLiteral("quoted longer than")));
    }

    // ── The ladder is withheld, never left showing last-good prices ─────────

    void every_not_live_state_withholds_the_ladder_with_a_stated_reason() {
        auto stale_markets = live_inputs();
        stale_markets.markets_listed_at_ms = kNow - (kCockpitMarketsStaleMs + 1'000);
        auto stale_books = live_inputs();
        stale_books.newest_leg_quote_ms = kNow - (kCockpitBooksStaleMs + 1'000);
        AutoCockpitInputs unselected;

        for (const auto& inputs : {stale_markets, stale_books, unselected}) {
            const auto view = present_auto_cockpit(inputs, kNow);
            QVERIFY(!view.ladder_trustworthy);
            QVERIFY2(view.ladder_notice.startsWith(QStringLiteral("LADDER NOT SHOWN")),
                     qPrintable(view.ladder_notice));
            // The notice must name a cause, never just assert a state.
            QVERIFY2(view.ladder_notice.size() >
                         QStringLiteral("LADDER NOT SHOWN — .").size() + 10,
                     qPrintable(view.ladder_notice));
        }
    }

    void both_stale_inputs_are_named_together() {
        auto inputs = live_inputs();
        inputs.markets_listed_at_ms = kNow - (kCockpitMarketsStaleMs + 1'000);
        inputs.newest_leg_quote_ms = kNow - (kCockpitBooksStaleMs + 1'000);
        const auto view = present_auto_cockpit(inputs, kNow);
        QVERIFY(view.headline.contains(QStringLiteral("the contract list is")));
        QVERIFY(view.headline.contains(QStringLiteral("the newest leg book is")));
        QVERIFY(view.ladder_notice.contains(QStringLiteral("the contract list is")));
        QVERIFY(view.ladder_notice.contains(QStringLiteral("the newest leg book is")));
    }

    /// The header is rendered against `now`, not against the last time the
    /// engine ran: the same inputs a minute later must read older, and cross
    /// into stale on their own. This is what makes a frozen cockpit impossible.
    void the_same_inputs_age_on_their_own() {
        const auto inputs = live_inputs();
        const auto early = present_auto_cockpit(inputs, kNow);
        const auto later = present_auto_cockpit(inputs, kNow + kCockpitMarketsStaleMs);
        QCOMPARE(early.state, QStringLiteral("live"));
        QCOMPARE(later.state, QStringLiteral("stale"));
        QVERIFY(early.markets_line != later.markets_line);
        QVERIFY(later.markets_line.contains(QStringLiteral("MARKETS STALE")));
    }

    // ── Colour-role mapping and the role statements ─────────────────────────

    void the_colour_role_mapping_is_green_amber_grey() {
        QCOMPARE(auto_cockpit_state_color_role(QStringLiteral("live")), QStringLiteral("green"));
        QCOMPARE(auto_cockpit_state_color_role(QStringLiteral("stale")), QStringLiteral("amber"));
        QCOMPARE(auto_cockpit_state_color_role(QStringLiteral("absent")), QStringLiteral("grey"));
        QCOMPARE(auto_cockpit_state_color_role(QStringLiteral("nonsense")), QStringLiteral("grey"));
    }

    void the_three_surfaces_have_distinct_one_line_roles() {
        const QStringList roles{auto_cockpit_role(), bot_surface_role(),
                                advisor_canary_surface_role()};
        for (const QString& role : roles) {
            QVERIFY2(!role.isEmpty(), qPrintable(role));
            QVERIFY2(!role.contains(QLatin1Char('\n')), qPrintable(role));
        }
        QVERIFY(roles.at(0).startsWith(QStringLiteral("AUTO COCKPIT")));
        QVERIFY(roles.at(0).contains(QStringLiteral("research ladder")));
        QVERIFY(roles.at(0).contains(QStringLiteral("bounded manual execution")));
        QVERIFY(roles.at(1).startsWith(QStringLiteral("BOT")));
        QVERIFY(roles.at(1).contains(QStringLiteral("gate")));
        QVERIFY(roles.at(2).startsWith(QStringLiteral("ADVISOR & CANARY")));
        QVERIFY(roles.at(2).contains(QStringLiteral("archived")));
        QCOMPARE(QSet<QString>(roles.cbegin(), roles.cend()).size(), 3);
    }

    void age_text_scales_from_seconds_to_hours() {
        QCOMPARE(auto_cockpit_age_text(2'000), QStringLiteral("2s ago"));
        QCOMPARE(auto_cockpit_age_text(180'000), QStringLiteral("3m ago"));
        QCOMPARE(auto_cockpit_age_text(4LL * 3'600'000), QStringLiteral("4h ago"));
        QCOMPARE(auto_cockpit_age_text(-5'000), QStringLiteral("clock skew"));
    }
};

QTEST_MAIN(KalshiAutoCockpitTest)
#include "tst_kalshi_auto_cockpit.moc"
