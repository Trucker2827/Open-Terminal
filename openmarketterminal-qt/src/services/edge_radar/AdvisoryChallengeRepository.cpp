#include "services/edge_radar/AdvisoryChallengeRepository.h"

#include "services/edge_radar/AdvisoryProtocol.h"
#include "storage/sqlite/Database.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QUuid>

#include <algorithm>

namespace openmarketterminal::adv {

namespace {

using openmarketterminal::Database;
using openmarketterminal::Result;

// Column order MUST match the CREATE TABLE in
// src/storage/sqlite/migrations/v067_edge_advisory_challenge.cpp exactly —
// the row mapper below reads positionally.
const char* kChallengeCols =
    "challenge_id, state, created_at, prediction_ttl_at, execution_relevance_at,"
    " ticker, market_id, horizon, settlement_def,"
    " context_json, context_hash, sealed_hash, nonce,"
    " market_at_open_json, market_at_blind_json, market_at_reveal_json, market_at_post_json,"
    " daemon_prob_at_open, daemon_prob_at_reveal,"
    " provider, model, prompt_version, context_schema_version, protocol_version,"
    " agent_id, run_id, temperature,"
    " p_pre, p_post, confidence_pre, confidence_post, rationale_pre, rationale_post,"
    " commit_id_blind, commit_id_post, journal_id,"
    " ts_blind, ts_reveal, ts_post";

struct ChallengeRow {
    QString challenge_id, state;
    qint64 created_at = 0, prediction_ttl_at = 0, execution_relevance_at = 0;
    QString ticker, market_id, horizon, settlement_def;
    QString context_json, context_hash, sealed_hash, nonce;
    QString market_at_open_json, market_at_blind_json, market_at_reveal_json, market_at_post_json;
    double daemon_prob_at_open = -1, daemon_prob_at_reveal = -1;
    QString provider, model, prompt_version;
    int context_schema_version = 1, protocol_version = 1;
    QString agent_id, run_id;
    double temperature = -1;
    double p_pre = -1, p_post = -1, confidence_pre = -1, confidence_post = -1;
    QString rationale_pre, rationale_post;
    QString commit_id_blind, commit_id_post, journal_id;
    qint64 ts_blind = 0, ts_reveal = 0, ts_post = 0;
};

ChallengeRow map_challenge_row(QSqlQuery& q) {
    ChallengeRow r;
    int i = 0;
    r.challenge_id = q.value(i++).toString();
    r.state = q.value(i++).toString();
    r.created_at = q.value(i++).toLongLong();
    r.prediction_ttl_at = q.value(i++).toLongLong();
    r.execution_relevance_at = q.value(i++).toLongLong();
    r.ticker = q.value(i++).toString();
    r.market_id = q.value(i++).toString();
    r.horizon = q.value(i++).toString();
    r.settlement_def = q.value(i++).toString();
    r.context_json = q.value(i++).toString();
    r.context_hash = q.value(i++).toString();
    r.sealed_hash = q.value(i++).toString();
    r.nonce = q.value(i++).toString();
    r.market_at_open_json = q.value(i++).toString();
    r.market_at_blind_json = q.value(i++).toString();
    r.market_at_reveal_json = q.value(i++).toString();
    r.market_at_post_json = q.value(i++).toString();
    r.daemon_prob_at_open = q.value(i++).toDouble();
    r.daemon_prob_at_reveal = q.value(i++).toDouble();
    r.provider = q.value(i++).toString();
    r.model = q.value(i++).toString();
    r.prompt_version = q.value(i++).toString();
    r.context_schema_version = q.value(i++).toInt();
    r.protocol_version = q.value(i++).toInt();
    r.agent_id = q.value(i++).toString();
    r.run_id = q.value(i++).toString();
    r.temperature = q.value(i++).toDouble();
    r.p_pre = q.value(i++).toDouble();
    r.p_post = q.value(i++).toDouble();
    r.confidence_pre = q.value(i++).toDouble();
    r.confidence_post = q.value(i++).toDouble();
    r.rationale_pre = q.value(i++).toString();
    r.rationale_post = q.value(i++).toString();
    r.commit_id_blind = q.value(i++).toString();
    r.commit_id_post = q.value(i++).toString();
    r.journal_id = q.value(i++).toString();
    r.ts_blind = q.value(i++).toLongLong();
    r.ts_reveal = q.value(i++).toLongLong();
    r.ts_post = q.value(i++).toLongLong();
    return r;
}

QString nn(const QString& s) { return s.isNull() ? QStringLiteral("") : s; }

QJsonObject parse_object(const QString& json) {
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

QString to_json_text(const QJsonObject& obj) {
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// The withheld market baseline's headline probability field (per
// build_blind_packet()'s forbidden-key list / the design doc's
// market_implied_probability). Missing/absent -> 0.0.
double market_probability_of(const QJsonObject& market_snapshot) {
    return market_snapshot.value("market_implied_probability").toDouble(0.0);
}

// Reads a single challenge row by id. Not found -> Result::err. This is a
// plain read with no transaction — every guard/state check runs on this
// result BEFORE any write path opens a transaction, so a rejected
// transition never leaves a stray open transaction behind.
Result<ChallengeRow> read_challenge(const QString& challenge_id) {
    auto r = Database::instance().execute(
        QStringLiteral("SELECT %1 FROM edge_advisory_challenge WHERE challenge_id=?").arg(kChallengeCols),
        {challenge_id});
    if (r.is_err())
        return Result<ChallengeRow>::err(r.error());
    auto& q = r.value();
    if (!q.next())
        return Result<ChallengeRow>::err("challenge not found: " + challenge_id.toStdString());
    return Result<ChallengeRow>::ok(map_challenge_row(q));
}

QJsonObject reveal_result(const ChallengeRow& row) {
    return QJsonObject{{"sealed_hash", row.sealed_hash},
                       {"withheld_market", parse_object(row.market_at_open_json)}};
}

} // namespace

Result<OpenResult> AdvisoryChallengeRepository::open(const OpenParams& p) {
    const TtlPolicy policy = ttl_for(p.seconds_left);
    if (!policy.may_open)
        return Result<OpenResult>::err("too close to settlement to open an advisory challenge");

    const QString challenge_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString context_hash = sha256_hex(canonical_json(p.blind_context));

    // sealed_hash binds the blind context, the withheld market baseline, and
    // forecaster/metadata identity + nonce together — any later mutation of
    // the stored context or withheld baseline is detectable by recomputing
    // this hash. Computed now (while we still have the withheld data) but
    // only surfaced to callers at reveal(), never at open().
    QJsonObject metadata{{"ticker", p.ticker},
                        {"market_id", p.market_id},
                        {"horizon", p.horizon},
                        {"settlement_def", p.settlement_def},
                        {"provider", p.provider},
                        {"model", p.model},
                        {"prompt_version", p.prompt_version},
                        {"agent_id", p.agent_id},
                        {"run_id", p.run_id},
                        {"temperature", p.temperature}};
    QJsonObject sealed_source{{"context", p.blind_context},
                              {"withheld_market", p.withheld_market},
                              {"metadata", metadata},
                              {"nonce", nonce}};
    const QString sealed_hash = sha256_hex(canonical_json(sealed_source));

    const qint64 prediction_ttl_at = p.now_ms + policy.prediction_ttl_ms;
    const qint64 execution_relevance_at = p.now_ms + policy.execution_relevance_ms;

    auto& db = Database::instance();
    auto bt = db.begin_transaction();
    if (bt.is_err())
        return Result<OpenResult>::err(bt.error());

    auto ins = db.execute(
        "INSERT INTO edge_advisory_challenge ("
        " challenge_id, state, created_at, prediction_ttl_at, execution_relevance_at,"
        " ticker, market_id, horizon, settlement_def,"
        " context_json, context_hash, sealed_hash, nonce,"
        " market_at_open_json, daemon_prob_at_open,"
        " provider, model, prompt_version, agent_id, run_id, temperature)"
        " VALUES (?, 'OPEN', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        {challenge_id, p.now_ms, prediction_ttl_at, execution_relevance_at,
         nn(p.ticker), nn(p.market_id), nn(p.horizon), nn(p.settlement_def),
         QString::fromUtf8(canonical_json(p.blind_context)), context_hash, sealed_hash, nonce,
         to_json_text(p.withheld_market), p.daemon_prob,
         nn(p.provider), nn(p.model), nn(p.prompt_version), nn(p.agent_id), nn(p.run_id), p.temperature});
    if (ins.is_err()) {
        db.rollback();
        return Result<OpenResult>::err(ins.error());
    }

    auto ct = db.commit();
    if (ct.is_err()) {
        db.rollback();
        return Result<OpenResult>::err(ct.error());
    }

    OpenResult out;
    out.challenge_id = challenge_id;
    out.context_hash = context_hash;
    out.prediction_ttl_at = prediction_ttl_at;
    out.execution_relevance_at = execution_relevance_at;
    return Result<OpenResult>::ok(out);
}

Result<QString> AdvisoryChallengeRepository::commit_blind(const CommitParams& cp) {
    auto rr = read_challenge(cp.challenge_id);
    if (rr.is_err())
        return Result<QString>::err(rr.error());
    const ChallengeRow row = rr.value();

    // Idempotent replay: same commit_id already recorded -> return the
    // original journal id, no second write.
    if (!cp.commit_id.trimmed().isEmpty() && row.commit_id_blind == cp.commit_id)
        return Result<QString>::ok(row.journal_id);

    if (row.state != QStringLiteral("OPEN"))
        return Result<QString>::err("commit_blind rejected: challenge is not OPEN (state=" +
                                    row.state.toStdString() + ")");

    auto& db = Database::instance();

    // Lazily discovered expiry: mark EXPIRED and reject, rather than
    // silently no-op-ing. This is its own single-write transaction.
    if (cp.now_ms > row.prediction_ttl_at) {
        auto bt = db.begin_transaction();
        if (bt.is_err())
            return Result<QString>::err(bt.error());
        auto upd = db.execute("UPDATE edge_advisory_challenge SET state='EXPIRED' WHERE challenge_id=?",
                              {cp.challenge_id});
        if (upd.is_err()) {
            db.rollback();
            return Result<QString>::err(upd.error());
        }
        auto ct = db.commit();
        if (ct.is_err()) {
            db.rollback();
            return Result<QString>::err(ct.error());
        }
        return Result<QString>::err("commit_blind rejected: challenge expired");
    }

    if (cp.probability < 0.0 || cp.probability > 1.0)
        return Result<QString>::err("commit_blind rejected: probability out of [0,1]");

    const double market_at_open = market_probability_of(parse_object(row.market_at_open_json));
    const QString side = cp.probability >= market_at_open ? QStringLiteral("yes") : QStringLiteral("no");
    const QString journal_id = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // seconds_left has no dedicated challenge column; recover it from the
    // blind context if the caller's packet carried it (build_blind_packet's
    // allowlist includes "seconds_left"), else default 0. No given test
    // exercises this value.
    const QJsonObject blind_context = parse_object(row.context_json);
    const int seconds_left = blind_context.value("seconds_left").toInt(0);

    QJsonObject forecaster{{"provider", row.provider},
                          {"model", row.model},
                          {"prompt_version", row.prompt_version},
                          {"temperature", row.temperature},
                          {"agent_id", row.agent_id},
                          {"run_id", row.run_id}};
    QJsonObject features{{"model_version", "llm-advisory-v1"},
                        {"protocol_version", row.protocol_version},
                        {"context_schema_version", row.context_schema_version},
                        {"challenge_id", row.challenge_id},
                        {"context_hash", row.context_hash},
                        {"sealed_hash", row.sealed_hash},
                        {"ts_opened", row.created_at},
                        {"ts_blind", cp.now_ms},
                        {"ts_reveal", 0},
                        {"ts_post", 0},
                        {"horizon", row.horizon},
                        {"settlement_def", row.settlement_def},
                        {"p_pre", cp.probability},
                        {"p_post", -1},
                        {"market_at_open", market_at_open},
                        {"market_at_post", -1},
                        {"daemon_probability", row.daemon_prob_at_open},
                        {"forecaster", forecaster},
                        {"authority", "advisory_only"},
                        {"execution_eligible", false},
                        {"gate", "measurement_only"},
                        {"call", "LLM_ADVISORY"}};

    // Cohort fields for advise-score grouping. Sourced from the blind
    // context (kalshi_advise_flatten_snapshot in CommandDispatch.cpp inserts
    // both under these exact key names) and copied here ONLY when present --
    // features_json is immutable once written, so a challenge opened without
    // them can never be re-cohorted; never fabricate a value it didn't carry.
    if (blind_context.contains(QStringLiteral("settlement_band")))
        features.insert(QStringLiteral("settlement_band"), blind_context.value(QStringLiteral("settlement_band")));
    if (blind_context.contains(QStringLiteral("distance_bps")))
        features.insert(QStringLiteral("distance_bps"), blind_context.value(QStringLiteral("distance_bps")));

    auto bt = db.begin_transaction();
    if (bt.is_err())
        return Result<QString>::err(bt.error());

    auto ins = db.execute(
        "INSERT INTO edge_decision_journal (id, created_at, updated_at, venue, symbol, horizon,"
        " market_id, side, call, gate, market_probability, model_probability, confidence,"
        " seconds_left, features_json, source, outcome)"
        " VALUES (?, ?, ?, 'kalshi', ?, ?, ?, ?, 'LLM_ADVISORY', 'measurement_only', ?, ?, ?, ?, ?, 'llm-advisory', -1)",
        {journal_id, cp.now_ms, cp.now_ms, nn(row.ticker), nn(row.horizon), nn(row.market_id),
         side, market_at_open, cp.probability, cp.confidence, seconds_left, to_json_text(features)});
    if (ins.is_err()) {
        db.rollback();
        return Result<QString>::err(ins.error());
    }

    // market_at_blind_json is left at its honest table default ('{}') here:
    // no fresh blind-time market snapshot distinct from the open-time one is
    // available through this interface, so we do not fabricate a copy.
    auto upd = db.execute(
        "UPDATE edge_advisory_challenge SET state='COMMITTED_BLIND', commit_id_blind=?,"
        " p_pre=?, confidence_pre=?, rationale_pre=?, ts_blind=?, journal_id=?"
        " WHERE challenge_id=?",
        {cp.commit_id, cp.probability, cp.confidence, nn(cp.rationale), cp.now_ms, journal_id,
         cp.challenge_id});
    if (upd.is_err()) {
        db.rollback();
        return Result<QString>::err(upd.error());
    }

    auto ct = db.commit();
    if (ct.is_err()) {
        db.rollback();
        return Result<QString>::err(ct.error());
    }
    return Result<QString>::ok(journal_id);
}

Result<QJsonObject> AdvisoryChallengeRepository::reveal(const QString& challenge_id, qint64 now_ms,
                                                        const QJsonObject& fresh_market_json,
                                                        double fresh_daemon_prob) {
    auto rr = read_challenge(challenge_id);
    if (rr.is_err())
        return Result<QJsonObject>::err(rr.error());
    const ChallengeRow row = rr.value();

    if (row.state == QStringLiteral("REVEALED") || row.state == QStringLiteral("COMMITTED_POST"))
        return Result<QJsonObject>::ok(reveal_result(row)); // idempotent replay, no write

    if (row.state != QStringLiteral("COMMITTED_BLIND"))
        return Result<QJsonObject>::err("reveal rejected: challenge is not COMMITTED_BLIND (state=" +
                                        row.state.toStdString() + ")");

    auto& db = Database::instance();
    auto bt = db.begin_transaction();
    if (bt.is_err())
        return Result<QJsonObject>::err(bt.error());

    // market_at_reveal_json/daemon_prob_at_reveal are meant to hold a DISTINCT
    // reveal-time snapshot, not a copy of the open-time baseline. Only write
    // them when the caller actually supplied fresh data; otherwise leave the
    // honest table defaults ('{}' / -1) rather than fabricating a
    // fresh-looking value. Built as a dynamic SET clause rather than nested
    // ternaries for clarity.
    const bool has_fresh_market = !fresh_market_json.isEmpty();
    const bool has_fresh_daemon = fresh_daemon_prob >= 0.0;
    QString sql = QStringLiteral("UPDATE edge_advisory_challenge SET state='REVEALED', ts_reveal=?");
    QVariantList params{now_ms};
    if (has_fresh_market) {
        sql += QStringLiteral(", market_at_reveal_json=?");
        params << to_json_text(fresh_market_json);
    }
    if (has_fresh_daemon) {
        sql += QStringLiteral(", daemon_prob_at_reveal=?");
        params << fresh_daemon_prob;
    }
    sql += QStringLiteral(" WHERE challenge_id=?");
    params << challenge_id;

    auto upd = db.execute(sql, params);
    if (upd.is_err()) {
        db.rollback();
        return Result<QJsonObject>::err(upd.error());
    }

    auto ct = db.commit();
    if (ct.is_err()) {
        db.rollback();
        return Result<QJsonObject>::err(ct.error());
    }

    // reveal_result() surfaces the withheld market baseline captured AT OPEN
    // (row.market_at_open_json) — that's the data that was hidden from the
    // forecaster during the blind phase and is being revealed to it now.
    // fresh_market_json/fresh_daemon_prob above are a SEPARATE internal
    // ledger concern (the reveal-time snapshot for later scoring), not part
    // of what's handed back to the caller here.
    return Result<QJsonObject>::ok(reveal_result(row));
}

Result<void> AdvisoryChallengeRepository::commit_post(const CommitParams& cp) {
    auto rr = read_challenge(cp.challenge_id);
    if (rr.is_err())
        return Result<void>::err(rr.error());
    const ChallengeRow row = rr.value();

    if (!cp.commit_id.trimmed().isEmpty() && row.commit_id_post == cp.commit_id)
        return Result<void>::ok(); // idempotent replay, no second write

    if (row.state != QStringLiteral("REVEALED"))
        return Result<void>::err("commit_post rejected: challenge is not REVEALED (state=" +
                                 row.state.toStdString() + ")");

    if (cp.probability < 0.0 || cp.probability > 1.0)
        return Result<void>::err("commit_post rejected: probability out of [0,1]");

    if (row.journal_id.trimmed().isEmpty())
        return Result<void>::err("commit_post rejected: no journal row recorded at commit_blind");

    auto& db = Database::instance();

    auto jr = db.execute("SELECT features_json FROM edge_decision_journal WHERE id=?", {row.journal_id});
    if (jr.is_err())
        return Result<void>::err(jr.error());
    if (!jr.value().next())
        return Result<void>::err("commit_post rejected: journal row not found: " +
                                 row.journal_id.toStdString());
    QJsonObject features = parse_object(jr.value().value(0).toString());

    // market_at_post_json / features_json.market_at_post are only populated
    // from a genuinely fresh post-time snapshot supplied by the caller
    // (cp.market_json). Absent that, both are left at their honest defaults
    // ('{}' on the table; the features_json key is omitted rather than
    // fabricated from an earlier snapshot). Pre fields (p_pre, market_at_open,
    // ts_blind, challenge/context/sealed hashes, forecaster,
    // authority/gate/call) are left untouched below.
    const bool has_fresh_market = !cp.market_json.isEmpty();
    QString market_at_post_json;
    if (has_fresh_market) {
        market_at_post_json = to_json_text(cp.market_json);
        features["market_at_post"] = market_probability_of(cp.market_json);
    } else {
        features.remove("market_at_post");
    }
    features["p_post"] = cp.probability;
    features["ts_post"] = cp.now_ms;

    auto bt = db.begin_transaction();
    if (bt.is_err())
        return Result<void>::err(bt.error());

    auto upd_journal = db.execute(
        "UPDATE edge_decision_journal SET features_json=?, updated_at=? WHERE id=?",
        {to_json_text(features), cp.now_ms, row.journal_id});
    if (upd_journal.is_err()) {
        db.rollback();
        return Result<void>::err(upd_journal.error());
    }

    QString sql = QStringLiteral(
        "UPDATE edge_advisory_challenge SET state='COMMITTED_POST', commit_id_post=?,"
        " p_post=?, confidence_post=?, rationale_post=?, ts_post=?");
    QVariantList params{cp.commit_id, cp.probability, cp.confidence, nn(cp.rationale), cp.now_ms};
    if (has_fresh_market) {
        sql += QStringLiteral(", market_at_post_json=?");
        params << market_at_post_json;
    }
    sql += QStringLiteral(" WHERE challenge_id=?");
    params << cp.challenge_id;

    auto upd_challenge = db.execute(sql, params);
    if (upd_challenge.is_err()) {
        db.rollback();
        return Result<void>::err(upd_challenge.error());
    }

    auto ct = db.commit();
    if (ct.is_err()) {
        db.rollback();
        return Result<void>::err(ct.error());
    }
    return Result<void>::ok();
}

Result<int> AdvisoryChallengeRepository::expire_stale(qint64 now_ms) {
    auto& db = Database::instance();
    auto bt = db.begin_transaction();
    if (bt.is_err())
        return Result<int>::err(bt.error());

    auto upd = db.execute(
        "UPDATE edge_advisory_challenge SET state='EXPIRED' WHERE state='OPEN' AND prediction_ttl_at < ?",
        {now_ms});
    if (upd.is_err()) {
        db.rollback();
        return Result<int>::err(upd.error());
    }
    const int affected = upd.value().numRowsAffected();

    auto ct = db.commit();
    if (ct.is_err()) {
        db.rollback();
        return Result<int>::err(ct.error());
    }
    return Result<int>::ok(affected);
}

} // namespace openmarketterminal::adv
