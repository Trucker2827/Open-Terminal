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

    // (e) seed_default_strategies is idempotent -> 10 rows both times, with
    // separate Chronos BTC books for 5m/15m/1h/1d plus the equity book.
    void seed_default_strategies_is_idempotent() {
        auto first = seed_default_strategies();
        QVERIFY2(first.is_ok(), first.is_err() ? first.error().c_str() : "");
        QCOMPARE(first.value().size(), 10);

        auto second = seed_default_strategies();
        QVERIFY2(second.is_ok(), second.is_err() ? second.error().c_str() : "");
        QCOMPARE(second.value().size(), 10);
        QCOMPARE(second.value(), first.value());

        auto rows = list_strategies();
        QVERIFY(rows.is_ok());
        QSet<QString> seed_ids(first.value().begin(), first.value().end());
        QSet<QString> kinds;
        for (const auto& row : rows.value())
            if (seed_ids.contains(row.strategy_id))
                kinds.insert(row.kind);
        QCOMPARE(kinds, QSet<QString>({"scalp", "spot", "btc5m", "kalshi", "long_short",
                                       "chronos2_5m", "chronos2", "chronos2_1h",
                                       "chronos2_1d", "chronos2_equity"}));

        int seed_row_count = 0;
        for (const auto& row : rows.value())
            if (seed_ids.contains(row.strategy_id))
                ++seed_row_count;
        QCOMPARE(seed_row_count, 10);
    }

    // Contract test (task-11 follow-up): the kalshi season-1 seed shipped
    // with journal_source "kalshi", but the ONLY kalshi journal producer
    // writes source='edge journal-kalshi-scan' (CommandDispatch.cpp:19481) --
    // a string mismatch the executor's exact `WHERE source = ?` match never
    // surfaces as an error, it just silently opens nothing, forever. Both the
    // seed and the (now-corrected) plan doc agreed on the wrong string, so no
    // single-file review caught it. Pin every non-scalp seed's journal_source
    // against its producer's actual source string so a future seed/producer
    // drift fails loudly here instead of shipping a dead book.
    //
    // scalp is intentionally excluded: it reads scalp_decisions.jsonl, not
    // edge_decision_journal, so it has no journal_source/producer pair.
    void seed_journal_sources_match_their_journal_producers() {
        auto seeded = seed_default_strategies();
        QVERIFY2(seeded.is_ok(), seeded.is_err() ? seeded.error().c_str() : "");

        auto rows = list_strategies();
        QVERIFY(rows.is_ok());

        QHash<QString, QString> journal_source_by_kind;
        for (const auto& row : rows.value()) {
            QJsonObject params = QJsonDocument::fromJson(row.params_json.toUtf8()).object();
            if (params.contains(QStringLiteral("journal_source")))
                journal_source_by_kind[row.kind] = params.value(QStringLiteral("journal_source")).toString();
        }

        // Expected producer source strings, one per non-scalp seed kind --
        // hardcoded here (rather than derived) so this test can only pass if
        // the seed's journal_source is byte-identical to what the producer
        // actually writes on the CommandDispatch side.
        QCOMPARE(journal_source_by_kind.value(QStringLiteral("spot")),
                 QStringLiteral("edge crypto-recommend")); // producer @ CommandDispatch.cpp ~13595
        QCOMPARE(journal_source_by_kind.value(QStringLiteral("btc5m")),
                 QStringLiteral("edge journal-evaluate-btc5m-live")); // producer @ CommandDispatch.cpp ~14512
        QCOMPARE(journal_source_by_kind.value(QStringLiteral("kalshi")),
                 QStringLiteral("edge journal-kalshi-scan")); // producer @ CommandDispatch.cpp ~19481
        QCOMPARE(journal_source_by_kind.value(QStringLiteral("long_short")),
                 QStringLiteral("edge long-short-strategy")); // producer @ CommandDispatch.cpp ~13824
        QCOMPARE(journal_source_by_kind.value(QStringLiteral("chronos2_5m")),
                 QStringLiteral("chronos2-forecast"));
        QCOMPARE(journal_source_by_kind.value(QStringLiteral("chronos2")),
                 QStringLiteral("chronos2-forecast"));
        QCOMPARE(journal_source_by_kind.value(QStringLiteral("chronos2_1h")),
                 QStringLiteral("chronos2-forecast"));
        QCOMPARE(journal_source_by_kind.value(QStringLiteral("chronos2_1d")),
                 QStringLiteral("chronos2-forecast"));
        QCOMPARE(journal_source_by_kind.value(QStringLiteral("chronos2_equity")),
                 QStringLiteral("chronos2-equity-forecast"));
    }
};
QTEST_GUILESS_MAIN(TstSandboxRegistry)
#include "tst_sandbox_registry.moc"
