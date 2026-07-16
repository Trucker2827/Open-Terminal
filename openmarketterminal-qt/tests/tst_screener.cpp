// tst_screener.cpp — Screener::screen (ai screen shortlist, Task 1): gathers
// distinct recent (symbol, venue) pairs from edge_decision_journal per
// market, runs each through DecisionContext::assess (reused, not
// re-derived), keeps only all-gates-pass rows, ranks by edge_after_cost
// desc, returns the top-N ScreenRows.
//
// DB bring-up mirrors tst_decision_context.cpp's
// open_profile_database_for_test() (same "default" profile bootstrap).

#include <QtTest>
#include <QDateTime>
#include <QTemporaryDir>
#include <QDir>

#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "services/ai_decision/Screener.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"

using namespace openmarketterminal;
using namespace openmarketterminal::ai_decision;

namespace {

// Copies the exact DB-open incantation from tst_decision_context.cpp. Do not
// invent a new bootstrap.
bool open_profile_database_for_test() {
    ProfileManager::instance().set_active("default");
    QDir().mkpath(AppPaths::root());
    AppPaths::ensure_all();
    register_all_migrations();
    auto db = Database::instance().open(AppPaths::data() + "/openmarketterminal.db");
    return db.is_ok();
}

// assess() now expires edge_decision_journal rows older than 24h
// (wall-clock) -- seeds must be fresh, `now`-relative timestamps rather than
// tiny fixed values.
static qint64 recent_ms(qint64 back = 60000) { return QDateTime::currentMSecsSinceEpoch() - back; }

} // namespace

class TstScreener : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        static QTemporaryDir home;
        QVERIFY(home.isValid());
        qputenv("HOME", home.path().toUtf8());
        QVERIFY(open_profile_database_for_test());
    }

    void screen_filters_ranks_and_tags() {
        auto seed = [&](const QString& id, const QString& sym, const QString& venue,
                        const QString& gate, double edge, qint64 ts) {
            auto r = Database::instance().execute(
                "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, venue, side, gate,"
                " edge_after_cost, spread_cost, fee_cost, freshness_json, source) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                {id, ts, ts, sym, venue, QStringLiteral("buy"), gate, edge, 0.0, 0.0,
                 QStringLiteral("{}"), QStringLiteral("s")}); // freshness {} => "unknown" (does not block)
            QVERIFY2(r.is_ok(), r.is_err() ? r.error().c_str() : "");
        };
        // Two crypto passers (pass gate, edge>0, unknown-freshness passes), different edge.
        seed(QStringLiteral("c1"), QStringLiteral("BTC-USD"), QStringLiteral("coinbase_advanced"),
             QStringLiteral("pass"), 12.0, recent_ms());
        seed(QStringLiteral("c2"), QStringLiteral("ETH-USD"), QStringLiteral("coinbase_advanced"),
             QStringLiteral("pass"), 5.0, recent_ms());
        // A crypto FAIL (excluded).
        seed(QStringLiteral("c3"), QStringLiteral("SOL-USD"), QStringLiteral("coinbase_advanced"),
             QStringLiteral("fail"), 9.0, recent_ms());
        // A prediction passer (kalshi).
        seed(QStringLiteral("p1"), QStringLiteral("KXBTC-USD"), QStringLiteral("kalshi"),
             QStringLiteral("pass"), 7.0, recent_ms());

        const auto crypto = screen(QStringLiteral("crypto"), 5);
        QCOMPARE(crypto.size(), 2);                                  // SOL-USD fail excluded
        QCOMPARE(crypto[0].symbol, QStringLiteral("BTC-USD"));       // ranked: 12 before 5
        QCOMPARE(crypto[1].symbol, QStringLiteral("ETH-USD"));
        QCOMPARE(crypto[0].market, QStringLiteral("crypto"));
        QCOMPARE(crypto[0].recommendation_hint, QStringLiteral("all gates pass"));

        const auto pred = screen(QStringLiteral("prediction"), 5);
        QCOMPARE(pred.size(), 1);
        QCOMPARE(pred[0].symbol, QStringLiteral("KXBTC-USD"));
        QCOMPARE(pred[0].market, QStringLiteral("prediction"));

        const auto all = screen(QString(), 5);                      // all markets tagged
        QCOMPARE(all.size(), 3);                                    // 2 crypto + 1 prediction
        QCOMPARE(all[0].symbol, QStringLiteral("BTC-USD"));         // highest edge first, cross-market

        const auto limited = screen(QString(), 1);
        QCOMPARE(limited.size(), 1);
        QCOMPARE(limited[0].symbol, QStringLiteral("BTC-USD"));
    }

    // Regression: the universe is DISTINCT (symbol, venue), but assess() keys
    // on symbol alone. A symbol logged under two venues that map to the SAME
    // market (e.g. BTC-USD on coinbase_advanced AND coinbase, both "crypto")
    // yields two universe rows returning the same packet — so it must be
    // deduped by (symbol, market) to appear exactly once, not twice consuming
    // two shortlist slots.
    void screen_dedups_same_symbol_across_same_market_venues() {
        auto seed = [&](const QString& id, const QString& sym, const QString& venue,
                        const QString& gate, double edge, qint64 ts) {
            auto r = Database::instance().execute(
                "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, venue, side, gate,"
                " edge_after_cost, spread_cost, fee_cost, freshness_json, source) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                {id, ts, ts, sym, venue, QStringLiteral("buy"), gate, edge, 0.0, 0.0,
                 QStringLiteral("{}"), QStringLiteral("s")});
            QVERIFY2(r.is_ok(), r.is_err() ? r.error().c_str() : "");
        };
        // Same symbol (XRP-USD) under two same-market (crypto) venues, both
        // passing gates. Distinct id/venue => two DISTINCT (symbol, venue)
        // universe rows for the one symbol.
        seed(QStringLiteral("d1"), QStringLiteral("XRP-USD"), QStringLiteral("coinbase_advanced"),
             QStringLiteral("pass"), 12.0, recent_ms());
        seed(QStringLiteral("d2"), QStringLiteral("XRP-USD"), QStringLiteral("coinbase"),
             QStringLiteral("pass"), 8.0, recent_ms());

        const auto crypto = screen(QStringLiteral("crypto"), 5);
        int xrp_count = 0;
        for (const auto& row : crypto)
            if (row.symbol == QStringLiteral("XRP-USD"))
                ++xrp_count;
        QCOMPARE(xrp_count, 1); // deduped by (symbol, market), not twice
    }

    // Pattern-based venue->market classification: the journal writer emits an
    // open-ended crypto venue vocabulary (coinbase_tier1..9, alpaca_crypto,
    // binance, kraken*, ...), so market classification must be prefix/exact
    // pattern-based, NOT a hardcoded IN-list — else non-enumerated fee tiers
    // are silently dropped from `screen("crypto")` and mis-tagged in the
    // all-markets path.
    void screen_classifies_open_ended_crypto_venues() {
        auto seed = [&](const QString& id, const QString& sym, const QString& venue,
                        const QString& gate, double edge, qint64 ts) {
            auto r = Database::instance().execute(
                "INSERT INTO edge_decision_journal (id, created_at, updated_at, symbol, venue, side, gate,"
                " edge_after_cost, spread_cost, fee_cost, freshness_json, source) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
                {id, ts, ts, sym, venue, QStringLiteral("buy"), gate, edge, 0.0, 0.0,
                 QStringLiteral("{}"), QStringLiteral("s")});
            QVERIFY2(r.is_ok(), r.is_err() ? r.error().c_str() : "");
        };
        auto contains = [](const QVector<ScreenRow>& rows, const QString& sym) {
            for (const auto& row : rows)
                if (row.symbol == sym)
                    return true;
            return false;
        };
        auto market_of = [](const QVector<ScreenRow>& rows, const QString& sym) -> QString {
            for (const auto& row : rows)
                if (row.symbol == sym)
                    return row.market;
            return QString();
        };

        // (a) A passer on a non-enumerated fee tier (coinbase_tier5) is
        // classified crypto and INCLUDED in screen("crypto").
        seed(QStringLiteral("t5"), QStringLiteral("AVAX-USD"), QStringLiteral("coinbase_tier5"),
             QStringLiteral("pass"), 11.0, recent_ms());
        // And alpaca_crypto is crypto too.
        seed(QStringLiteral("ac"), QStringLiteral("LTC-USD"), QStringLiteral("alpaca_crypto"),
             QStringLiteral("pass"), 6.0, recent_ms());
        const auto crypto = screen(QStringLiteral("crypto"), 20);
        QVERIFY2(contains(crypto, QStringLiteral("AVAX-USD")), "coinbase_tier5 passer must be crypto");
        QCOMPARE(market_of(crypto, QStringLiteral("AVAX-USD")), QStringLiteral("crypto"));
        QVERIFY2(contains(crypto, QStringLiteral("LTC-USD")), "alpaca_crypto passer must be crypto");
        QCOMPARE(market_of(crypto, QStringLiteral("LTC-USD")), QStringLiteral("crypto"));

        // (b) An unclassifiable venue is DROPPED from the all-markets path
        // (never emitted with an empty market tag).
        seed(QStringLiteral("wv"), QStringLiteral("WEIRD-SYM"), QStringLiteral("weirdvenue"),
             QStringLiteral("pass"), 99.0, recent_ms());
        const auto all = screen(QString(), 50);
        QVERIFY2(!contains(all, QStringLiteral("WEIRD-SYM")),
                 "unclassifiable venue must be dropped, never tagged market=''");

        // (c) Cross-tier double-count guard: same symbol under two crypto fee
        // tiers (coinbase_advanced + coinbase_tier5) both now map to "crypto",
        // so the (symbol, market) dedup collapses them to ONE row.
        seed(QStringLiteral("x1"), QStringLiteral("MATIC-USD"), QStringLiteral("coinbase_advanced"),
             QStringLiteral("pass"), 10.0, recent_ms());
        seed(QStringLiteral("x2"), QStringLiteral("MATIC-USD"), QStringLiteral("coinbase_tier5"),
             QStringLiteral("pass"), 4.0, recent_ms());
        const auto all2 = screen(QString(), 50);
        int matic_count = 0;
        for (const auto& row : all2)
            if (row.symbol == QStringLiteral("MATIC-USD"))
                ++matic_count;
        QCOMPARE(matic_count, 1); // both tiers -> crypto -> collapsed by dedup
    }
};
QTEST_GUILESS_MAIN(TstScreener)
#include "tst_screener.moc"
