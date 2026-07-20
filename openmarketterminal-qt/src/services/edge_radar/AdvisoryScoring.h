#pragma once

#include <QString>
#include <QVector>

#include <QtGlobal>

namespace openmarketterminal::adv {

// One resolved prediction row available for paired out-of-sample scoring.
// p_pre  - the model's blind (pre-reveal) probability estimate.
// p_post - the model's post-reveal probability estimate (informational only
//          here; score_paired() scores p_pre against outcome/market/daemon).
// market - the market-implied probability at prediction time.
// daemon - the daemon/baseline model's probability at prediction time.
// outcome - realized binary outcome, 0 or 1.
// cohort  - free-form grouping label (not used by score_paired() math itself,
//           carried through for callers that partition rows by cohort).
struct ScoredRow {
    double p_pre = 0.0;
    double p_post = 0.0;
    double market = 0.0;
    double daemon = 0.0;
    int outcome = 0;
    QString cohort;
};

// Paired out-of-sample scoring result over a set of ScoredRow.
// brier_* are mean squared errors of each probability source against outcome.
// logloss_pre is the clamped log loss of p_pre.
// improvement_vs_market_pre = brier_market - brier_pre (positive => pre beats market).
// improvement_vs_daemon_pre = brier_daemon - brier_pre (positive => pre beats daemon).
// ci_low/ci_high: 2.5/97.5 percentile bootstrap CI on the PAIRED improvement
// of pre vs daemon (improvement_vs_daemon_pre), computed by resampling rows
// with replacement. See score_paired() implementation comment for rationale.
// n: number of rows scored (0 if input was empty; all fields are 0.0 in that
// case, never NaN).
struct PairedResult {
    double brier_pre = 0.0;
    double brier_post = 0.0;
    double brier_market = 0.0;
    double brier_daemon = 0.0;
    double logloss_pre = 0.0;
    double improvement_vs_market_pre = 0.0;
    double improvement_vs_daemon_pre = 0.0;
    double ci_low = 0.0;
    double ci_high = 0.0;
    int n = 0;
};

// Computes paired Brier/log-loss metrics for `rows` and a percentile
// bootstrap confidence interval on improvement_vs_daemon_pre. Deterministic:
// uses std::mt19937(seed) only, never wall-clock/random_device. Pure math,
// no I/O. Returns a zeroed PairedResult (n=0, no NaNs) when rows is empty.
PairedResult score_paired(const QVector<ScoredRow>& rows, int bootstrap_iters = 2000, quint32 seed = 12345);

// Counts of advisory-challenge state-machine outcomes over a list of raw
// state strings (e.g. "OPEN", "COMMITTED_BLIND", "REVEALED",
// "COMMITTED_POST", "EXPIRED", "ABANDONED"). `opened` is simply the total
// number of states passed in (every challenge starts OPEN), regardless of
// which value each string holds.
struct Participation {
    int opened = 0;
    int committed_blind = 0;
    int revealed = 0;
    int committed_post = 0;
    int expired = 0;
    int abandoned = 0;
    double open_to_commit_rate = 0.0;
    double expiration_rate = 0.0;
};

// open_to_commit_rate = (committed_blind + revealed + committed_post) / opened.
// expiration_rate = expired / opened.
// Guards divide-by-zero: an empty `states` yields opened=0 and both rates 0.0.
Participation participation(const QVector<QString>& states);

} // namespace openmarketterminal::adv
