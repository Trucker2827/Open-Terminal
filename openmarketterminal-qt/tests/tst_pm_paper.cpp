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
#include <QDateTime>
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
#include "storage/repositories/LivePnlRepository.h"
#include "storage/repositories/OrderDraftRepository.h"
#include "storage/repositories/PmPaperRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/repositories/TradeAuditRepository.h"
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
        // Risk-floor fixtures keyed by market_id (Task 4):
        if (market_id == "mkt-expo")
            m.category = "expo";                    // isolated topic for exposure test
        if (market_id == "mkt-illiquid")
            m.liquidity = 10.0;                     // < default pm_min_liquidity (1000)
        if (market_id == "mkt-near")
            m.end_date_iso =                        // < default pm_min_hours_to_resolution (1)
                QDateTime::currentDateTimeUtc().addSecs(30 * 60).toString(Qt::ISODate);
        return m;
    }
    static pred::PredictionOrderBook fixture_book(const QString& asset_id) {
        pred::PredictionOrderBook b;
        b.asset_id = asset_id;
        b.tick_size = 0.01;
        if (asset_id == "asset-wide") {
            // spread 0.20 > default pm_max_spread (0.10)
            b.bids = {{0.30, 100.0}, {0.29, 50.0}};   // highest first
            b.asks = {{0.50, 80.0}, {0.51, 60.0}};    // lowest first
        } else {
            b.bids = {{0.40, 100.0}, {0.39, 50.0}};   // highest first
            b.asks = {{0.45, 80.0}, {0.46, 60.0}};    // lowest first; spread 0.05
        }
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

    // ── Live-submit seam (Task 4) ────────────────────────────────────────────
    // creds_ defaults TRUE so the gated live path can reach place_order; the
    // no-credentials gate test flips it false and MUST restore (process-global
    // singleton shared across slots). last_req_ records the request the bridge
    // actually fired so the happy-path test can assert the RESOLVED price/size.
    bool creds_ = true;
    bool suppress_place_result_ = false;
    // Negative means "the venue filled the whole order". A real exchange also
    // answers OK for an order it merely ACCEPTED, or filled only in part, so
    // these let a test drive those acknowledgements. Restore after use.
    double filled_override_ = -1.0;
    QString status_override_;
    int place_order_calls_ = 0;
    pred::OrderRequest last_req_;

    bool has_credentials() const override { return creds_; }
    QString account_label() const override { return {}; }
    void fetch_balance() override {}
    void fetch_positions() override {}
    void fetch_open_orders() override {}
    void fetch_user_activity(int) override {}
    void place_order(const pred::OrderRequest& req) override {
        ++place_order_calls_;
        last_req_ = req;
        if (suppress_place_result_)
            return;
        // Async fill on the event loop while run_async_wait blocks the worker —
        // exercising the real correlated+timed bridge. A success OrderResult.
        QMetaObject::invokeMethod(
            this,
            [this, req] {
                pred::OrderResult r;
                r.ok = true;
                r.order_id = "PM-FAKE-1";
                r.filled = filled_override_ >= 0.0 ? filled_override_ : req.size;
                r.remaining = req.size - r.filled;
                r.status = status_override_.isEmpty() ? QStringLiteral("FILLED") : status_override_;
                emit order_placed(r);
            },
            Qt::QueuedConnection);
    }
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
        // The persisted avg_price is re-blended (not the first-entry 0.60), so
        // mark-to-market over the stored position is correct after averaging in.
        QVERIFY(qFuzzyCompare(g2.value()->avg_price, 100.0 / 150.0));
        QVERIFY(qFuzzyCompare(mark_to_market(*g2.value(), 0.70), (0.70 - 100.0 / 150.0) * 150.0));

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

    // ── Task 4: prepare_order prediction branch + deterministic PM risk floor ─

    void set_setting(const QString& key, const QString& value) {
        auto r = SettingsRepository::instance().set(key, value, "cli");
        QVERIFY2(r.is_ok(), qPrintable("set " + key + " failed"));
    }

    // A valid BUY prediction intent within caps is PREPARED: status "prepared"
    // with a draft_id; the draft row exists (account pm-paper); a prepare/prepared
    // audit row is recorded. stake = contracts * best_ask = 10 * 0.45 = 4.5.
    void pm_prepare_valid_buy_prepares_draft() {
        set_setting("cli.allowed_venues", "polymarket");
        auto res = rt_.call_tool(
            "prepare_order",
            QJsonObject{{"asset_class", "prediction"}, {"venue", "polymarket"},
                        {"market_id", "mkt-1"}, {"asset_id", "asset-yes"},
                        {"outcome", "Yes"}, {"side", "buy"}, {"contracts", 10},
                        {"autonomous", true}, {"max_orders_per_hour", 10},
                        {"automation_session_id", "session-test"},
                        {"automation_session_ends_at", "2026-12-30T00:00:00.000Z"}});
        QVERIFY2(res.success, qPrintable("pm prepare should succeed: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("prepared"));
        QCOMPARE(data.value("risk_status").toString(), QStringLiteral("passed"));
        QVERIFY(qFuzzyCompare(data.value("fill_price").toDouble(), 0.45));
        QVERIFY(qFuzzyCompare(data.value("stake").toDouble(), 4.5));
        QVERIFY(qFuzzyCompare(data.value("max_loss").toDouble(), 4.5));

        const QString draft_id = data.value("draft_id").toString();
        QVERIFY2(!draft_id.isEmpty(), "prepared result must carry a draft_id");

        auto got = OrderDraftRepository::instance().get(draft_id);
        QVERIFY2(got.is_ok(), qPrintable("draft row must exist for " + draft_id));
        QCOMPARE(got.value().status, QStringLiteral("prepared"));
        QCOMPARE(got.value().account, QStringLiteral("pm-paper"));
        QCOMPARE(got.value().mode_hint, QStringLiteral("paper"));
        const QJsonObject persisted_intent =
            QJsonDocument::fromJson(got.value().intent_json.toUtf8()).object();
        QVERIFY(persisted_intent.value(QStringLiteral("autonomous")).toBool());
        QCOMPARE(persisted_intent.value(QStringLiteral("max_orders_per_hour")).toInt(), 10);
        QCOMPARE(persisted_intent.value(QStringLiteral("automation_session_id")).toString(),
                 QStringLiteral("session-test"));
        QCOMPARE(persisted_intent.value(QStringLiteral("automation_session_ends_at")).toString(),
                 QStringLiteral("2026-12-30T00:00:00.000Z"));

        auto recent = TradeAuditRepository::instance().recent(50);
        QVERIFY2(recent.is_ok(), "audit recent() failed");
        bool found = false;
        for (const TradeAuditRow& r : recent.value())
            if (r.tool == "prepare_order" && r.phase == "prepare" && r.decision == "prepared") {
                found = true;
                break;
            }
        QVERIFY2(found, "expected a prepare/prepared audit row");
    }

    // Oversized stake (contracts * price > max_order_value default) is REJECTED.
    void pm_prepare_oversized_stake_rejected() {
        set_setting("cli.allowed_venues", "polymarket");
        // 60000 * 0.45 = 27000 > 25000 default max_order_value (checked first).
        auto res = rt_.call_tool(
            "prepare_order",
            QJsonObject{{"asset_class", "prediction"}, {"venue", "polymarket"},
                        {"market_id", "mkt-1"}, {"asset_id", "asset-yes"},
                        {"outcome", "Yes"}, {"side", "buy"}, {"contracts", 60000}});
        QVERIFY2(res.success, qPrintable("risk rejection is a decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("max order value", Qt::CaseInsensitive),
                 qPrintable("reason: " + data.value("reason").toString()));
        QVERIFY2(data.value("draft_id").toString().isEmpty(), "rejected must carry no draft_id");
    }

    // Over-exposure: pre-seed an open position near the per-topic cap, then a buy
    // that pushes category stake over the (small) cap → REJECTED (exposure).
    void pm_prepare_over_exposure_rejected() {
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.risk.max_exposure_per_topic", "50");

        PmPosition seed;
        seed.venue = "polymarket";
        seed.market_id = "mkt-expo";
        seed.asset_id = "expo-seed";
        seed.outcome = "Yes";
        seed.category = "expo"; // matches fixture_market("mkt-expo")
        seed.contracts = 100;
        seed.avg_price = 0.45;
        seed.cost_basis = 45.0;
        seed.opened_at = "2026-06-15T10:00:00Z";
        seed.status = "open";
        QVERIFY2(PmPaperRepository::instance().insert_open(seed).is_ok(), "seed insert failed");

        // stake = 20 * 0.45 = 9 → 45 + 9 = 54 > 50 cap.
        auto res = rt_.call_tool(
            "prepare_order",
            QJsonObject{{"asset_class", "prediction"}, {"venue", "polymarket"},
                        {"market_id", "mkt-expo"}, {"asset_id", "asset-yes"},
                        {"outcome", "Yes"}, {"side", "buy"}, {"contracts", 20}});
        set_setting("cli.risk.max_exposure_per_topic", "10000"); // restore
        QVERIFY2(res.success, qPrintable("decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("exposure", Qt::CaseInsensitive),
                 qPrintable("reason: " + data.value("reason").toString()));
    }

    // Illiquid market (liquidity below default pm_min_liquidity) is REJECTED.
    void pm_prepare_illiquid_rejected() {
        set_setting("cli.allowed_venues", "polymarket");
        auto res = rt_.call_tool(
            "prepare_order",
            QJsonObject{{"asset_class", "prediction"}, {"venue", "polymarket"},
                        {"market_id", "mkt-illiquid"}, {"asset_id", "asset-yes"},
                        {"outcome", "Yes"}, {"side", "buy"}, {"contracts", 10}});
        QVERIFY2(res.success, qPrintable("decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("illiquid", Qt::CaseInsensitive),
                 qPrintable("reason: " + data.value("reason").toString()));
    }

    // Wide-spread book (ask - bid > default pm_max_spread) is REJECTED.
    void pm_prepare_wide_spread_rejected() {
        set_setting("cli.allowed_venues", "polymarket");
        auto res = rt_.call_tool(
            "prepare_order",
            QJsonObject{{"asset_class", "prediction"}, {"venue", "polymarket"},
                        {"market_id", "mkt-1"}, {"asset_id", "asset-wide"},
                        {"outcome", "Yes"}, {"side", "buy"}, {"contracts", 10}});
        QVERIFY2(res.success, qPrintable("decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("spread", Qt::CaseInsensitive),
                 qPrintable("reason: " + data.value("reason").toString()));
    }

    // Near resolution (end_date within pm_min_hours_to_resolution) is REJECTED.
    void pm_prepare_near_resolution_rejected() {
        set_setting("cli.allowed_venues", "polymarket");
        auto res = rt_.call_tool(
            "prepare_order",
            QJsonObject{{"asset_class", "prediction"}, {"venue", "polymarket"},
                        {"market_id", "mkt-near"}, {"asset_id", "asset-yes"},
                        {"outcome", "Yes"}, {"side", "buy"}, {"contracts", 10}});
        QVERIFY2(res.success, qPrintable("decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("resolution", Qt::CaseInsensitive),
                 qPrintable("reason: " + data.value("reason").toString()));
    }

    // Venue not in cli.allowed_venues → REJECTED before any fetch.
    void pm_prepare_venue_not_allowed_rejected() {
        set_setting("cli.allowed_venues", ""); // clear allow-list
        auto res = rt_.call_tool(
            "prepare_order",
            QJsonObject{{"asset_class", "prediction"}, {"venue", "polymarket"},
                        {"market_id", "mkt-1"}, {"asset_id", "asset-yes"},
                        {"outcome", "Yes"}, {"side", "buy"}, {"contracts", 10}});
        set_setting("cli.allowed_venues", "polymarket"); // restore
        QVERIFY2(res.success, qPrintable("decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("venue", Qt::CaseInsensitive),
                 qPrintable("reason: " + data.value("reason").toString()));
    }

    // side=sell with no open position → REJECTED (short-open not enabled in Phase B).
    void pm_prepare_sell_without_position_rejected() {
        set_setting("cli.allowed_venues", "polymarket");
        auto res = rt_.call_tool(
            "prepare_order",
            QJsonObject{{"asset_class", "prediction"}, {"venue", "polymarket"},
                        {"market_id", "mkt-1"}, {"asset_id", "asset-never-held"},
                        {"outcome", "Yes"}, {"side", "sell"}, {"contracts", 10}});
        QVERIFY2(res.success, qPrintable("decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("no open position", Qt::CaseInsensitive),
                 qPrintable("reason: " + data.value("reason").toString()));
    }

    // ── Task 5: submit_order prediction branch (paper engine / live hard-off) ─

    // Helper: prepare a valid PM BUY and return its draft_id (caller sets gates).
    QString prepare_pm_buy(const QString& asset_id, double contracts,
                           const QString& market_id = QStringLiteral("mkt-1")) {
        auto res = rt_.call_tool(
            "prepare_order",
            QJsonObject{{"asset_class", "prediction"}, {"venue", "polymarket"},
                        {"market_id", market_id}, {"asset_id", asset_id},
                        {"outcome", "Yes"}, {"side", "buy"}, {"contracts", contracts}});
        if (!res.success)
            return {};
        const QJsonObject d = res.data.toObject();
        if (d.value("status").toString() != QLatin1String("prepared"))
            return {};
        return d.value("draft_id").toString();
    }

    bool find_submit_audit(const QString& decision, const QString& mode) {
        auto recent = TradeAuditRepository::instance().recent(50);
        if (!recent.is_ok())
            return false;
        for (const TradeAuditRow& r : recent.value())
            if (r.tool == "submit_order" && r.phase == "submit" && r.decision == decision &&
                r.mode == mode)
                return true;
        return false;
    }

    // PAPER happy (BUY): toggle on, prepare a valid BUY, submit paper → "filled",
    // a position row exists, cash debited, draft "submitted", a submit/paper audit.
    void pm_submit_paper_happy_buy() {
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.allow_paper_trading", "true");

        const QString draft_id = prepare_pm_buy("sub-buy", 10);
        QVERIFY2(!draft_id.isEmpty(), "prepare a valid BUY draft");

        const double cash_before = PmPaperRepository::instance().cash().value();

        auto res = rt_.call_tool("submit_order",
                                 QJsonObject{{"draft_id", draft_id}, {"mode", "paper"}});
        QVERIFY2(res.success, qPrintable("paper submit is a decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("filled"));
        QCOMPARE(data.value("mode").toString(), QStringLiteral("paper"));
        QCOMPARE(data.value("action").toString(), QStringLiteral("buy_to_open"));
        QVERIFY(qFuzzyCompare(data.value("contracts").toDouble(), 10.0));
        QVERIFY(qFuzzyCompare(data.value("fill_price").toDouble(), 0.45));  // best_ask

        auto open = PmPaperRepository::instance().get_open("polymarket", "sub-buy");
        QVERIFY2(open.value().has_value(), "a paper position must exist after the fill");
        QCOMPARE(open.value()->contracts, 10.0);

        QVERIFY2(qFuzzyCompare(PmPaperRepository::instance().cash().value(), cash_before - 4.5),
                 "cash debited by 10 * 0.45 = 4.5");

        auto got = OrderDraftRepository::instance().get(draft_id);
        QCOMPARE(got.value().status, QStringLiteral("submitted"));

        QVERIFY2(find_submit_audit("filled", "paper"), "expected a submit/paper filled audit row");
    }

    // PAPER toggle OFF after prepare → "rejected" (paper disabled), no position,
    // draft NOT "submitted".
    void pm_submit_paper_toggle_off_rejected() {
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.allow_paper_trading", "true");
        const QString draft_id = prepare_pm_buy("sub-toggle", 10);
        QVERIFY2(!draft_id.isEmpty(), "prepare a valid BUY draft");

        set_setting("cli.allow_paper_trading", "false");  // revoked after prepare
        auto res = rt_.call_tool("submit_order",
                                 QJsonObject{{"draft_id", draft_id}, {"mode", "paper"}});
        set_setting("cli.allow_paper_trading", "true");  // restore
        QVERIFY2(res.success, qPrintable("paper-disabled is a decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("paper trading disabled", Qt::CaseInsensitive),
                 qPrintable("reason: " + data.value("reason").toString()));

        QVERIFY2(!PmPaperRepository::instance().get_open("polymarket", "sub-toggle").value().has_value(),
                 "a rejected paper submit must not open a position");
        QVERIFY2(OrderDraftRepository::instance().get(draft_id).value().status !=
                     QLatin1String("submitted"),
                 "draft must not be marked submitted");
    }

    // Venue cleared after prepare → "rejected" (venue not allowed), no position.
    void pm_submit_venue_not_allowed_rejected() {
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.allow_paper_trading", "true");
        const QString draft_id = prepare_pm_buy("sub-venue", 10);
        QVERIFY2(!draft_id.isEmpty(), "prepare a valid BUY draft");

        set_setting("cli.allowed_venues", "");  // revoke venue after prepare
        auto res = rt_.call_tool("submit_order",
                                 QJsonObject{{"draft_id", draft_id}, {"mode", "paper"}});
        set_setting("cli.allowed_venues", "polymarket");  // restore
        QVERIFY2(res.success, qPrintable("venue-denied is a decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("venue", Qt::CaseInsensitive),
                 qPrintable("reason: " + data.value("reason").toString()));

        QVERIFY2(!PmPaperRepository::instance().get_open("polymarket", "sub-venue").value().has_value(),
                 "a venue-denied submit must not open a position");
    }

    // ── Task 4 (Phase C): submit_order mode "live" — gated adapter execution ──

    // Handle to the process-global fake adapter (registered in initTestCase).
    FakePredictionAdapter* fake_adapter() {
        return static_cast<FakePredictionAdapter*>(
            pred::PredictionExchangeRegistry::instance().adapter("polymarket"));
    }
    // No OPEN live ledger row for the PM (account ""/venue/asset) → adapter never
    // reached. The real-money safety property for every gate rejection.
    void verify_no_live_position(const QString& asset_id) {
        auto pos = LivePnlRepository::instance().get_open("", "polymarket", asset_id);
        QVERIFY2(pos.is_ok(), "live get_open must not error");
        QVERIFY2(!pos.value().has_value(),
                 qPrintable("a gated-out live submit must NOT record a live position: " + asset_id));
    }
    void arm_live() {
        set_setting("cli.allow_trading", "true");
        set_setting("cli.live_trading_armed", "true");
    }
    void disarm_live() {
        set_setting("cli.allow_trading", "false");
        set_setting("cli.live_trading_armed", "false");
    }

    // HAPPY PATH: armed + venue allowed + credentials → the adapter is fired
    // through the timed bridge and reports FILLED. The submit response is an
    // order-state acknowledgement, not a durable fill event, so P&L remains
    // unchanged until account reconciliation observes a uniquely identified fill.
    void pm_submit_live_happy_fills_via_adapter() {
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.allow_paper_trading", "true");
        const QString draft_id = prepare_pm_buy("live-happy", 10);
        QVERIFY2(!draft_id.isEmpty(), "prepare a valid BUY draft");

        QVERIFY2(fake_adapter(), "fake adapter must be registered");
        fake_adapter()->creds_ = true;
        fake_adapter()->suppress_place_result_ = false;

        arm_live();
        auto res = rt_.call_tool("submit_order",
                                 QJsonObject{{"draft_id", draft_id}, {"mode", "live"}});
        disarm_live();
        QVERIFY2(res.success, qPrintable("live submit is a decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("filled"));
        QCOMPARE(data.value("mode").toString(), QStringLiteral("live"));
        QCOMPARE(data.value("order_id").toString(), QStringLiteral("PM-FAKE-1"));
        QCOMPARE(data.value("venue").toString(), QStringLiteral("polymarket"));

        // The adapter received the request built at the RESOLVED price/size.
        const pred::OrderRequest& req = fake_adapter()->last_req_;
        QCOMPARE(req.asset_id, QStringLiteral("live-happy"));
        QCOMPARE(req.side, QStringLiteral("BUY"));
        QCOMPARE(req.size, 10.0);
        QVERIFY2(qFuzzyCompare(req.price, 0.45), "OrderRequest must carry the RESOLVED best_ask 0.45");
        QCOMPARE(req.key.exchange_id, QStringLiteral("polymarket"));
        QCOMPARE(req.client_order_id, draft_id);

        // Never book P&L from an order acknowledgement. Doing so would duplicate
        // the later authenticated fill stream and made accepted/resting orders
        // look executed.
        auto pos = LivePnlRepository::instance().get_open("", "polymarket", "live-happy");
        QVERIFY2(pos.is_ok() && !pos.value().has_value(),
                 "an order acknowledgement must not create a live position");

        QCOMPARE(OrderDraftRepository::instance().get(draft_id).value().status,
                 QStringLiteral("filled"));
        QVERIFY2(find_submit_audit("filled", "live"), "the live fill must be audited mode 'live'");
    }

    // A local timeout does not prove rejection. Keep the draft and its market
    // locked under the same exchange client UUID until account reconciliation
    // determines whether Kalshi accepted the order.
    void pm_submit_live_timeout_is_indeterminate_and_not_retryable() {
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.allow_paper_trading", "true");
        const QString draft_id = prepare_pm_buy("live-timeout", 10);
        QVERIFY2(!draft_id.isEmpty(), "prepare a valid BUY draft");
        QVERIFY2(fake_adapter(), "fake adapter must be registered");
        fake_adapter()->creds_ = true;
        fake_adapter()->suppress_place_result_ = true;
        const int calls_before = fake_adapter()->place_order_calls_;
        qputenv("OPENTERMINAL_TEST_PM_PLACE_TIMEOUT_MS", "25");

        arm_live();
        auto res = rt_.call_tool("submit_order",
                                 QJsonObject{{"draft_id", draft_id}, {"mode", "live"}});
        disarm_live();
        qunsetenv("OPENTERMINAL_TEST_PM_PLACE_TIMEOUT_MS");
        fake_adapter()->suppress_place_result_ = false;

        QVERIFY2(res.success, qPrintable("timeout remains an auditable decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("submission_unknown"));
        QVERIFY(data.value("reconciliation_required").toBool());
        QCOMPARE(data.value("client_order_id").toString(), draft_id);
        QCOMPARE(fake_adapter()->last_req_.client_order_id, draft_id);
        QCOMPARE(fake_adapter()->place_order_calls_, calls_before + 1);
        QCOMPARE(OrderDraftRepository::instance().get(draft_id).value().status,
                 QStringLiteral("submission_unknown"));
        verify_no_live_position("live-timeout");

        arm_live();
        const auto retry = rt_.call_tool(
            "submit_order", QJsonObject{{"draft_id", draft_id}, {"mode", "live"}});
        disarm_live();
        QVERIFY(!retry.success || retry.data.toObject().value("status").toString() ==
                                  QStringLiteral("rejected"));
        QCOMPARE(fake_adapter()->place_order_calls_, calls_before + 1);

        const QString other_draft = prepare_pm_buy(
            QStringLiteral("live-after-timeout"), 10, QStringLiteral("mkt-2"));
        QVERIFY2(!other_draft.isEmpty(), "a second market can still be drafted for review");
        arm_live();
        const auto other_submit = rt_.call_tool(
            "submit_order", QJsonObject{{"draft_id", other_draft}, {"mode", "live"}});
        disarm_live();
        QVERIFY(other_submit.success);
        QCOMPARE(other_submit.data.toObject().value("status").toString(),
                 QStringLiteral("rejected"));
        QVERIFY(other_submit.data.toObject().value("reconciliation_required").toBool());
        QCOMPARE(fake_adapter()->place_order_calls_, calls_before + 1);
        QVERIFY(OrderDraftRepository::instance()
                    .update_status(draft_id, QStringLiteral("reconciled_rejected"))
                    .is_ok());
    }

    // GATE — NOT ARMED: venue allowed + creds present, but the live arming toggle
    // is OFF. The headless auth-checker carve-out only opens the door when BOTH
    // toggles are on, so an un-armed live submit is DENIED at the checker (before
    // the handler) — res.success false. Either way the adapter is never reached
    // (no live position). Mirrors the equity live_not_armed_rejected test.
    void pm_submit_live_not_armed_rejected() {
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.allow_paper_trading", "true");
        const QString draft_id = prepare_pm_buy("live-noarm", 10);
        QVERIFY2(!draft_id.isEmpty(), "prepare a valid BUY draft");
        fake_adapter()->creds_ = true;

        disarm_live();  // explicitly NOT armed
        auto res = rt_.call_tool("submit_order",
                                 QJsonObject{{"draft_id", draft_id}, {"mode", "live"}});
        QVERIFY2(!res.success, "un-armed live submit must be denied at the gate stack");
        verify_no_live_position("live-noarm");
        QCOMPARE(OrderDraftRepository::instance().get(draft_id).value().status,
                 QStringLiteral("prepared"));
    }

    // GATE — VENUE NOT ALLOWED: armed + creds, but cli.allowed_venues cleared →
    // rejected before the adapter (no live position).
    void pm_submit_live_venue_not_allowed_rejected() {
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.allow_paper_trading", "true");
        const QString draft_id = prepare_pm_buy("live-venue", 10);
        QVERIFY2(!draft_id.isEmpty(), "prepare a valid BUY draft");
        fake_adapter()->creds_ = true;

        arm_live();
        set_setting("cli.allowed_venues", "");  // revoke venue after prepare
        auto res = rt_.call_tool("submit_order",
                                 QJsonObject{{"draft_id", draft_id}, {"mode", "live"}});
        set_setting("cli.allowed_venues", "polymarket");  // restore
        disarm_live();
        QVERIFY2(res.success, qPrintable("venue-denied is a decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("venue", Qt::CaseInsensitive),
                 qPrintable("reason: " + data.value("reason").toString()));
        verify_no_live_position("live-venue");
    }

    // GATE — NO CREDENTIALS: armed + venue allowed, but the adapter has no creds
    // → rejected before place_order (no live position). MUST restore creds_.
    void pm_submit_live_no_credentials_rejected() {
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.allow_paper_trading", "true");
        const QString draft_id = prepare_pm_buy("live-nocreds", 10);
        QVERIFY2(!draft_id.isEmpty(), "prepare a valid BUY draft");

        fake_adapter()->creds_ = false;  // no credentials configured
        arm_live();
        auto res = rt_.call_tool("submit_order",
                                 QJsonObject{{"draft_id", draft_id}, {"mode", "live"}});
        disarm_live();
        fake_adapter()->creds_ = true;  // RESTORE — shared singleton
        QVERIFY2(res.success, qPrintable("no-creds is a decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("no credentials", Qt::CaseInsensitive),
                 qPrintable("reason: " + data.value("reason").toString()));
        verify_no_live_position("live-nocreds");
        QCOMPARE(OrderDraftRepository::instance().get(draft_id).value().status,
                 QStringLiteral("prepared"));
    }

    // GATE — DAILY-LOSS HALT: armed + venue + creds, but the daily-loss cap is set
    // below this order's stake → rejected before the adapter (no live position).
    void pm_submit_live_daily_loss_rejected() {
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.allow_paper_trading", "true");
        const QString draft_id = prepare_pm_buy("live-dloss", 10);  // stake 10*0.45=4.5
        QVERIFY2(!draft_id.isEmpty(), "prepare a valid BUY draft");
        fake_adapter()->creds_ = true;

        set_setting("cli.risk.max_daily_loss", "1");  // 0 + 4.5 > 1 → halt
        arm_live();
        auto res = rt_.call_tool("submit_order",
                                 QJsonObject{{"draft_id", draft_id}, {"mode", "live"}});
        disarm_live();
        set_setting("cli.risk.max_daily_loss", "5000");  // restore
        QVERIFY2(res.success, qPrintable("daily-loss is a decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("daily loss", Qt::CaseInsensitive),
                 qPrintable("reason: " + data.value("reason").toString()));
        verify_no_live_position("live-dloss");
    }

    // GATE — KILL SWITCH DOMINATES: fully armed + venue + creds, but the panic
    // button short-circuits at the handler top → "kill switch engaged".
    void pm_submit_live_kill_switch_dominates() {
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.allow_paper_trading", "true");
        const QString draft_id = prepare_pm_buy("live-kill", 10);
        QVERIFY2(!draft_id.isEmpty(), "prepare a valid BUY draft");
        fake_adapter()->creds_ = true;

        arm_live();
        set_setting("cli.kill_switch", "true");
        auto res = rt_.call_tool("submit_order",
                                 QJsonObject{{"draft_id", draft_id}, {"mode", "live"}});
        set_setting("cli.kill_switch", "false");  // restore
        disarm_live();
        QVERIFY2(res.success, qPrintable("kill-switch is a decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("kill switch", Qt::CaseInsensitive),
                 qPrintable("reason: " + data.value("reason").toString()));
        verify_no_live_position("live-kill");
        QCOMPARE(OrderDraftRepository::instance().get(draft_id).value().status,
                 QStringLiteral("prepared"));
    }

    // SELL-to-close happy: pre-open a position via the engine, prepare a partial
    // SELL, submit paper → position reduced, cash credited at best_bid.
    void pm_submit_sell_to_close_happy() {
        using openmarketterminal::mcp::tools::buy_to_open;
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.allow_paper_trading", "true");

        // Pre-open 20 @ 0.45 directly on the engine.
        auto pre = buy_to_open("polymarket", "mkt-1", "sub-sell", "Yes", "weather", 20, 0.45);
        QVERIFY2(pre.ok, qPrintable("pre-open failed: " + pre.reason));

        // Prepare a SELL of 8 contracts.
        auto prep = rt_.call_tool(
            "prepare_order",
            QJsonObject{{"asset_class", "prediction"}, {"venue", "polymarket"},
                        {"market_id", "mkt-1"}, {"asset_id", "sub-sell"},
                        {"outcome", "Yes"}, {"side", "sell"}, {"contracts", 8}});
        QVERIFY2(prep.success, qPrintable("prepare sell: " + prep.error));
        const QString draft_id = prep.data.toObject().value("draft_id").toString();
        QVERIFY2(!draft_id.isEmpty(), "prepared SELL must carry a draft_id");

        const double cash_before = PmPaperRepository::instance().cash().value();
        auto res = rt_.call_tool("submit_order",
                                 QJsonObject{{"draft_id", draft_id}, {"mode", "paper"}});
        QVERIFY2(res.success, qPrintable("sell submit: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("filled"));
        QCOMPARE(data.value("action").toString(), QStringLiteral("sell_to_close"));
        QVERIFY(qFuzzyCompare(data.value("fill_price").toDouble(), 0.40));  // best_bid

        auto open = PmPaperRepository::instance().get_open("polymarket", "sub-sell");
        QVERIFY2(open.value().has_value(), "partial sell leaves an open position");
        QCOMPARE(open.value()->contracts, 12.0);  // 20 - 8

        QVERIFY2(qFuzzyCompare(PmPaperRepository::instance().cash().value(), cash_before + 3.2),
                 "cash credited by 8 * 0.40 = 3.2");
    }

    // Revocable re-check: prepare within caps, then LOWER max_order_value below the
    // staked value → submit paper rejected at SUBMIT (fresh risk), no fill.
    void pm_submit_revocable_recheck_rejected() {
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.allow_paper_trading", "true");
        // 100 * 0.45 = 45 stake (within the 25000 default at prepare).
        const QString draft_id = prepare_pm_buy("sub-revoke", 100);
        QVERIFY2(!draft_id.isEmpty(), "prepare a valid BUY draft");

        set_setting("cli.risk.max_order_value", "10");  // below 45 → rejects at submit
        auto res = rt_.call_tool("submit_order",
                                 QJsonObject{{"draft_id", draft_id}, {"mode", "paper"}});
        set_setting("cli.risk.max_order_value", "25000");  // restore
        QVERIFY2(res.success, qPrintable("risk re-check is a decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("max order value", Qt::CaseInsensitive),
                 qPrintable("reason: " + data.value("reason").toString()));

        QVERIFY2(!PmPaperRepository::instance().get_open("polymarket", "sub-revoke").value().has_value(),
                 "a risk-rejected submit must NOT fill");
    }

    // Double-submit: the same buy draft submitted twice → 2nd rejected (the draft
    // is consumed), only one position effect.
    void pm_submit_double_submit_rejected() {
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.allow_paper_trading", "true");
        const QString draft_id = prepare_pm_buy("sub-double", 10);
        QVERIFY2(!draft_id.isEmpty(), "prepare a valid BUY draft");

        auto first = rt_.call_tool("submit_order",
                                   QJsonObject{{"draft_id", draft_id}, {"mode", "paper"}});
        QVERIFY2(first.success, qPrintable("first submit: " + first.error));
        QCOMPARE(first.data.toObject().value("status").toString(), QStringLiteral("filled"));

        auto second = rt_.call_tool("submit_order",
                                    QJsonObject{{"draft_id", draft_id}, {"mode", "paper"}});
        QVERIFY2(second.success, qPrintable("second submit is a decision: " + second.error));
        const QJsonObject data = second.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("already used", Qt::CaseInsensitive),
                 qPrintable("reason: " + data.value("reason").toString()));

        auto open = PmPaperRepository::instance().get_open("polymarket", "sub-double");
        QVERIFY2(open.value().has_value(), "the one successful submit must have opened a position");
        QCOMPARE(open.value()->contracts, 10.0);  // not 20 — the 2nd submit had no effect
    }

    // An acknowledgement with zero confirmed fills is a RESTING order, not a
    // position and not a completed evidence point. It must report as such, and
    // the live P&L ledger must stay untouched until authenticated fill
    // reconciliation observes a uniquely identified execution.
    void pm_submit_live_accepted_but_unfilled_is_not_a_fill() {
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.allow_paper_trading", "true");
        const QString draft_id = prepare_pm_buy("live-resting", 10, QStringLiteral("mkt-resting"));
        QVERIFY2(!draft_id.isEmpty(), "prepare a valid BUY draft");
        QVERIFY2(fake_adapter(), "fake adapter must be registered");
        fake_adapter()->creds_ = true;
        fake_adapter()->filled_override_ = 0.0;
        fake_adapter()->status_override_ = QStringLiteral("RESTING");

        arm_live();
        const auto res = rt_.call_tool(
            "submit_order", QJsonObject{{"draft_id", draft_id}, {"mode", "live"}});
        disarm_live();
        fake_adapter()->filled_override_ = -1.0;
        fake_adapter()->status_override_.clear();

        QVERIFY2(res.success, qPrintable("a resting live submit is auditable: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("resting"));
        QCOMPARE(data.value("filled_count").toDouble(), 0.0);
        QCOMPARE(data.value("remaining_count").toDouble(), 10.0);
        QCOMPARE(OrderDraftRepository::instance().get(draft_id).value().status,
                 QStringLiteral("resting"));
        verify_no_live_position("live-resting");
        QVERIFY2(find_submit_audit("resting", "live"),
                 "an accepted but unfilled order must NOT be audited as filled");
        QVERIFY2(!find_submit_audit("filled", "live") ||
                     TradeAuditRepository::instance().recent(1).value().front().decision !=
                         QLatin1String("filled"),
                 "the most recent submit decision must not be 'filled'");
    }

    // A partial acknowledgement reports only the confirmed quantity and leaves
    // the resting remainder visible; it still books no P&L from the ack.
    void pm_submit_live_partial_fill_reports_confirmed_quantity_only() {
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.allow_paper_trading", "true");
        const QString draft_id = prepare_pm_buy("live-partial", 10, QStringLiteral("mkt-partial"));
        QVERIFY2(!draft_id.isEmpty(), "prepare a valid BUY draft");
        QVERIFY2(fake_adapter(), "fake adapter must be registered");
        fake_adapter()->creds_ = true;
        fake_adapter()->filled_override_ = 4.0;

        arm_live();
        const auto res = rt_.call_tool(
            "submit_order", QJsonObject{{"draft_id", draft_id}, {"mode", "live"}});
        disarm_live();
        fake_adapter()->filled_override_ = -1.0;

        QVERIFY2(res.success, qPrintable("a partial live submit is auditable: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("partially_filled"));
        QCOMPARE(data.value("filled_count").toDouble(), 4.0);
        QCOMPARE(data.value("remaining_count").toDouble(), 6.0);
        QCOMPARE(OrderDraftRepository::instance().get(draft_id).value().status,
                 QStringLiteral("partially_filled"));
        verify_no_live_position("live-partial");
        QVERIFY2(find_submit_audit("partially_filled", "live"),
                 "a partial acknowledgement must be audited as partially filled");
    }

    // ── micro-live experiment gates (session id + all-in ceiling) ────────────

    // Helper: prepare a micro-live experiment draft. Every field the submit gate
    // fails closed on is a parameter so a test can omit exactly one of them.
    QString prepare_experiment_buy(const QString& asset_id, const QString& market_id,
                                   double contracts, double max_live_stake,
                                   double max_live_all_in, double estimated_fee,
                                   const QString& session_id) {
        QJsonObject args{{"asset_class", "prediction"}, {"venue", "polymarket"},
                         {"market_id", market_id}, {"asset_id", asset_id},
                         {"outcome", "Yes"}, {"side", "buy"}, {"contracts", contracts},
                         {"limit_price", 0.45},
                         {"experiment_id", "kalshi-micro-live-v1"},
                         {"max_live_stake", max_live_stake},
                         {"max_live_all_in", max_live_all_in},
                         {"estimated_fee", estimated_fee},
                         {"estimated_total", contracts * 0.45 + estimated_fee},
                         {"experiment_loss_cap", 120.0}};
        if (!session_id.isEmpty())
            args.insert(QStringLiteral("automation_session_id"), session_id);
        auto res = rt_.call_tool("prepare_order", args);
        if (!res.success) return {};
        const QJsonObject d = res.data.toObject();
        if (d.value("status").toString() != QLatin1String("prepared")) return {};
        return d.value("draft_id").toString();
    }

    // The stake cap bounds contract cost only. What actually leaves the account
    // is stake PLUS the venue fee, so the all-in ceiling is enforced from the
    // immutable draft at submit — the adapter is never reached when it breaches.
    void pm_submit_live_all_in_cap_rejected() {
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.allow_paper_trading", "true");
        QVERIFY2(fake_adapter(), "fake adapter must be registered");
        fake_adapter()->creds_ = true;

        // stake 2 * 0.45 = 0.90 (inside max_live_stake 1.00) but 0.90 + 0.50 fee
        // = 1.40, which breaches the 1.00 all-in ceiling.
        const QString draft_id = prepare_experiment_buy(
            QStringLiteral("live-allin"), QStringLiteral("mkt-allin"), 2, 1.0, 1.0, 0.50,
            QStringLiteral("session-allin"));
        QVERIFY2(!draft_id.isEmpty(), "prepare a micro-live draft");
        const int calls_before = fake_adapter()->place_order_calls_;

        arm_live();
        const auto res = rt_.call_tool(
            "submit_order", QJsonObject{{"draft_id", draft_id}, {"mode", "live"}});
        disarm_live();

        QVERIFY2(res.success, qPrintable("an all-in breach is an auditable decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("all-in cap exceeded"),
                 qPrintable("reason: " + data.value("reason").toString()));
        QCOMPARE(fake_adapter()->place_order_calls_, calls_before);
        verify_no_live_position("live-allin");

        // Control: the SAME order with an honest fee that fits under the ceiling
        // clears the gate and reaches the adapter, so the rejection above can
        // only have come from the all-in check.
        const QString ok_draft = prepare_experiment_buy(
            QStringLiteral("live-allin-ok"), QStringLiteral("mkt-allin-ok"), 2, 1.0, 1.0, 0.05,
            QStringLiteral("session-allin"));
        QVERIFY2(!ok_draft.isEmpty(), "prepare a within-ceiling micro-live draft");
        arm_live();
        const auto ok_res = rt_.call_tool(
            "submit_order", QJsonObject{{"draft_id", ok_draft}, {"mode", "live"}});
        disarm_live();
        QVERIFY2(ok_res.success, qPrintable("within-ceiling submit: " + ok_res.error));
        QCOMPARE(ok_res.data.toObject().value("status").toString(), QStringLiteral("filled"));
        QCOMPARE(fake_adapter()->place_order_calls_, calls_before + 1);
    }

    // A micro-live order that cannot be attributed to an armed automation
    // session is not a bounded experiment order. This holds for EVERY
    // experiment draft, not only the autonomous ones.
    void pm_submit_live_without_session_id_rejected() {
        set_setting("cli.allowed_venues", "polymarket");
        set_setting("cli.allow_paper_trading", "true");
        QVERIFY2(fake_adapter(), "fake adapter must be registered");
        fake_adapter()->creds_ = true;

        const QString draft_id = prepare_experiment_buy(
            QStringLiteral("live-nosession"), QStringLiteral("mkt-nosession"), 2, 1.0, 1.0, 0.05,
            QString());
        QVERIFY2(!draft_id.isEmpty(), "prepare a session-less micro-live draft");
        const int calls_before = fake_adapter()->place_order_calls_;

        arm_live();
        const auto res = rt_.call_tool(
            "submit_order", QJsonObject{{"draft_id", draft_id}, {"mode", "live"}});
        disarm_live();

        QVERIFY2(res.success, qPrintable("a session-less draft is an auditable decision: " + res.error));
        const QJsonObject data = res.data.toObject();
        QCOMPARE(data.value("status").toString(), QStringLiteral("rejected"));
        QVERIFY2(data.value("reason").toString().contains("armed automation session"),
                 qPrintable("reason: " + data.value("reason").toString()));
        QCOMPARE(fake_adapter()->place_order_calls_, calls_before);
        verify_no_live_position("live-nosession");
    }

    void cleanupTestCase() { rt_.shutdown(); }
};

QTEST_MAIN(TstPmPaper)
#include "tst_pm_paper.moc"
