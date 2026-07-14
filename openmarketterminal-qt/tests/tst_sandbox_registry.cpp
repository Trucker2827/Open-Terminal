// tst_sandbox_registry.cpp — SandboxRegistry: immutable, param-hashed
// strategy books on top of migration v056's sandbox_strategy table.
//
// DB bring-up mirrors tst_sandbox_schema.cpp / tst_data_services.cpp's
// initTestCase(): select the "default" profile, create its datadir tree,
// register migrations, then open the DB (which runs them).

#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "services/sandbox/SandboxRegistry.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"

using namespace openmarketterminal;
using namespace openmarketterminal::services::sandbox;

namespace {

// Copies the exact DB-open incantation from tst_data_services.cpp /
// tst_sandbox_schema.cpp. Do not invent a new bootstrap.
bool open_profile_database_for_test() {
    ProfileManager::instance().set_active("default");
    QDir().mkpath(AppPaths::root());
    AppPaths::ensure_all();
    register_all_migrations();
    auto db = Database::instance().open(AppPaths::data() + "/openmarketterminal.db");
    return db.is_ok();
}

// Generic sample params — shape doesn't matter, only that changing a field
// changes the hash. NOT one of the season-1 seed kinds, so tests that write
// rows under made-up kinds never collide with seed_default_strategies() rows
// or with each other (each write-test below uses its own kind string, since
// the DB persists across test slots within this binary).
QJsonObject sample_params() {
    return QJsonObject{{"notional_usd", 50.0},   {"source", "scalp_decisions"}, {"max_age_sec", 15},
                        {"entry_offset_bps", 1.0}, {"target_bps", 25.0},          {"stop_bps", 15.0},
                        {"horizon_sec", 900}};
}

} // namespace

class TstSandboxRegistry : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        static QTemporaryDir home;
        QVERIFY(home.isValid());
        qputenv("HOME", home.path().toUtf8());
        QVERIFY(open_profile_database_for_test());
    }

    // (a) strategy_id_for is stable across calls and differs when any of
    // kind/symbols/params changes.
    void id_is_stable_and_sensitive_to_every_field() {
        const QJsonObject params = sample_params();
        const QString id1 = strategy_id_for("scalp", "BTC-USD", params);
        const QString id2 = strategy_id_for("scalp", "BTC-USD", params);
        QCOMPARE(id1, id2);
        QVERIFY(!id1.isEmpty());

        QCOMPARE(strategy_id_for("spot", "BTC-USD", params) == id1, false);
        QCOMPARE(strategy_id_for("scalp", "ETH-USD", params) == id1, false);

        QJsonObject changed = params;
        changed["target_bps"] = 26.0;
        QCOMPARE(strategy_id_for("scalp", "BTC-USD", changed) == id1, false);
    }

    // (b) register_strategy twice with same inputs -> same id, one row.
    void register_is_idempotent_for_identical_inputs() {
        const QJsonObject params = sample_params();
        auto r1 = register_strategy("test_idem", "ZZZ-USD", params, "first");
        QVERIFY2(r1.is_ok(), r1.is_err() ? r1.error().c_str() : "");
        auto r2 = register_strategy("test_idem", "ZZZ-USD", params, "first");
        QVERIFY2(r2.is_ok(), r2.is_err() ? r2.error().c_str() : "");
        QCOMPARE(r1.value(), r2.value());

        auto rows = list_strategies();
        QVERIFY(rows.is_ok());
        int count = 0;
        for (const auto& row : rows.value())
            if (row.strategy_id == r1.value())
                ++count;
        QCOMPARE(count, 1);
    }

    // (c) register, then register again with one param changed -> two rows;
    // first row's params_json (and notes) byte-identical to before — proves
    // register_strategy never touches an existing row, even when called
    // again for the same (kind, symbols) with different notes.
    void register_never_mutates_an_existing_row() {
        const QJsonObject paramsA = sample_params();
        auto rA = register_strategy("test_immut", "ZZZ-USD", paramsA, "note-v1");
        QVERIFY(rA.is_ok());
        const QString idA = rA.value();

        auto before = list_strategies();
        QVERIFY(before.is_ok());
        StrategyRow rowA_before;
        bool found = false;
        for (const auto& row : before.value())
            if (row.strategy_id == idA) { rowA_before = row; found = true; }
        QVERIFY(found);

        // Re-register the SAME (kind, symbols, params) with DIFFERENT notes —
        // must return the same id and must NOT overwrite notes or params_json.
        auto rA2 = register_strategy("test_immut", "ZZZ-USD", paramsA, "note-v2-should-be-ignored");
        QVERIFY(rA2.is_ok());
        QCOMPARE(rA2.value(), idA);

        // Register a genuinely different strategy (one param changed) — must
        // get a different id, i.e. a second row.
        QJsonObject paramsB = paramsA;
        paramsB["target_bps"] = 26.0;
        auto rB = register_strategy("test_immut", "ZZZ-USD", paramsB, "note-b");
        QVERIFY(rB.is_ok());
        const QString idB = rB.value();
        QVERIFY(idB != idA);

        auto after = list_strategies();
        QVERIFY(after.is_ok());
        StrategyRow rowA_after;
        found = false;
        int matching_ids = 0;
        for (const auto& row : after.value()) {
            if (row.strategy_id == idA) { rowA_after = row; found = true; ++matching_ids; }
            if (row.strategy_id == idB) ++matching_ids;
        }
        QVERIFY(found);
        QCOMPARE(matching_ids, 2);

        // Immutability: params_json and notes on row A must be byte-identical
        // to the very first registration, unaffected by the second call.
        QCOMPARE(rowA_after.params_json, rowA_before.params_json);
        QCOMPARE(rowA_after.notes, QStringLiteral("note-v1"));
        QCOMPARE(rowA_after.created_at, rowA_before.created_at);
    }

    // (d) set_status round-trips; invalid status errs.
    void set_status_validates_and_round_trips() {
        auto r = register_strategy("test_status", "ZZZ-USD", sample_params(), "status-test");
        QVERIFY(r.is_ok());
        const QString id = r.value();

        auto ok = set_status(id, "paused");
        QVERIFY(ok.is_ok());
        auto rows = list_strategies();
        QVERIFY(rows.is_ok());
        QString status;
        for (const auto& row : rows.value())
            if (row.strategy_id == id) status = row.status;
        QCOMPARE(status, QStringLiteral("paused"));

        auto bad = set_status(id, "bogus");
        QVERIFY2(bad.is_err(), "set_status must reject a status outside active|paused|retired");

        // status must be unchanged after the rejected call
        rows = list_strategies();
        QVERIFY(rows.is_ok());
        status.clear();
        for (const auto& row : rows.value())
            if (row.strategy_id == id) status = row.status;
        QCOMPARE(status, QStringLiteral("paused"));
    }

    // (e) seed_default_strategies is idempotent. Scalp books are separate by
    // venue so fee-dead Coinbase results cannot suppress Kraken research.
    void seed_default_strategies_is_idempotent() {
        auto first = seed_default_strategies();
        QVERIFY2(first.is_ok(), first.is_err() ? first.error().c_str() : "");
        QCOMPARE(first.value().size(), 35);  // 28 base + 7 activatable spot-grid lanes

        auto second = seed_default_strategies();
        QVERIFY2(second.is_ok(), second.is_err() ? second.error().c_str() : "");
        QCOMPARE(second.value().size(), 35);
        QCOMPARE(second.value(), first.value());

        auto rows = list_strategies();
        QVERIFY(rows.is_ok());
        QSet<QString> seed_ids(first.value().begin(), first.value().end());
        QSet<QString> kinds;
        int seed_row_count = 0;
        QSet<int> spot_horizons;
        int spot_count = 0;
        int scalp_count = 0;
        int kalshi_count = 0;
        int long_short_count = 0;
        QSet<QString> scalp_venues;
        QSet<QString> kalshi_cohorts;
        QSet<QString> kalshi_exit_policies;
        for (const auto& row : rows.value()) {
            if (!seed_ids.contains(row.strategy_id))
                continue;
            ++seed_row_count;
            kinds.insert(row.kind);
            const QJsonObject params = QJsonDocument::fromJson(row.params_json.toUtf8()).object();
            const int horizon_sec = params.value(QStringLiteral("horizon_sec")).toInt();
            if (row.kind == QStringLiteral("spot")) {
                ++spot_count;
                spot_horizons.insert(horizon_sec);
            } else if (row.kind == QStringLiteral("scalp")) {
                ++scalp_count;
                scalp_venues.insert(params.value(QStringLiteral("venue")).toString());
            } else if (row.kind == QStringLiteral("kalshi")) {
                ++kalshi_count;
                kalshi_cohorts.insert(params.value(QStringLiteral("horizon")).toString() + QLatin1Char(':') +
                                      params.value(QStringLiteral("entry_cohort")).toString());
                kalshi_exit_policies.insert(params.value(QStringLiteral("exit_policy")).toString());
                QCOMPARE(params.value(QStringLiteral("experiment_protocol")).toString(),
                         QStringLiteral("kalshi-v2"));
            } else if (row.kind == QStringLiteral("long_short")) {
                ++long_short_count;
            }
        }
        QCOMPARE(seed_row_count, 35);
        QCOMPARE(kinds, QSet<QString>({"scalp", "spot", "swing", "kalshi", "long_short", "chronos2",
                                       "chronos2_1h", "chronos2_1d", "chronos2_equity"}));
        QCOMPARE(scalp_count, 6);  // 2 legacy + 4 honest grid (coinbase_advanced/kraken_pro maker+taker)
        QCOMPARE(scalp_venues, QSet<QString>({"kraken_pro", "coinbase_advanced"}));
        QCOMPARE(spot_count, 3);
        QCOMPARE(kalshi_count, 18);
        QCOMPARE(kalshi_cohorts.size(), 9);
        QCOMPARE(kalshi_exit_policies, QSet<QString>({"settlement", "managed"}));
        QCOMPARE(long_short_count, 1);
        QCOMPARE(spot_horizons, QSet<int>({3600, 14400, 86400}));

        // No book of a removed kind is ever seeded.
        for (const auto& row : rows.value()) {
            if (!seed_ids.contains(row.strategy_id))
                continue;
            QVERIFY2(row.kind != QStringLiteral("btc5m"), "btc5m must not be seeded");
            QVERIFY2(row.kind != QStringLiteral("chronos2_5m"), "chronos2_5m must not be seeded");
        }
    }

    // Seeding activates exactly the 7 producer-backed spot-grid lanes -- honest
    // execution params, kinds scalp + swing -- distinct from the legacy books.
    void seed_activates_honest_spot_grid_lanes() {
        auto seeded = seed_default_strategies();
        QVERIFY2(seeded.is_ok(), seeded.is_err() ? seeded.error().c_str() : "");
        auto rows = list_strategies(QStringLiteral("active"));
        QVERIFY(rows.is_ok());

        int honest = 0, honest_scalp = 0, honest_swing = 0;
        for (const auto& row : rows.value()) {
            const QJsonObject params = QJsonDocument::fromJson(row.params_json.toUtf8()).object();
            if (!params.contains(QStringLiteral("half_spread_bps")))
                continue;  // legacy books carry no honest params
            ++honest;
            QVERIFY(params.contains(QStringLiteral("source")));  // activatable => producer-backed
            QVERIFY(params.value(QStringLiteral("paper_only")).toBool());
            if (row.kind == QStringLiteral("swing")) ++honest_swing;
            else if (row.kind == QStringLiteral("scalp")) ++honest_scalp;
        }
        QCOMPARE(honest, 7);
        QCOMPARE(honest_scalp, 4);  // coinbase/kraken maker+taker
        QCOMPARE(honest_swing, 3);  // coinbase/kraken/alpaca
    }

    // (e2) seeding retires legacy venue-less scalp plus removed horizon kinds.
    void seed_default_strategies_retires_removed_kinds() {
        auto pre_scalp = register_strategy(QStringLiteral("scalp"), QStringLiteral("BTC-USD"),
                                           QJsonObject{{"notional_usd", 50.0}, {"horizon_sec", 900}},
                                           QStringLiteral("pre-existing scalp book"));
        QVERIFY(pre_scalp.is_ok());
        auto pre_btc5m = register_strategy(QStringLiteral("btc5m"), QStringLiteral("BTC-USD"),
                                           QJsonObject{{"notional_usd", 50.0}, {"max_age_sec", 900}},
                                           QStringLiteral("pre-existing btc5m book"));
        QVERIFY(pre_btc5m.is_ok());
        auto pre_chronos5m = register_strategy(QStringLiteral("chronos2_5m"), QStringLiteral("BTC-USD"),
                                               QJsonObject{{"horizon", "5m"}, {"horizon_sec", 300}},
                                               QStringLiteral("pre-existing chronos2_5m book"));
        QVERIFY(pre_chronos5m.is_ok());

        // Confirm they start ACTIVE (register_strategy's default status).
        auto before = list_strategies(QStringLiteral("active"));
        QVERIFY(before.is_ok());
        QSet<QString> active_ids_before;
        for (const auto& row : before.value())
            active_ids_before.insert(row.strategy_id);
        QVERIFY(active_ids_before.contains(pre_scalp.value()));
        QVERIFY(active_ids_before.contains(pre_btc5m.value()));
        QVERIFY(active_ids_before.contains(pre_chronos5m.value()));

        auto seeded = seed_default_strategies();
        QVERIFY2(seeded.is_ok(), seeded.is_err() ? seeded.error().c_str() : "");

        auto after = list_strategies();
        QVERIFY(after.is_ok());
        for (const auto& row : after.value()) {
            if (row.strategy_id == pre_scalp.value() || row.strategy_id == pre_btc5m.value() ||
                row.strategy_id == pre_chronos5m.value()) {
                QCOMPARE(row.status, QStringLiteral("retired"));
            }
        }

        auto active_after = list_strategies(QStringLiteral("active"));
        QVERIFY(active_after.is_ok());
        bool found_venue_scalp = false;
        for (const auto& row : active_after.value()) {
            QVERIFY2(row.kind != QStringLiteral("btc5m"), "no ACTIVE btc5m book may remain after reseed");
            QVERIFY2(row.kind != QStringLiteral("chronos2_5m"),
                     "no ACTIVE chronos2_5m book may remain after reseed");
            if (row.kind == QStringLiteral("scalp")) {
                const auto params = QJsonDocument::fromJson(row.params_json.toUtf8()).object();
                QVERIFY2(!params.value(QStringLiteral("venue")).toString().isEmpty(),
                         "every active scalp book must identify its execution venue");
                found_venue_scalp = true;
            }
        }
        QVERIFY(found_venue_scalp);
    }

    // Contract test: pin every non-scalp seed's journal_source
    // against its producer's actual source string so a future seed/producer
    // drift fails loudly here instead of shipping a dead book.
    //
    // scalp is intentionally excluded: it reads scalp_decisions.jsonl, not
    // edge_decision_journal, so it has no journal_source/producer pair.
    // Since 'spot' and 'kalshi' are now each 3 rows (horizon variants, same
    // kind, distinct params/strategy_id), this asserts EVERY row of a kind
    // matches the producer's source, not just one representative per kind.
    void seed_journal_sources_match_their_journal_producers() {
        auto seeded = seed_default_strategies();
        QVERIFY2(seeded.is_ok(), seeded.is_err() ? seeded.error().c_str() : "");

        auto rows = list_strategies();
        QVERIFY(rows.is_ok());

        // Expected producer source strings, one per seed kind -- hardcoded
        // here (rather than derived) so this test can only pass if every
        // seeded row's journal_source is byte-identical to what the
        // producer actually writes on the CommandDispatch side.
        QHash<QString, QString> expected{
            {QStringLiteral("spot"), QStringLiteral("edge crypto-recommend")},        // CommandDispatch.cpp ~13611
            {QStringLiteral("kalshi"), QStringLiteral("kalshi auto-plan")},
            {QStringLiteral("long_short"), QStringLiteral("edge long-short-strategy")}, // CommandDispatch.cpp ~13824
            {QStringLiteral("chronos2"), QStringLiteral("chronos2-forecast")},
            {QStringLiteral("chronos2_1h"), QStringLiteral("chronos2-forecast")},
            {QStringLiteral("chronos2_1d"), QStringLiteral("chronos2-forecast")},
            {QStringLiteral("chronos2_equity"), QStringLiteral("chronos2-equity-forecast")},
        };

        QSet<QString> seed_ids(seeded.value().begin(), seeded.value().end());
        QHash<QString, int> checked_count;
        for (const auto& row : rows.value()) {
            if (!seed_ids.contains(row.strategy_id))
                continue;
            if (!expected.contains(row.kind))
                continue; // scalp reads JSONL; removed horizon kinds are not seeded
            QJsonObject params = QJsonDocument::fromJson(row.params_json.toUtf8()).object();
            QCOMPARE(params.value(QStringLiteral("journal_source")).toString(), expected.value(row.kind));
            ++checked_count[row.kind];
        }
        // Every expected kind must actually have been present and checked --
        // 3 rows for spot (horizon variants), 1 row for the rest.
        QCOMPARE(checked_count.value(QStringLiteral("spot")), 3);
        QCOMPARE(checked_count.value(QStringLiteral("kalshi")), 18);
        QCOMPARE(checked_count.value(QStringLiteral("long_short")), 1);
        QCOMPARE(checked_count.value(QStringLiteral("chronos2")), 1);
        QCOMPARE(checked_count.value(QStringLiteral("chronos2_1h")), 1);
        QCOMPARE(checked_count.value(QStringLiteral("chronos2_1d")), 1);
        QCOMPARE(checked_count.value(QStringLiteral("chronos2_equity")), 1);
    }

    // The spot measurement grid enumerates every style x venue with an honest
    // execution-cost profile on each lane. Pure (no DB) -- just shape checks.
    void spot_lane_grid_covers_styles_venues_and_honest_costs() {
        const auto grid = spot_lane_grid();
        QCOMPARE(grid.size(), 12);  // 3 venues x (scalp-maker, scalp-taker, swing, maker)

        QSet<QString> kinds;
        QSet<QString> venues;
        QSet<QString> ids;
        int maker_liquidity = 0;
        int taker_liquidity = 0;
        for (const auto& lane : grid) {
            kinds.insert(lane.kind);
            venues.insert(lane.params.value(QStringLiteral("venue")).toString());
            ids.insert(strategy_id_for(lane.kind, lane.symbols, lane.params));

            // Every lane carries the honest-cost params -- the whole point.
            QVERIFY(lane.params.contains(QStringLiteral("half_spread_bps")));
            QVERIFY(lane.params.contains(QStringLiteral("maker_fill_through_bps")));
            QVERIFY(lane.params.contains(QStringLiteral("slippage_bps")));
            QVERIFY(lane.params.value(QStringLiteral("paper_only")).toBool());

            const QString liquidity = lane.params.value(QStringLiteral("liquidity")).toString();
            const double through = lane.params.value(QStringLiteral("maker_fill_through_bps")).toDouble();
            if (liquidity == QLatin1String("maker")) {
                ++maker_liquidity;
                QVERIFY(through > 0.0);   // makers pay the queue/adverse-selection cost
            } else if (liquidity == QLatin1String("taker")) {
                ++taker_liquidity;
                QCOMPARE(through, 0.0);   // takers cross immediately, no through requirement
            }
        }

        QCOMPARE(kinds, (QSet<QString>{QStringLiteral("scalp"), QStringLiteral("swing"),
                                       QStringLiteral("maker")}));
        QCOMPARE(venues, (QSet<QString>{QStringLiteral("coinbase_advanced"), QStringLiteral("kraken_pro"),
                                        QStringLiteral("alpaca")}));
        QCOMPARE(ids.size(), 12);  // content-addressed: no duplicate lanes
        QVERIFY(maker_liquidity > 0);
        QVERIFY(taker_liquidity > 0);

        // The equity venue trades equities; crypto venues trade crypto.
        for (const auto& lane : grid) {
            const QString venue = lane.params.value(QStringLiteral("venue")).toString();
            if (venue == QLatin1String("alpaca"))
                QVERIFY(lane.symbols.contains(QStringLiteral("AAPL")));
            else
                QVERIFY(lane.symbols.contains(QStringLiteral("BTC-USD")));
        }

        // Producer wiring: 7 activatable lanes reuse an existing feed; the rest
        // (maker spread-capture, equity scalp) are deferred (no source).
        int activatable = 0;
        for (const auto& lane : grid) {
            const QString kind = lane.kind;
            const QString venue = lane.params.value(QStringLiteral("venue")).toString();
            const bool crypto = venue != QLatin1String("alpaca");
            const bool has_source = lane.params.contains(QStringLiteral("source"));
            if (has_source)
                ++activatable;

            if (kind == QLatin1String("maker")) {
                QVERIFY(!has_source);  // market-making has no directional producer yet
            } else if (kind == QLatin1String("scalp")) {
                if (crypto)
                    QCOMPARE(lane.params.value(QStringLiteral("source")).toString(),
                             QStringLiteral("scalp_decisions"));
                else
                    QVERIFY(!has_source);  // equity scalp deferred
            } else if (kind == QLatin1String("swing")) {
                QCOMPARE(lane.params.value(QStringLiteral("journal_source")).toString(),
                         crypto ? QStringLiteral("edge crypto-recommend")
                                : QStringLiteral("chronos2-equity-forecast"));
            }
        }
        QCOMPARE(activatable, 7);  // 2 crypto x scalp{maker,taker} + 3 swing
    }
};
QTEST_GUILESS_MAIN(TstSandboxRegistry)
#include "tst_sandbox_registry.moc"
