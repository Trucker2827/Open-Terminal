#pragma once

#include "core/result/Result.h"

#include <QJsonObject>
#include <QString>

namespace openmarketterminal::adv {

// Parameters to open a new firewalled advisory challenge. `blind_context` is
// the price-free packet the forecaster will see (see build_blind_packet());
// `withheld_market` is the contemporaneous market/price baseline that stays
// hidden from the forecaster until reveal(). `daemon_prob` is the daemon's
// own probability at open time, likewise withheld until reveal.
struct OpenParams {
    QString ticker, market_id, horizon, settlement_def;
    QJsonObject blind_context, withheld_market;
    double daemon_prob = 0.0;
    qint64 seconds_left = 0, now_ms = 0;
    QString provider, model, prompt_version, agent_id, run_id;
    double temperature = 0.0;
};

struct OpenResult {
    QString challenge_id, context_hash;
    qint64 prediction_ttl_at = 0, execution_relevance_at = 0;
};

// Parameters shared by commit_blind() and commit_post(). `commit_id` is a
// client-supplied idempotency key: replaying the same (challenge_id,
// commit_id) pair returns the original result without a second write.
struct CommitParams {
    QString challenge_id, commit_id;
    double probability = 0.0, confidence = 0.0;
    QString rationale;
    qint64 now_ms = 0;
};

// DB-backed CRUD + atomic, idempotent state transitions over
// edge_advisory_challenge (migration v067), plus the resolvable advisory
// row written into edge_decision_journal at commit_blind().
//
// State machine: OPEN -> COMMITTED_BLIND -> REVEALED -> COMMITTED_POST,
// with OPEN -> EXPIRED as a side transition once prediction_ttl_at elapses.
// Every write path is wrapped in Database::instance().begin_transaction()/
// commit()/rollback(); read-only guard checks (not-found, wrong-state,
// already-expired-at-read-time) happen BEFORE any transaction is opened, so
// a rejected transition never leaves a stray open transaction on the
// connection for the next call to trip over.
class AdvisoryChallengeRepository {
  public:
    // Inserts state=OPEN, computes context_hash/sealed_hash + nonce.
    Result<OpenResult> open(const OpenParams& params);

    // OPEN -> COMMITTED_BLIND. Writes the resolvable edge_decision_journal
    // row and returns its id. Idempotent on (challenge_id, commit_id).
    Result<QString> commit_blind(const CommitParams& params);

    // COMMITTED_BLIND -> REVEALED. Returns {sealed_hash, withheld_market}.
    Result<QJsonObject> reveal(const QString& challenge_id, qint64 now_ms);

    // REVEALED -> COMMITTED_POST. Finalizes the journal row's features_json
    // (appends p_post/market_at_post/ts_post) without touching pre fields.
    // Idempotent on (challenge_id, commit_id).
    Result<void> commit_post(const CommitParams& params);

    // OPEN rows past prediction_ttl_at -> EXPIRED. Returns rows affected.
    Result<int> expire_stale(qint64 now_ms);
};

} // namespace openmarketterminal::adv
