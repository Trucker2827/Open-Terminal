#pragma once

#include <QString>

#include <cstdint>

namespace openmarketterminal::services::news {

// Outcome of one event-impact scoring pass.  `ok` is true only when the scorer
// produced a well-formed record AND the snapshot was written; on any failure
// `ok` is false, `wrote_snapshot` is false, and `error` carries a diagnostic.
struct BtcEventImpactResult {
    bool ok = false;
    bool wrote_snapshot = false;
    int events = 0;
    QString stdout_json;  // the raw scorer record that was written (compact-ish)
    QString error;        // human-readable failure reason when !ok
};

// Core producer glue shared by the `news bitcoin-event-impact` CLI command and
// the `--selftest-btc-event-impact` one-shot.  It:
//   1. reads `{data_dir}/btc-news-pulse-latest.json` (the pulse snapshot),
//   2. builds a blind stdin payload {as_of_ms, stories:[{headline,
//      published_ts_ms}]} containing ONLY stories whose publish time is
//      > 0 and <= as_of_ms (no look-ahead at the source),
//   3. pipes it to the Python scorer (`<python> <script> score`) via stdin,
//   4. validates the scorer's stdout is a JSON object containing both `events`
//      and `as_of_ms`, and
//   5. on success writes it verbatim to `{data_dir}/btc-event-impact-latest.json`
//      (+ appends `.jsonl`) with the exact byte format the calibrator consumer
//      reads.
//
// Failure isolation: if the scorer times out, exits non-zero, or emits
// malformed JSON, NO snapshot is written and `ok` is false -- the consumer
// keeps its last good file (or reads neutral).
//
// The scorer script is resolved from the `OPENTERMINAL_BTC_EVENT_IMPACT_SCORER`
// environment override when set, otherwise `default_scorer_script`.  `python`
// is the interpreter path.  `as_of_ms` is the caller's "now" (epoch ms).
BtcEventImpactResult run_btc_event_impact(const QString& data_dir,
                                          const QString& python,
                                          const QString& default_scorer_script,
                                          int limit, qint64 as_of_ms);

} // namespace openmarketterminal::services::news
