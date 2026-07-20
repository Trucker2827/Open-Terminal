#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace openmarketterminal::adv {

// Deterministic, compact, sorted-key JSON serialization: equal objects with
// differently-ordered keys (or nested differently-ordered keys) produce
// byte-identical output, so hashing over this text is stable across
// re-serialization. Doubles are formatted with a fixed 6-decimal-place
// representation to avoid platform/locale float-formatting drift.
QByteArray canonical_json(const QJsonObject& obj);

// Hex-encoded SHA-256 digest of the given bytes.
QString sha256_hex(const QByteArray& bytes);

// Horizon-aware TTL policy: how long a prediction stays valid and whether
// the advisory challenge may even be opened, given how many seconds remain
// until settlement.
struct TtlPolicy {
    qint64 prediction_ttl_ms = 0;
    qint64 execution_relevance_ms = 0;
    bool may_open = true;
};

// seconds_left <= 60: may_open = false (too close to settlement to open a
// new advisory challenge). configured_max_ms caps prediction_ttl_ms.
TtlPolicy ttl_for(qint64 seconds_left, qint64 configured_max_ms = 60000);

// Reveal-only field names: these must NEVER appear in a blind packet — they
// reveal market pricing / model conclusions that would let a forecaster
// reverse-engineer the answer before it commits.
QStringList kBlindForbiddenKeys();

// Builds a blind packet by copying ONLY the allowlisted keys present in
// `snapshot`. Never implemented by removing forbidden keys from a copy of
// the snapshot — a new snapshot field must never leak by default.
QJsonObject build_blind_packet(const QJsonObject& snapshot);

} // namespace openmarketterminal::adv
