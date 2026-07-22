// tst_advisory_repository.cpp — edge_advisory_challenge table (migration v067).
//
// DB bring-up mirrors tst_ai_handler_repository.cpp's open_profile_database_for_test():
// select the "default" profile, create its datadir tree, register migrations,
// then open the DB (which runs them). Do not invent a new bootstrap.

#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "services/edge_radar/AdvisoryChallengeRepository.h"
#include "storage/repositories/EdgePredictionModelRepository.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"

using namespace openmarketterminal;

namespace {

bool open_profile_database_for_test() {
    ProfileManager::instance().set_active("default");
    QDir().mkpath(AppPaths::root());
    AppPaths::ensure_all();
    register_all_migrations();
    auto db = Database::instance().open(AppPaths::data() + "/openmarketterminal.db");
    return db.is_ok();
}

} // namespace

class TstAdvisoryRepository : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        static QTemporaryDir home;
        QVERIFY(home.isValid());
        qputenv("HOME", home.path().toUtf8());
        QVERIFY(open_profile_database_for_test());
    }

    void table_exists() {
        auto r = Database::instance().execute(
            "SELECT name FROM sqlite_master WHERE type='table' AND name='edge_advisory_challenge'", {});
        QVERIFY(r.is_ok());
        QVERIFY(r.value().next());
        QCOMPARE(r.value().value(0).toString(), QStringLiteral("edge_advisory_challenge"));
    }

    void recent_price_series_uses_bounded_receipt_index_path() {
        auto& repo = EdgePredictionModelRepository::instance();
        for (int i = 0; i < 3; ++i) {
            EdgePredictionRawTick tick;
            tick.id = QStringLiteral("recent-%1").arg(i);
            tick.symbol = QStringLiteral("BTC");
            tick.source = QStringLiteral("test");
            tick.price = 100.0 + i;
            tick.exchange_ts = 60'000 + i * 10'000;
            tick.received_ts = 100'000 + i;
            QVERIFY(repo.add_raw_tick(tick).is_ok());
        }
        const auto recent = repo.list_recent_price_series_since(
            QStringLiteral("BTC"), 100'000, 10);
        QVERIFY(recent.is_ok());
        QCOMPARE(recent.value().size(), 1);
        QCOMPARE(recent.value().first().price, 101.0);
    }

    void competition_sibling_reuses_exact_immutable_snapshot() {
        adv::AdvisoryChallengeRepository repo;
        adv::OpenParams source;
        source.ticker = "KXPAIR"; source.market_id = "PAIR-M1"; source.horizon = "15m";
        source.blind_context = QJsonObject{{"spot", 66180.0}, {"strike", 66250.0},
                                           {"seconds_left", 320}};
        source.withheld_market = QJsonObject{{"market_implied_probability", 0.44}};
        source.daemon_prob = 0.47; source.seconds_left = 320; source.now_ms = 10'000;
        source.provider = "openai-codex-cli"; source.model = "gpt-5.6-sol";
        source.competition_pair_id = "pair-one";
        auto left = repo.open(source); QVERIFY(left.is_ok());

        adv::OpenParams identity;
        identity.provider = "anthropic-claude-cli"; identity.model = "claude-opus-4-8";
        identity.competition_pair_id = "pair-one";
        auto right = repo.open_sibling(left.value().challenge_id, identity); QVERIFY(right.is_ok());
        QCOMPARE(right.value().context_hash, left.value().context_hash);
        QCOMPARE(right.value().blind_context, left.value().blind_context);
        QCOMPARE(right.value().created_at, left.value().created_at);
        QCOMPARE(right.value().prediction_ttl_at, left.value().prediction_ttl_at);

        auto rows = Database::instance().execute(
            "SELECT provider, context_json, context_hash, market_at_open_json, competition_pair_id "
            "FROM edge_advisory_challenge WHERE competition_pair_id='pair-one' ORDER BY provider", {});
        QVERIFY(rows.is_ok());
        QStringList contexts, markets, providers;
        while (rows.value().next()) {
            providers << rows.value().value(0).toString();
            contexts << rows.value().value(1).toString();
            markets << rows.value().value(3).toString();
            QCOMPARE(rows.value().value(4).toString(), QStringLiteral("pair-one"));
        }
        QCOMPARE(providers.size(), 2);
        QCOMPARE(contexts.at(0), contexts.at(1));
        QCOMPARE(markets.at(0), markets.at(1));
    }

    void competition_pair_has_latency_neutral_ttl() {
        adv::AdvisoryChallengeRepository repo;
        adv::OpenParams p;
        p.ticker = "KXPAIRTTL"; p.market_id = "PAIR-TTL"; p.horizon = "hourly";
        p.blind_context = QJsonObject{{"seconds_left", 1920}};
        p.withheld_market = QJsonObject{{"market_implied_probability", 0.5}};
        p.daemon_prob = 0.5; p.seconds_left = 1920; p.now_ms = 20'000;
        p.provider = "openai-codex-cli"; p.competition_pair_id = "pair-ttl";
        auto paired = repo.open(p); QVERIFY(paired.is_ok());
        QCOMPARE(paired.value().prediction_ttl_at - paired.value().created_at, qint64(100000));

        p.market_id = "SOLO-TTL"; p.now_ms = 30'000; p.competition_pair_id.clear();
        auto solo = repo.open(p); QVERIFY(solo.is_ok());
        QCOMPARE(solo.value().prediction_ttl_at - solo.value().created_at, qint64(45000));
    }

    void open_then_commit_blind_is_idempotent() {
        adv::AdvisoryChallengeRepository repo;
        adv::OpenParams p; p.ticker="KXBTC"; p.market_id="M1"; p.horizon="hourly";
        p.blind_context=QJsonObject{{"strike_floor",60000}}; p.withheld_market=QJsonObject{{"market_implied_probability",0.55}};
        p.daemon_prob=0.55; p.seconds_left=900; p.now_ms=1000; p.model="opus";
        auto o = repo.open(p); QVERIFY(o.is_ok());
        adv::CommitParams c; c.challenge_id=o.value().challenge_id; c.commit_id="K1";
        c.probability=0.62; c.confidence=0.7; c.now_ms=1500;
        auto j1 = repo.commit_blind(c); QVERIFY(j1.is_ok());
        auto j2 = repo.commit_blind(c);                       // retry same commit_id
        QVERIFY(j2.is_ok()); QCOMPARE(j2.value(), j1.value()); // same journal id, no duplicate
        auto n = Database::instance().execute(
            "SELECT COUNT(*) FROM edge_decision_journal WHERE source='llm-advisory'", {});
        QVERIFY(n.is_ok() && n.value().next()); QCOMPARE(n.value().value(0).toInt(), 1);

        // Verify the journal row's *content* was mapped correctly, not just that
        // exactly one row exists -- a count-only assertion would pass even if
        // side/market_probability/model_probability/outcome/gate/call were wrong.
        auto row = Database::instance().execute(
            "SELECT side, market_probability, model_probability, confidence, outcome, gate, call,"
            " venue, symbol, market_id, horizon, features_json"
            " FROM edge_decision_journal WHERE source='llm-advisory'", {});
        QVERIFY(row.is_ok() && row.value().next());
        auto& rq = row.value();
        QCOMPARE(rq.value(0).toString(), QStringLiteral("yes"));              // 0.62 >= 0.55 market@open
        QCOMPARE(rq.value(1).toDouble(), 0.55);                              // market_probability = market@open
        QCOMPARE(rq.value(2).toDouble(), 0.62);                              // model_probability = p_pre
        QCOMPARE(rq.value(3).toDouble(), 0.7);                               // confidence
        QCOMPARE(rq.value(4).toInt(), -1);                                   // outcome (settlement resolves later)
        QCOMPARE(rq.value(5).toString(), QStringLiteral("measurement_only")); // gate
        QCOMPARE(rq.value(6).toString(), QStringLiteral("LLM_ADVISORY"));     // call
        QCOMPARE(rq.value(7).toString(), QStringLiteral("kalshi"));           // venue
        QCOMPARE(rq.value(8).toString(), QStringLiteral("KXBTC"));           // symbol = ticker
        QCOMPARE(rq.value(9).toString(), QStringLiteral("M1"));               // market_id
        QCOMPARE(rq.value(10).toString(), QStringLiteral("hourly"));         // horizon
        const QJsonObject features =
            QJsonDocument::fromJson(rq.value(11).toString().toUtf8()).object();
        QCOMPARE(features.value("p_pre").toDouble(), 0.62);
        QCOMPARE(features.value("market_at_open").toDouble(), 0.55);
        QCOMPARE(features.value("challenge_id").toString(), o.value().challenge_id);
        QCOMPARE(features.value("gate").toString(), QStringLiteral("measurement_only"));
        QCOMPARE(features.value("call").toString(), QStringLiteral("LLM_ADVISORY"));
        QCOMPARE(features.value("execution_eligible").toBool(), false);
        QVERIFY(features.value("ts_blind").toDouble() > features.value("ts_opened").toDouble()); // 1500 > 1000
    }
    void reveal_before_blind_is_rejected() {
        adv::AdvisoryChallengeRepository repo;
        adv::OpenParams p; p.ticker="KXBTC"; p.market_id="M2"; p.seconds_left=900; p.now_ms=1000;
        p.blind_context=QJsonObject{}; p.withheld_market=QJsonObject{}; p.daemon_prob=0.5;
        auto o = repo.open(p); QVERIFY(o.is_ok());
        QVERIFY(repo.reveal(o.value().challenge_id, 1200).is_err());   // still OPEN
    }
    void expired_challenge_cannot_commit() {
        adv::AdvisoryChallengeRepository repo;
        adv::OpenParams p; p.ticker="KXBTC"; p.market_id="M3"; p.seconds_left=180; p.now_ms=1000;
        p.blind_context=QJsonObject{}; p.withheld_market=QJsonObject{}; p.daemon_prob=0.5;
        auto o = repo.open(p); QVERIFY(o.is_ok());
        QCOMPARE(repo.expire_stale(o.value().prediction_ttl_at + 1).value(), 1);
        adv::CommitParams c; c.challenge_id=o.value().challenge_id; c.commit_id="K"; c.probability=0.6; c.now_ms=o.value().prediction_ttl_at + 2;
        QVERIFY(repo.commit_blind(c).is_err());
    }

    void commit_post_finalizes_journal_without_clobbering_pre_fields() {
        adv::AdvisoryChallengeRepository repo;
        adv::OpenParams p; p.ticker="KXBTC"; p.market_id="M4"; p.horizon="hourly";
        p.blind_context=QJsonObject{{"strike_floor",61000}};
        p.withheld_market=QJsonObject{{"market_implied_probability",0.5}};
        p.daemon_prob=0.5; p.seconds_left=900; p.now_ms=1000; p.model="opus";
        auto o = repo.open(p); QVERIFY(o.is_ok());

        adv::CommitParams cb; cb.challenge_id=o.value().challenge_id; cb.commit_id="B1";
        cb.probability=0.6; cb.confidence=0.7; cb.now_ms=1500;
        auto jb = repo.commit_blind(cb); QVERIFY(jb.is_ok());

        // reveal() supplies a fresh reveal-time snapshot, distinct from open.
        const QJsonObject fresh_reveal_market{{"market_implied_probability", 0.58}};
        auto rv = repo.reveal(o.value().challenge_id, 1800, fresh_reveal_market, 0.57);
        QVERIFY(rv.is_ok());

        // commit_post() supplies a fresh post-time snapshot, distinct again.
        adv::CommitParams cp; cp.challenge_id=o.value().challenge_id; cp.commit_id="P1";
        cp.probability=0.53; cp.confidence=0.65; cp.now_ms=2000;
        cp.market_json = QJsonObject{{"market_implied_probability", 0.56}};
        auto post1 = repo.commit_post(cp); QVERIFY(post1.is_ok());

        auto jr = Database::instance().execute(
            "SELECT features_json FROM edge_decision_journal WHERE id=?", {jb.value()});
        QVERIFY(jr.is_ok() && jr.value().next());
        const QString features_text_1 = jr.value().value(0).toString();
        const QJsonObject features = QJsonDocument::fromJson(features_text_1.toUtf8()).object();

        // Post fields finalized with the fresh values supplied.
        QCOMPARE(features.value("p_post").toDouble(), 0.53);
        QCOMPARE(features.value("market_at_post").toDouble(), 0.56);
        QCOMPARE(features.value("ts_post").toDouble(), 2000.0);

        // Immutable pre fields untouched by commit_post.
        QCOMPARE(features.value("p_pre").toDouble(), 0.6);
        QCOMPARE(features.value("market_at_open").toDouble(), 0.5);
        QVERIFY(!features.value("context_hash").toString().isEmpty());
        QVERIFY(!features.value("sealed_hash").toString().isEmpty());
        QCOMPARE(features.value("challenge_id").toString(), o.value().challenge_id);
        QCOMPARE(features.value("authority").toString(), QStringLiteral("advisory_only"));
        QCOMPARE(features.value("gate").toString(), QStringLiteral("measurement_only"));
        QCOMPARE(features.value("call").toString(), QStringLiteral("LLM_ADVISORY"));
        QVERIFY(features.value("forecaster").isObject());

        // Idempotent replay: same commit_id finalizes once, no double-update.
        auto post2 = repo.commit_post(cp); QVERIFY(post2.is_ok());
        auto jr2 = Database::instance().execute(
            "SELECT features_json FROM edge_decision_journal WHERE id=?", {jb.value()});
        QVERIFY(jr2.is_ok() && jr2.value().next());
        QCOMPARE(jr2.value().value(0).toString(), features_text_1); // byte-identical, not rewritten

        // The challenge ledger's reveal/post baselines hold the DISTINCT
        // fresh snapshots supplied at each transition, not a copy of open.
        auto crow = Database::instance().execute(
            "SELECT market_at_reveal_json, market_at_post_json, daemon_prob_at_reveal"
            " FROM edge_advisory_challenge WHERE challenge_id=?",
            {o.value().challenge_id});
        QVERIFY(crow.is_ok() && crow.value().next());
        const QJsonObject reveal_json =
            QJsonDocument::fromJson(crow.value().value(0).toString().toUtf8()).object();
        const QJsonObject post_json =
            QJsonDocument::fromJson(crow.value().value(1).toString().toUtf8()).object();
        QCOMPARE(reveal_json.value("market_implied_probability").toDouble(), 0.58);
        QCOMPARE(post_json.value("market_implied_probability").toDouble(), 0.56);
        QCOMPARE(crow.value().value(2).toDouble(), 0.57); // daemon_prob_at_reveal
    }

    void commit_post_without_fresh_data_leaves_honest_defaults() {
        adv::AdvisoryChallengeRepository repo;
        adv::OpenParams p; p.ticker="KXBTC"; p.market_id="M5"; p.horizon="hourly";
        p.blind_context=QJsonObject{}; p.withheld_market=QJsonObject{{"market_implied_probability",0.5}};
        p.daemon_prob=0.5; p.seconds_left=900; p.now_ms=1000;
        auto o = repo.open(p); QVERIFY(o.is_ok());

        adv::CommitParams cb; cb.challenge_id=o.value().challenge_id; cb.commit_id="B2";
        cb.probability=0.6; cb.now_ms=1500;
        auto jb = repo.commit_blind(cb); QVERIFY(jb.is_ok());

        auto rv = repo.reveal(o.value().challenge_id, 1800); // no fresh data supplied
        QVERIFY(rv.is_ok());

        adv::CommitParams cp; cp.challenge_id=o.value().challenge_id; cp.commit_id="P2";
        cp.probability=0.55; cp.now_ms=2000; // no market_json supplied
        auto post = repo.commit_post(cp); QVERIFY(post.is_ok());

        auto crow = Database::instance().execute(
            "SELECT market_at_blind_json, market_at_reveal_json, market_at_post_json, daemon_prob_at_reveal"
            " FROM edge_advisory_challenge WHERE challenge_id=?",
            {o.value().challenge_id});
        QVERIFY(crow.is_ok() && crow.value().next());
        QCOMPARE(crow.value().value(0).toString(), QStringLiteral("{}")); // honest default, not copied
        QCOMPARE(crow.value().value(1).toString(), QStringLiteral("{}"));
        QCOMPARE(crow.value().value(2).toString(), QStringLiteral("{}"));
        QCOMPARE(crow.value().value(3).toDouble(), -1.0);

        auto jr = Database::instance().execute(
            "SELECT features_json FROM edge_decision_journal WHERE id=?", {jb.value()});
        QVERIFY(jr.is_ok() && jr.value().next());
        const QJsonObject features =
            QJsonDocument::fromJson(jr.value().value(0).toString().toUtf8()).object();
        QVERIFY(!features.contains("market_at_post")); // absent, not fabricated from an earlier snapshot
        QCOMPARE(features.value("p_post").toDouble(), 0.55);
    }

    // Cohort fields (settlement_band / distance_bps): these already exist in
    // the blind context supplied at open() -- kalshi_advise_flatten_snapshot
    // inserts them into the packet under those exact key names -- but
    // commit_blind() was not copying them into features_json, so `advise
    // score` had no per-cohort grouping data available in the (immutable)
    // journal row. This asserts commit_blind() copies both fields into
    // features_json when the blind context carries them, and that
    // commit_post() (which rewrites features_json to add p_post/
    // market_at_post/ts_post) does not clobber them.
    void commit_blind_persists_settlement_band_and_distance_bps_when_present() {
        adv::AdvisoryChallengeRepository repo;
        adv::OpenParams p; p.ticker="KXBTC"; p.market_id="M6"; p.horizon="hourly";
        p.blind_context=QJsonObject{{"settlement_band", "final_5m"}, {"distance_bps", 42.5}};
        p.withheld_market=QJsonObject{{"market_implied_probability",0.5}};
        p.daemon_prob=0.5; p.seconds_left=900; p.now_ms=1000;
        auto o = repo.open(p); QVERIFY(o.is_ok());

        adv::CommitParams cb; cb.challenge_id=o.value().challenge_id; cb.commit_id="B3";
        cb.probability=0.6; cb.now_ms=1500;
        auto jb = repo.commit_blind(cb); QVERIFY(jb.is_ok());

        auto jr = Database::instance().execute(
            "SELECT features_json FROM edge_decision_journal WHERE id=?", {jb.value()});
        QVERIFY(jr.is_ok() && jr.value().next());
        const QJsonObject features_after_blind =
            QJsonDocument::fromJson(jr.value().value(0).toString().toUtf8()).object();
        QCOMPARE(features_after_blind.value("settlement_band").toString(), QStringLiteral("final_5m"));
        QCOMPARE(features_after_blind.value("distance_bps").toDouble(), 42.5);

        auto rv = repo.reveal(o.value().challenge_id, 1800);
        QVERIFY(rv.is_ok());
        adv::CommitParams cp; cp.challenge_id=o.value().challenge_id; cp.commit_id="P3";
        cp.probability=0.55; cp.now_ms=2000;
        cp.market_json = QJsonObject{{"market_implied_probability", 0.52}};
        auto post = repo.commit_post(cp); QVERIFY(post.is_ok());

        auto jr2 = Database::instance().execute(
            "SELECT features_json FROM edge_decision_journal WHERE id=?", {jb.value()});
        QVERIFY(jr2.is_ok() && jr2.value().next());
        const QJsonObject features_after_post =
            QJsonDocument::fromJson(jr2.value().value(0).toString().toUtf8()).object();
        // commit_post() must not clobber the immutable cohort fields set at
        // commit_blind() -- it only merges p_post/market_at_post/ts_post.
        QCOMPARE(features_after_post.value("settlement_band").toString(), QStringLiteral("final_5m"));
        QCOMPARE(features_after_post.value("distance_bps").toDouble(), 42.5);
        QCOMPARE(features_after_post.value("p_post").toDouble(), 0.55); // sanity: post merge still happened
    }

    void commit_blind_omits_settlement_band_and_distance_bps_when_absent() {
        adv::AdvisoryChallengeRepository repo;
        adv::OpenParams p; p.ticker="KXBTC"; p.market_id="M7"; p.horizon="hourly";
        p.blind_context=QJsonObject{{"strike_floor", 61000}}; // no settlement_band/distance_bps
        p.withheld_market=QJsonObject{{"market_implied_probability",0.5}};
        p.daemon_prob=0.5; p.seconds_left=900; p.now_ms=1000;
        auto o = repo.open(p); QVERIFY(o.is_ok());

        adv::CommitParams cb; cb.challenge_id=o.value().challenge_id; cb.commit_id="B4";
        cb.probability=0.6; cb.now_ms=1500;
        auto jb = repo.commit_blind(cb); QVERIFY(jb.is_ok());

        auto jr = Database::instance().execute(
            "SELECT features_json FROM edge_decision_journal WHERE id=?", {jb.value()});
        QVERIFY(jr.is_ok() && jr.value().next());
        const QJsonObject features =
            QJsonDocument::fromJson(jr.value().value(0).toString().toUtf8()).object();
        // Absent from the blind context -> absent from features_json, never
        // fabricated.
        QVERIFY(!features.contains("settlement_band"));
        QVERIFY(!features.contains("distance_bps"));
    }

    // Mirrors edge_resolve_kalshi_decisions_command's SELECT in
    // src/cli/CommandDispatch.cpp. Kept in sync by hand: the full command
    // drives a live Kalshi network fetch (KalshiAdapter::fetch_market) to
    // resolve outcomes, which a unit test cannot exercise deterministically,
    // so this proves the widened SELECT admits 'llm-advisory' rows into the
    // resolvable set instead of driving the whole command.
    void kalshi_settlement_select_widens_to_include_advisory_rows() {
        // Old (pre-Task-5) SELECT: proven RED against this test's advisory
        // row before CommandDispatch.cpp was widened -- see task-5-report.md
        // for the failing run. Kept here only to prove the widening is
        // additive (still excludes what it always excluded).
        static const QString kOldSelect =
            "SELECT id, market_id, side FROM edge_decision_journal "
            "WHERE source IN ('edge journal-kalshi-scan','kalshi auto-plan') "
            "AND (gate='pass' OR source='kalshi auto-plan') "
            "AND outcome=-1 AND market_id<>'' "
            "AND seconds_left>=0 AND created_at + seconds_left * 1000 <= ? "
            "ORDER BY created_at ASC LIMIT ?";
        // Current production SELECT (edge_resolve_kalshi_decisions_command).
        static const QString kNewSelect =
            "SELECT id, market_id, side FROM edge_decision_journal "
            "WHERE source IN ('edge journal-kalshi-scan','kalshi auto-plan','llm-advisory') "
            "AND (gate='pass' OR source='kalshi auto-plan' OR source='llm-advisory') "
            "AND outcome=-1 AND market_id<>'' "
            "AND seconds_left>=0 AND created_at + seconds_left * 1000 <= ? "
            "ORDER BY created_at ASC LIMIT ?";

        // Advisory row: written the same way Task 4's CLI verbs write it.
        adv::AdvisoryChallengeRepository repo;
        adv::OpenParams p; p.ticker="KXTEST"; p.market_id="M_RESOLVE"; p.horizon="hourly";
        p.blind_context=QJsonObject{{"seconds_left", 0}};
        p.withheld_market=QJsonObject{{"market_implied_probability",0.5}};
        p.daemon_prob=0.5; p.seconds_left=900; p.now_ms=1000; p.model="opus";
        auto o = repo.open(p); QVERIFY(o.is_ok());
        adv::CommitParams c; c.challenge_id=o.value().challenge_id; c.commit_id="RESOLVE1";
        c.probability=0.6; c.confidence=0.7; c.now_ms=1500;
        auto jb = repo.commit_blind(c); QVERIFY(jb.is_ok());
        const QString advisory_id = jb.value();

        // Deterministic sibling rows, inserted directly in the same shape the
        // auto-plan/scan writers produce, to prove the widening is additive:
        // a 'kalshi auto-plan' row (gate != 'pass') must still resolve under
        // both queries, and an 'edge journal-kalshi-scan' row with a failing
        // gate must still be excluded from both.
        auto ins1 = Database::instance().execute(
            "INSERT INTO edge_decision_journal (id, created_at, updated_at, market_id, side, gate, source, seconds_left, outcome)"
            " VALUES ('DET_AUTOPLAN', 1500, 1500, 'M_DET', 'yes', 'n/a', 'kalshi auto-plan', 0, -1)", {});
        QVERIFY(ins1.is_ok());
        auto ins2 = Database::instance().execute(
            "INSERT INTO edge_decision_journal (id, created_at, updated_at, market_id, side, gate, source, seconds_left, outcome)"
            " VALUES ('DET_SCAN_FAILGATE', 1500, 1500, 'M_SCAN', 'no', 'fail', 'edge journal-kalshi-scan', 0, -1)", {});
        QVERIFY(ins2.is_ok());

        auto ids_from = [](const QString& sql) {
            QSet<QString> ids;
            auto r = Database::instance().execute(sql, {2000, 500});
            if (r.is_ok()) {
                auto& q = r.value();
                while (q.next()) ids.insert(q.value(0).toString());
            }
            return ids;
        };

        const QSet<QString> old_ids = ids_from(kOldSelect);
        const QSet<QString> new_ids = ids_from(kNewSelect);

        // The advisory row is excluded by the old select, included by the
        // widened one.
        QVERIFY(!old_ids.contains(advisory_id));
        QVERIFY(new_ids.contains(advisory_id));

        // No regression: deterministic sources resolve exactly as before.
        QVERIFY(old_ids.contains(QStringLiteral("DET_AUTOPLAN")));
        QVERIFY(new_ids.contains(QStringLiteral("DET_AUTOPLAN")));
        QVERIFY(!old_ids.contains(QStringLiteral("DET_SCAN_FAILGATE")));
        QVERIFY(!new_ids.contains(QStringLiteral("DET_SCAN_FAILGATE")));
    }
};

QTEST_MAIN(TstAdvisoryRepository)
#include "tst_advisory_repository.moc"
