// tst_pm_paper.cpp — storage + constitution readers for the prediction-market
// paper book (Phase B, Task 1). Verifies the v050 migration creates the
// pm_paper_account / pm_paper_positions tables, that PmPaperRepository
// round-trips cash + positions, and that the SettingsGate venue helpers read
// cli.allowed_venues correctly.
//
// Bring-up mirrors tst_order_flow.cpp: a HeadlessRuntime init("default")
// registers all migrations (incl. v050) and opens the DB under a QTemporaryDir
// HOME before any repo call.

#include <QtTest>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QTemporaryDir>

#include "core/headless/HeadlessRuntime.h"
#include "mcp/McpTypes.h"
#include "mcp/tools/PmPaperEngine.h"
#include "mcp/tools/SettingsGate.h"
#include "services/prediction/PredictionExchangeAdapter.h"
#include "services/prediction/PredictionExchangeRegistry.h"
#include "services/prediction/PredictionTypes.h"
#include "storage/repositories/PmPaperRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/sqlite/Database.h"

#include <memory>

using namespace openmarketterminal;
using namespace openmarketterminal::headless;

namespace pred = openmarketterminal::services::prediction;

// ── Fake prediction adapter (Phase B, Task 2 bridge seam) ────────────────────
//
// Stands in for the real Polymarket adapter so the PM read tools can be tested
// without network. Emits its fixtures via QueuedConnection so the *_ready signal
// fires on the event loop WHILE run_async_wait blocks the worker — exercising
// the real async→sync bridge. fetch_order_book deliberately emits a WRONG asset
// id FIRST, then the requested one, to prove the bridge's id-correlation ignores
// the broadcast mismatch (the bug this task exists to prevent).
class FakePredictionAdapter : public pred::PredictionExchangeAdapter {
    Q_OBJECT
  public:
    static pred::PredictionMarket fixture_market(const QString& market_id) {
        pred::PredictionMarket m;
        m.key.exchange_id = "polymarket";
        m.key.market_id = market_id;
        m.question = "Will it rain tomorrow?";
        m.category = "weather";
        m.end_date_iso = "2026-12-31T00:00:00Z";
        m.liquidity = 1234.5;
        m.volume = 6789.0;
        m.outcomes = {{"Yes", "asset-yes", 0.62}, {"No", "asset-no", 0.38}};
        return m;
    }
    static pred::PredictionOrderBook fixture_book(const QString& asset_id) {
        pred::PredictionOrderBook b;
        b.asset_id = asset_id;
        b.tick_size = 0.01;
        b.bids = {{0.40, 100.0}, {0.39, 50.0}};   // highest first
        b.asks = {{0.45, 80.0}, {0.46, 60.0}};    // lowest first
        return b;
    }

    QString id() const override { return "polymarket"; }
    QString display_name() const override { return "Fake Polymarket"; }
    pred::ExchangeCapabilities capabilities() const override { return {}; }

    void list_markets(const QString&, const QString&, int, int) override {
        QMetaObject::invokeMethod(
            this, [this] { emit markets_ready({fixture_market("mkt-1")}); }, Qt::QueuedConnection);
    }
    void list_events(const QString&, const QString&, int, int) override {}
    void search(const QString&, int) override {
        QMetaObject::invokeMethod(
            this,
            [this] { emit search_results_ready({fixture_market("mkt-1")}, {}); },
            Qt::QueuedConnection);
    }
    void list_tags() override {}
    void fetch_market(const pred::MarketKey& key) override {
        const QString mid = key.market_id;
        // Emit a detail for a DIFFERENT market first → must be ignored.
        QMetaObject::invokeMethod(
            this, [this] { emit market_detail_ready(fixture_market("other-market")); },
            Qt::QueuedConnection);
        QMetaObject::invokeMethod(
            this, [this, mid] { emit market_detail_ready(fixture_market(mid)); },
            Qt::QueuedConnection);
    }
    void fetch_event(const pred::MarketKey&) override {}
    void fetch_order_book(const QString& asset_id) override {
        // Broadcast for the WRONG asset first, then the requested one.
        QMetaObject::invokeMethod(
            this, [this] { emit order_book_ready(fixture_book("WRONG-ASSET")); },
            Qt::QueuedConnection);
        QMetaObject::invokeMethod(
            this, [this, asset_id] { emit order_book_ready(fixture_book(asset_id)); },
            Qt::QueuedConnection);
    }
    void fetch_price_history(const QString&, const QString&, int) override {}
    void fetch_recent_trades(const pred::MarketKey&, int) override {}

    void subscribe_market(const QStringList&) override {}
    void unsubscribe_market(const QStringList&) override {}
    bool is_ws_connected() const override { return false; }

    bool has_credentials() const override { return false; }
    QString account_label() const override { return {}; }
    void fetch_balance() override {}
    void fetch_positions() override {}
    void fetch_open_orders() override {}
    void fetch_user_activity(int) override {}
    void place_order(const pred::OrderRequest&) override {}
    void cancel_order(const QString&) override {}
    void cancel_all_for_market(const pred::MarketKey&, const QString&) override {}

    void ensure_registered_with_hub() override {}
};

class TstPmPaper : public QObject {
    Q_OBJECT
  private:
    QTemporaryDir home_;
    HeadlessRuntime rt_;

  private slots:
    void initTestCase() {
        QVERIFY(home_.isValid());
        qputenv("HOME", home_.path().toUtf8());

        // Register the fake adapter for id "polymarket" BEFORE rt_.init().
        // The registry is a process-global singleton and the headless init guard
        // (`if(!reg.adapter("polymarket"))`) means whoever registers first wins —
        // so the fake must go in first to displace the real network adapter.
        pred::PredictionExchangeRegistry::instance().register_adapter(
            std::make_unique<FakePredictionAdapter>());

        auto r = rt_.init("default");
        QVERIFY2(r.ok, qPrintable(r.error));
    }

    // The v050 migration must have created both PM paper tables.
    void tables_exist() {
        for (const QString& tbl : {QStringLiteral("pm_paper_account"), QStringLiteral("pm_paper_positions")}) {
            auto r = Database::instance().execute(
                "SELECT name FROM sqlite_master WHERE type='table' AND name=?", {tbl});
            QVERIFY2(r.is_ok(), qPrintable("sqlite_master query failed for " + tbl));
            QVERIFY2(r.value().next(), qPrintable("table missing: " + tbl));
        }
    }

    // cash() seeds the account to 100000 on first read; adjust_cash(-500) debits.
    void cash_seeds_and_adjusts() {
        auto& repo = PmPaperRepository::instance();

        auto c = repo.cash();
        QVERIFY2(c.is_ok(), qPrintable("cash() failed: " + QString::fromStdString(c.is_err() ? c.error() : "")));
        QCOMPARE(c.value(), 100000.0);

        auto adj = repo.adjust_cash(-500.0);
        QVERIFY2(adj.is_ok(), "adjust_cash failed");

        auto c2 = repo.cash();
        QVERIFY2(c2.is_ok(), "cash() after adjust failed");
        QCOMPARE(c2.value(), 99500.0);
    }

    // insert_open → get_open round-trips every field; open_stake_in_category sums
    // cost_basis across all OPEN rows in a category.
    void position_round_trip_and_category_stake() {
        auto& repo = PmPaperRepository::instance();

        PmPosition p;
        p.venue = "polymarket";
        p.market_id = "mkt-1";
        p.asset_id = "asset-yes-1";
        p.outcome = "YES";
        p.category = "elections";
        p.contracts = 100;
        p.avg_price = 0.42;
        p.cost_basis = 42.0;
        p.opened_at = "2026-06-15T10:00:00Z";
        p.status = "open";

        auto ins = repo.insert_open(p);
        QVERIFY2(ins.is_ok(), qPrintable("insert_open failed: " + QString::fromStdString(ins.is_err() ? ins.error() : "")));
        QVERIFY2(ins.value() > 0, "insert_open should return a positive row id");

        auto got = repo.get_open(p.venue, p.asset_id);
        QVERIFY2(got.is_ok(), "get_open failed");
        QVERIFY2(got.value().has_value(), "get_open should find the just-inserted open position");
        const PmPosition& g = got.value().value();
        QCOMPARE(g.id, ins.value());
        QCOMPARE(g.venue, p.venue);
        QCOMPARE(g.market_id, p.market_id);
        QCOMPARE(g.asset_id, p.asset_id);
        QCOMPARE(g.outcome, p.outcome);
        QCOMPARE(g.category, p.category);
        QCOMPARE(g.contracts, p.contracts);
        QCOMPARE(g.avg_price, p.avg_price);
        QCOMPARE(g.cost_basis, p.cost_basis);
        QCOMPARE(g.opened_at, p.opened_at);
        QCOMPARE(g.status, p.status);

        // get_open on an unknown asset returns nullopt (not an error).
        auto none = repo.get_open(p.venue, "no-such-asset");
        QVERIFY2(none.is_ok(), "get_open(unknown) should be ok");
        QVERIFY2(!none.value().has_value(), "get_open(unknown) should be nullopt");

        // A second OPEN position in the same category: stake sums both cost_basis.
        PmPosition p2 = p;
        p2.asset_id = "asset-no-1";
        p2.outcome = "NO";
        p2.cost_basis = 58.0;
        auto ins2 = repo.insert_open(p2);
        QVERIFY2(ins2.is_ok(), "second insert_open failed");

        auto stake = repo.open_stake_in_category("elections");
        QVERIFY2(stake.is_ok(), "open_stake_in_category failed");
        QCOMPARE(stake.value(), 100.0); // 42 + 58

        // A category with no open rows sums to 0.
        auto empty = repo.open_stake_in_category("sports");
        QVERIFY2(empty.is_ok(), "open_stake_in_category(empty) failed");
        QCOMPARE(empty.value(), 0.0);
    }

    // The venue constitution reader: default-deny, then allow after the human
    // (modelled by a direct repo write) names venues in cli.allowed_venues.
    void venue_gate_default_deny_then_allow() {
        // Default (unset) — no venue is allowed.
        QVERIFY2(!mcp::cli_venue_allowed("polymarket"), "polymarket must be denied by default");

        auto set = SettingsRepository::instance().set("cli.allowed_venues", "polymarket,kalshi", "cli");
        QVERIFY2(set.is_ok(), "set cli.allowed_venues failed");

        // Case-insensitive + whitespace-tolerant membership.
        QVERIFY2(mcp::cli_venue_allowed("polymarket"), "polymarket must be allowed");
        QVERIFY2(mcp::cli_venue_allowed("KALSHI"), "KALSHI (case-insensitive) must be allowed");
        QVERIFY2(mcp::cli_venue_allowed(" Polymarket "), "' Polymarket ' (trimmed) must be allowed");
        QVERIFY2(!mcp::cli_venue_allowed("foo"), "foo must be denied");

        const QStringList venues = mcp::cli_allowed_venues();
        QCOMPARE(venues.size(), 2);
        QVERIFY2(venues.contains("polymarket") && venues.contains("kalshi"),
                 "allowed venues must be normalised to lowercase");
    }

    // ── PM read tools over the async→sync bridge (Task 2) ─────────────────

    // pm_get_order_book correlates the broadcast order_book_ready by asset_id:
    // the fake emits a WRONG asset first, then the requested one — the tool must
    // return the requested asset's book with best_bid/best_ask/spread computed.
    void pm_get_order_book_correlates_and_computes() {
        auto res = rt_.call_tool(
            "pm_get_order_book",
            QJsonObject{{"venue", "polymarket"}, {"asset_id", "asset-yes"}});
        QVERIFY2(res.success, qPrintable("pm_get_order_book failed: " + res.error));

        const QJsonObject book = res.data.toObject();
        QCOMPARE(book.value("asset_id").toString(), QStringLiteral("asset-yes"));
        QCOMPARE(book.value("best_bid").toDouble(), 0.40);
        QCOMPARE(book.value("best_ask").toDouble(), 0.45);
        QVERIFY(qFuzzyCompare(book.value("spread").toDouble(), 0.05));
        QCOMPARE(book.value("bids").toArray().size(), 2);
        QCOMPARE(book.value("asks").toArray().size(), 2);
    }

    // pm_get_market correlates market_detail_ready by market_id (ignores the
    // broadcast detail for "other-market" the fake emits first).
    void pm_get_market_returns_fixture() {
        auto res = rt_.call_tool(
            "pm_get_market",
            QJsonObject{{"venue", "polymarket"}, {"market_id", "mkt-42"}});
        QVERIFY2(res.success, qPrintable("pm_get_market failed: " + res.error));

        const QJsonObject m = res.data.toObject();
        QCOMPARE(m.value("market_id").toString(), QStringLiteral("mkt-42"));
        QCOMPARE(m.value("question").toString(), QStringLiteral("Will it rain tomorrow?"));
        QCOMPARE(m.value("category").toString(), QStringLiteral("weather"));
        QCOMPARE(m.value("liquidity").toDouble(), 1234.5);
        QCOMPARE(m.value("outcomes").toArray().size(), 2);
        QCOMPARE(m.value("outcomes").toArray().at(0).toObject().value("asset_id").toString(),
                 QStringLiteral("asset-yes"));
    }

    // pm_search_markets accepts the first search_results_ready emission.
    void pm_search_markets_returns_fixture() {
        auto res = rt_.call_tool(
            "pm_search_markets",
            QJsonObject{{"venue", "polymarket"}, {"query", "rain"}});
        QVERIFY2(res.success, qPrintable("pm_search_markets failed: " + res.error));

        const QJsonArray markets = res.data.toObject().value("markets").toArray();
        QCOMPARE(markets.size(), 1);
        QCOMPARE(markets.at(0).toObject().value("market_id").toString(), QStringLiteral("mkt-1"));
    }

    // Unknown venue fails clean (no hang) without ever touching an adapter.
    void pm_unknown_venue_fails_clean() {
        auto res = rt_.call_tool(
            "pm_get_order_book",
            QJsonObject{{"venue", "nope"}, {"asset_id", "x"}});
        QVERIFY2(!res.success, "unknown venue must fail");
        QVERIFY(res.error.contains("venue", Qt::CaseInsensitive));
    }

    // ── Paper fill engine (Task 3) ────────────────────────────────────────
    //
    // One ordered slot walks a full position lifecycle so cash stays
    // deterministic across the open → average → partial-sell → close → re-buy
    // sequence, then exercises every rejection path. The engine operates
    // directly on PmPaperRepository (no adapter needed): pure DB writes.
    void engine_buy_sell_lifecycle() {
        using openmarketterminal::mcp::tools::buy_to_open;
        using openmarketterminal::mcp::tools::mark_to_market;
        using openmarketterminal::mcp::tools::sell_to_close;
        auto& repo = PmPaperRepository::instance();

        // Reset cash to a known 100000 baseline (prior slots debited it).
        auto c0 = repo.cash();
        QVERIFY2(c0.is_ok(), "cash() baseline read failed");
        QVERIFY2(repo.adjust_cash(100000.0 - c0.value()).is_ok(), "cash reset failed");
        QVERIFY(qFuzzyCompare(repo.cash().value(), 100000.0));

        const QString venue = "polymarket", mkt = "mkt-eng", asset = "eng-asset",
                      outcome = "YES", cat = "crypto";

        // buy_to_open(100 @ 0.60) → cost 60, cash 99940, 100 @ 0.60.
        auto f1 = buy_to_open(venue, mkt, asset, outcome, cat, 100, 0.60);
        QVERIFY2(f1.ok, qPrintable(f1.reason));
        QCOMPARE(f1.action, QStringLiteral("buy_to_open"));
        QVERIFY(qFuzzyCompare(f1.cash_after, 99940.0));
        QVERIFY(qFuzzyCompare(repo.cash().value(), 99940.0));
        auto g1 = repo.get_open(venue, asset);
        QVERIFY(g1.value().has_value());
        QCOMPARE(g1.value()->contracts, 100.0);
        QVERIFY(qFuzzyCompare(g1.value()->avg_price, 0.60));
        QVERIFY(qFuzzyCompare(g1.value()->cost_basis, 60.0));

        // buy_to_open same asset (50 @ 0.80) → contracts 150, cost 100,
        // avg = cost_basis/contracts = 100/150 ≈ 0.6667, cash 99900.
        auto f2 = buy_to_open(venue, mkt, asset, outcome, cat, 50, 0.80);
        QVERIFY2(f2.ok, qPrintable(f2.reason));
        QVERIFY(qFuzzyCompare(repo.cash().value(), 99900.0));
        auto g2 = repo.get_open(venue, asset);
        QVERIFY(g2.value().has_value());
        QCOMPARE(g2.value()->contracts, 150.0);
        QVERIFY(qFuzzyCompare(g2.value()->cost_basis, 100.0));
        QVERIFY(qFuzzyCompare(g2.value()->cost_basis / g2.value()->contracts, 100.0 / 150.0));

        // sell_to_close(60 @ 0.70) → contracts 90, proceeds 42 → cash 99942,
        // cost_basis pro-rata = 100 * (90/150) = 60.
        auto s1 = sell_to_close(venue, asset, 60, 0.70);
        QVERIFY2(s1.ok, qPrintable(s1.reason));
        QCOMPARE(s1.action, QStringLiteral("sell_to_close"));
        QVERIFY(qFuzzyCompare(repo.cash().value(), 99942.0));
        auto g3 = repo.get_open(venue, asset);
        QVERIFY(g3.value().has_value());
        QCOMPARE(g3.value()->contracts, 90.0);
        QVERIFY(qFuzzyCompare(g3.value()->cost_basis, 60.0));
        QCOMPARE(g3.value()->status, QStringLiteral("open"));

        // sell_to_close(90 @ 0.50) → contracts 0, status "closed",
        // proceeds 45 → cash 99987. get_open now finds nothing (closed).
        auto s2 = sell_to_close(venue, asset, 90, 0.50);
        QVERIFY2(s2.ok, qPrintable(s2.reason));
        QVERIFY(qFuzzyCompare(repo.cash().value(), 99987.0));
        auto g4 = repo.get_open(venue, asset);
        QVERIFY2(!g4.value().has_value(), "position must be closed (no open row)");

        // close → re-buy SAME asset (50 @ 0.55): succeeds and yields a FRESH
        // open row (proves no UNIQUE conflict + one-open invariant across a
        // lifecycle). cost 27.5 → cash 99959.5.
        auto f3 = buy_to_open(venue, mkt, asset, outcome, cat, 50, 0.55);
        QVERIFY2(f3.ok, qPrintable(f3.reason));
        QVERIFY(qFuzzyCompare(repo.cash().value(), 99959.5));
        auto g5 = repo.get_open(venue, asset);
        QVERIFY2(g5.value().has_value(), "re-buy must reopen a fresh OPEN row");
        QCOMPARE(g5.value()->contracts, 50.0);
        QVERIFY(qFuzzyCompare(g5.value()->avg_price, 0.55));
        QCOMPARE(g5.value()->status, QStringLiteral("open"));
        QVERIFY2(g5.value()->id != g1.value()->id, "re-buy must be a NEW row, not the closed one");

        // mark_to_market on the 50 @ 0.55 position at 0.65 → (0.65-0.55)*50 = 5.0.
        const double pnl = mark_to_market(g5.value().value(), 0.65);
        QVERIFY(qFuzzyCompare(pnl, 5.0));

        // sell_to_close(1000 @ 0.50) on the 50-held position → rejected, no mutation.
        const double cash_before_overflow = repo.cash().value();
        auto s_over = sell_to_close(venue, asset, 1000, 0.50);
        QVERIFY2(!s_over.ok, "selling more than held must be rejected");
        QVERIFY2(s_over.reason.contains("cannot sell more than held"),
                 qPrintable("unexpected reason: " + s_over.reason));
        QVERIFY(qFuzzyCompare(repo.cash().value(), cash_before_overflow));
        QCOMPARE(repo.get_open(venue, asset).value()->contracts, 50.0);

        // sell_to_close on an UNHELD asset → rejected (short-open disabled).
        auto s_short = sell_to_close(venue, "never-held-asset", 10, 0.50);
        QVERIFY2(!s_short.ok, "selling an unheld asset must be rejected");
        QVERIFY2(s_short.reason.contains("short-open is not enabled"),
                 qPrintable("unexpected reason: " + s_short.reason));

        // buy_to_open with cost exceeding cash → rejected, cash unchanged.
        const double cash_before_broke = repo.cash().value();
        auto f_broke = buy_to_open(venue, mkt, "expensive-asset", outcome, cat, 1e9, 1.0);
        QVERIFY2(!f_broke.ok, "a buy exceeding cash must be rejected");
        QVERIFY2(f_broke.reason.contains("insufficient paper cash"),
                 qPrintable("unexpected reason: " + f_broke.reason));
        QVERIFY(qFuzzyCompare(repo.cash().value(), cash_before_broke));
        QVERIFY2(!repo.get_open(venue, "expensive-asset").value().has_value(),
                 "a rejected buy must not create a position");
    }

    void cleanupTestCase() { rt_.shutdown(); }
};

QTEST_MAIN(TstPmPaper)
#include "tst_pm_paper.moc"
