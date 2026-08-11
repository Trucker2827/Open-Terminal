#include "services/prediction/kalshi/KalshiBotGate.h"

#include "services/prediction/kalshi/KalshiBotLive.h"
#include "services/prediction/kalshi/KalshiBotGateStats.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QTimeZone>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <vector>

namespace openmarketterminal::services::prediction::kalshi_ns {

namespace {

constexpr auto kSealField = "seal_sha256";
constexpr auto kParamsEvent = "kalshi_bot_gate_params";
constexpr auto kVerdictEvent = "kalshi_bot_gate";
/// Where a refusal keeps the truncation anchor alive across publications.
constexpr auto kAnchorField = "record_anchor_first_settled_ts_ms";

/// Money is reported in whole cents, like the rest of the Kalshi ledger.
double round_cents(double dollars) { return std::round(dollars * 100.0) / 100.0; }

QString iso(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

bool is_number(const QJsonValue& value) { return value.isDouble(); }

/// A criterion row: same shape for all four so a viewer can render them
/// uniformly. Numbers are inserted by the caller only once measured.
QJsonObject criterion(const char* id, const QString& description, bool met) {
    return QJsonObject{{QStringLiteral("id"), QString::fromLatin1(id)},
                       {QStringLiteral("description"), description},
                       {QStringLiteral("met"), met}};
}

QJsonObject refusal(const char* verdict, const QString& reason, qint64 now_ms,
                    qint64 carried_anchor_ms = 0) {
    // A refusal carries NO criteria and NO ledger numbers: the gate did not
    // evaluate, and an empty criteria array must not read as "nothing passed".
    QJsonObject out{{QStringLiteral("schema"), 1},
                    {QStringLiteral("event"), QString::fromLatin1(kVerdictEvent)},
                    {QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
                    {QStringLiteral("ts"), iso(now_ms)},
                    {QStringLiteral("mode"), QStringLiteral("paper")},
                    {QStringLiteral("verdict"), QString::fromLatin1(verdict)},
                    {QStringLiteral("evaluated"), false},
                    {QStringLiteral("reason"), reason}};
    // The one thing a refusal DOES carry: the truncation anchor it was handed.
    // This refusal is about to be written over the verdict file it came from,
    // and an anchor that only lived in a scored `ledger` block would be erased
    // by the first refusal — after which the next run would see no anchor and
    // score the remainder of a record it had already refused. It is not a
    // number about the current record; it is the earlier verdict's, echoed.
    if (carried_anchor_ms > 0)
        out.insert(QString::fromLatin1(kAnchorField), static_cast<double>(carried_anchor_ms));
    return out;
}

} // namespace

QString KalshiBotGate::seal(const QJsonObject& record) {
    QJsonObject sealed = record;
    sealed.remove(QString::fromLatin1(kSealField));
    // Qt emits object keys in sorted order, so compact JSON is canonical.
    const QByteArray canonical = QJsonDocument(sealed).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
}

bool KalshiBotGate::seal_valid(const QJsonObject& record) {
    const QJsonValue stored = record.value(QString::fromLatin1(kSealField));
    return stored.isString() && !stored.toString().isEmpty() && stored.toString() == seal(record);
}

QJsonObject KalshiBotGate::parse_params(const QJsonObject& raw, QString* error) {
    const auto fail = [&error](const QString& message) {
        if (error) *error = message;
        return QJsonObject();
    };

    // Unknown keys are refused rather than ignored: the params file is the
    // only input to the verdict, so it must not carry anything the gate does
    // not understand — an "override" key must be a hard error, not a no-op.
    static const QSet<QString> allowed = {QStringLiteral("min_settled_bids"),
                                          QStringLiteral("min_net_pnl_usd"),
                                          QStringLiteral("max_drawdown_usd"),
                                          QStringLiteral("min_brier_margin"),
                                          QStringLiteral("families"),
                                          QStringLiteral("pnl_confidence")};
    QStringList unknown;
    for (auto it = raw.constBegin(); it != raw.constEnd(); ++it)
        if (!allowed.contains(it.key())) unknown.append(it.key());
    if (!unknown.isEmpty()) {
        unknown.sort();
        QStringList known = allowed.values();
        known.sort();
        return fail(QStringLiteral("unknown gate params [%1]; allowed: [%2]")
                        .arg(unknown.join(QStringLiteral(", ")), known.join(QStringLiteral(", "))));
    }

    const QJsonValue settled = raw.value(QStringLiteral("min_settled_bids"));
    if (!is_number(settled) || settled.toDouble() != std::floor(settled.toDouble()))
        return fail(QStringLiteral("gate param 'min_settled_bids' is required and must be an integer"));
    const int min_settled = settled.toInt();
    if (min_settled < kMinSettledFloor)
        return fail(QStringLiteral("gate param 'min_settled_bids' must be >= %1 (the ladder's floor); "
                                   "a gate may be tightened, never loosened")
                        .arg(kMinSettledFloor));

    const QJsonValue drawdown = raw.value(QStringLiteral("max_drawdown_usd"));
    if (!is_number(drawdown))
        return fail(QStringLiteral("gate param 'max_drawdown_usd' is required and must be a number"));
    const double max_drawdown = drawdown.toDouble();
    if (!(max_drawdown > 0.0) || max_drawdown > kMaxDrawdownCeiling)
        return fail(QStringLiteral("gate param 'max_drawdown_usd' must be in (0, %1]")
                        .arg(kMaxDrawdownCeiling, 0, 'f', 2));

    double min_net_pnl = kMinNetPnlFloor;
    const QJsonValue pnl = raw.value(QStringLiteral("min_net_pnl_usd"));
    if (!pnl.isUndefined()) {
        if (!is_number(pnl) || pnl.toDouble() < kMinNetPnlFloor)
            return fail(QStringLiteral("gate param 'min_net_pnl_usd' must be a number >= %1")
                            .arg(kMinNetPnlFloor, 0, 'f', 2));
        min_net_pnl = pnl.toDouble();
    }

    double margin = 0.0;
    const QJsonValue brier_margin = raw.value(QStringLiteral("min_brier_margin"));
    if (!brier_margin.isUndefined()) {
        if (!is_number(brier_margin) || brier_margin.toDouble() < 0.0 ||
            brier_margin.toDouble() > kMaxBrierMargin)
            return fail(QStringLiteral("gate param 'min_brier_margin' must be a number in [0, %1]")
                            .arg(kMaxBrierMargin, 0, 'f', 2));
        margin = brier_margin.toDouble();
    }

    // Which families may earn a per-family PASS. Preregistered like every other
    // criterion, and for the same reason: without it, pointing the bot at a new
    // series would mint a fresh hypothesis after the fact, and N families each
    // getting an independent shot at the same thresholds inflates the chance
    // one passes by luck. Absent means NO family is eligible — fail-closed, so
    // per-family admission is something an operator preregisters deliberately
    // rather than something that switches on the moment this code ships.
    double pnl_confidence = kMinPnlConfidence;
    const QJsonValue confidence_raw = raw.value(QStringLiteral("pnl_confidence"));
    if (!confidence_raw.isUndefined()) {
        if (!is_number(confidence_raw) || confidence_raw.toDouble() < kMinPnlConfidence ||
            confidence_raw.toDouble() >= 1.0)
            return fail(QStringLiteral("gate param 'pnl_confidence' must be a number in [%1, 1); "
                                       "a gate may demand more evidence, never less")
                            .arg(kMinPnlConfidence, 0, 'f', 2));
        pnl_confidence = confidence_raw.toDouble();
    }

    // Present in the normalized params ONLY when the operator preregistered it.
    // Emitting `families: []` for every gate would change the sealed bytes of
    // params files sealed before this existed, and every one of them would then
    // read TAMPERED — turning a compatible addition into a fleet-wide refusal.
    bool has_families = false;
    QJsonArray families;
    const QJsonValue families_raw = raw.value(QStringLiteral("families"));
    if (!families_raw.isUndefined()) {
        if (!families_raw.isArray() || families_raw.toArray().isEmpty())
            return fail(QStringLiteral("gate param 'families' must be a non-empty array of series "
                                       "tickers (e.g. [\"KXBTCD\"]); omit it to preregister no "
                                       "per-family admission at all"));
        QSet<QString> seen;
        for (const auto& value : families_raw.toArray()) {
            const QString name = value.toString().trimmed().toUpper();
            if (name.isEmpty() || name != value.toString().trimmed())
                return fail(QStringLiteral("gate param 'families' entries must be non-empty "
                                           "upper-case series tickers"));
            if (seen.contains(name))
                return fail(QStringLiteral("gate param 'families' lists '%1' twice").arg(name));
            seen.insert(name);
            families.append(name);
        }
        has_families = true;
    }

    if (error) error->clear();
    QJsonObject normalized{{QStringLiteral("min_settled_bids"), min_settled},
                           {QStringLiteral("min_net_pnl_usd"), min_net_pnl},
                           {QStringLiteral("max_drawdown_usd"), max_drawdown},
                           {QStringLiteral("min_brier_margin"), margin},
                           {QStringLiteral("pnl_confidence"), pnl_confidence}};
    if (has_families) normalized.insert(QStringLiteral("families"), families);
    return normalized;
}

QJsonObject KalshiBotGate::preregister(const QString& path, const QJsonObject& raw_params,
                                       qint64 now_ms, QString* error) {
    const auto fail = [&error](const QString& message) {
        if (error) *error = message;
        return QJsonObject();
    };

    // Immutable while it exists — including when it exists in a tampered
    // state, which must be inspected by a human, never silently overwritten.
    if (QFileInfo::exists(path))
        return fail(QStringLiteral("%1 already exists — preregistered gate criteria are immutable; "
                                   "the gate refuses to re-seal over them")
                        .arg(path));

    QString parse_error;
    const QJsonObject params = parse_params(raw_params, &parse_error);
    if (params.isEmpty()) return fail(parse_error);

    QJsonObject record{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("event"), QString::fromLatin1(kParamsEvent)},
        {QStringLiteral("gate_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("sealed_at_ms"), static_cast<double>(now_ms)},
        {QStringLiteral("sealed_at"), iso(now_ms)},
        {QStringLiteral("params"), params}};
    record.insert(QString::fromLatin1(kSealField), seal(record));

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return fail(QStringLiteral("cannot write %1: %2").arg(path, file.errorString()));
    file.write(QJsonDocument(record).toJson(QJsonDocument::Indented));
    if (!file.commit())
        return fail(QStringLiteral("cannot write %1: %2").arg(path, file.errorString()));
    // Read-only on disk, like the arena season file: editing it is a
    // deliberate act that still breaks the seal.
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadGroup |
                                    QFileDevice::ReadOther);

    if (error) error->clear();
    return record;
}

QJsonValue KalshiBotGate::load_params_file(const QString& path) {
    QFile file(path);
    if (!file.exists()) return QJsonValue(QJsonValue::Undefined);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        // The file is there but unreadable: that is not "nothing was
        // preregistered", so it must not read as NOT_PREREGISTERED.
        return QJsonValue(false);
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (document.isObject()) return document.object();
    if (document.isArray()) return document.array();
    return QJsonValue(false);
}

qint64 KalshiBotGate::published_anchor_ms(const QJsonObject& published_verdict) {
    const QJsonValue scored = published_verdict.value(QStringLiteral("ledger"))
                                  .toObject()
                                  .value(QStringLiteral("first_settled_ts_ms"));
    // `null` whenever the published verdict scored no settlement: that is "no
    // anchor", not an anchor at the epoch.
    if (is_number(scored)) return static_cast<qint64>(scored.toDouble());
    const QJsonValue carried = published_verdict.value(QString::fromLatin1(kAnchorField));
    return is_number(carried) ? static_cast<qint64>(carried.toDouble()) : 0;
}

QString KalshiBotGate::family_of(const QString& ticker) {
    // Kalshi series ticker: everything before the first '-'. A row whose ticker
    // is empty or has no series prefix belongs to NO family, so it can never be
    // bucketed into a "" family that then earns a PASS on nobody's evidence.
    const QString trimmed = ticker.trimmed().toUpper();
    const int dash = trimmed.indexOf(QLatin1Char('-'));
    if (dash <= 0) return {};
    return trimmed.left(dash);
}

QJsonObject KalshiBotGate::evaluate(const QJsonValue& params_record,
                                    const QJsonArray& decision_rows,
                                    const QJsonArray& settlement_rows,
                                    qint64 now_ms,
                                    const RecordIntegrity& record) {
    return evaluate_scoped(params_record, decision_rows, settlement_rows, now_ms, record, true);
}

QJsonObject KalshiBotGate::evaluate_scoped(const QJsonValue& params_record,
                                    const QJsonArray& decision_rows,
                                    const QJsonArray& settlement_rows,
                                    qint64 now_ms,
                                    const RecordIntegrity& ledger_record,
                                    bool with_families) {
    const qint64 anchor = ledger_record.published_first_settled_ts_ms;

    // --- the criteria must be trustworthy before the ledger is read at all --
    if (params_record.isUndefined() || params_record.isNull())
        return refusal(kVerdictNotPreregistered,
                       QStringLiteral("no sealed gate params exist — nothing was preregistered, so "
                                      "there is nothing to judge the paper record against"),
                       now_ms, anchor);
    if (!params_record.isObject())
        return refusal(kVerdictTampered,
                       QStringLiteral("the gate params file is not a JSON object — refusing to guess "
                                      "what was preregistered"),
                       now_ms, anchor);

    const QJsonObject record = params_record.toObject();
    if (!seal_valid(record))
        return refusal(kVerdictTampered,
                       QStringLiteral("the gate params failed their seal check — the preregistered "
                                      "criteria were altered (or never sealed) after preregistration"),
                       now_ms, anchor);

    // Re-validated on every read, not only at seal time: a correctly sealed
    // file that preregisters below-floor criteria is still not a gate.
    QString parse_error;
    const QJsonObject params =
        parse_params(record.value(QStringLiteral("params")).toObject(), &parse_error);
    if (params.isEmpty())
        return refusal(kVerdictTampered,
                       QStringLiteral("the sealed gate params are not valid criteria: %1")
                           .arg(parse_error),
                       now_ms, anchor);

    // --- and the record must be the WHOLE record before it is scored --------
    // Two independent checks, because they catch different truncations. The
    // sequence check sees a generation deleted from the middle of the record;
    // the anchor check sees a record whose oldest surviving row is newer than a
    // settlement this gate has already scored, which is what a two-generation
    // recycling rotation leaves behind — a perfectly contiguous record with its
    // beginning gone.
    if (!ledger_record.missing_generations.isEmpty()) {
        QStringList missing = ledger_record.missing_generations;
        missing.sort();
        return refusal(kVerdictRecordIncomplete,
                       QStringLiteral("the paper record is missing %1 of its generations [%2] — a "
                                      "gap in the sequence is a deleted chunk of the ledger, and "
                                      "the remainder is not the record these criteria judge")
                           .arg(missing.size())
                           .arg(missing.join(QStringLiteral(", "))),
                       now_ms, anchor);
    }
    if (anchor > 0 && ledger_record.oldest_row_ts_ms <= 0)
        return refusal(kVerdictRecordIncomplete,
                       QStringLiteral("the paper record carries no dated row at all, yet this gate "
                                      "has already published a verdict scoring a settlement at %1 "
                                      "(%2) — the record it scored is gone")
                           .arg(anchor)
                           .arg(iso(anchor)),
                       now_ms, anchor);
    if (anchor > 0 && ledger_record.oldest_row_ts_ms > anchor)
        return refusal(kVerdictRecordIncomplete,
                       QStringLiteral("the paper record now begins at %1 (%2), after the first "
                                      "settlement this gate has already scored at %3 (%4) — the "
                                      "earlier record has been truncated, so the remainder is not "
                                      "scored")
                           .arg(ledger_record.oldest_row_ts_ms)
                           .arg(iso(ledger_record.oldest_row_ts_ms))
                           .arg(anchor)
                           .arg(iso(anchor)),
                       now_ms, anchor);

    const int min_settled = params.value(QStringLiteral("min_settled_bids")).toInt();
    const double min_net_pnl = params.value(QStringLiteral("min_net_pnl_usd")).toDouble();
    const double max_drawdown_limit = params.value(QStringLiteral("max_drawdown_usd")).toDouble();
    const double brier_margin = params.value(QStringLiteral("min_brier_margin")).toDouble();

    // --- the bot's own PAPER bids, indexed for the Brier join --------------
    // Ladder rung 5 writes live rows into this same ledger, and the carve-out
    // that permits live bidding at all requires the signal to have passed THIS
    // gate on paper results. A live outcome scored here would be the bot
    // grading itself on the trades its own promotion authorised — the filter
    // lives inside `evaluate()` rather than at the call site precisely because
    // a caller that forgot it would silently invalidate the seal's whole point.
    QHash<QString, QJsonObject> bid_by_position;
    for (const auto& value : decision_rows) {
        const QJsonObject row = value.toObject();
        if (KalshiBotLive::is_live_row(row)) continue;
        if (row.value(QStringLiteral("event")).toString() != QLatin1String(kDecisionEvent)) continue;
        if (row.value(QStringLiteral("action")).toString() != QStringLiteral("bid")) continue;
        const QString id = row.value(QStringLiteral("position_id")).toString();
        if (!id.isEmpty() && !bid_by_position.contains(id)) bid_by_position.insert(id, row);
    }

    // --- settled paper bids, deduped and replayed in settlement order ------
    std::vector<QJsonObject> settled;
    QSet<QString> seen;
    int malformed = 0;
    for (const auto& value : settlement_rows) {
        const QJsonObject row = value.toObject();
        if (KalshiBotLive::is_live_row(row)) continue;
        if (row.value(QStringLiteral("event")).toString() != QLatin1String(kSettlementEvent)) continue;
        const QString id = row.value(QStringLiteral("position_id")).toString();
        // A settlement we cannot identify is not counted as a settled bid: it
        // cannot be deduped, joined, or audited.
        if (id.isEmpty() || !is_number(row.value(QStringLiteral("realized_pnl")))) {
            ++malformed;
            continue;
        }
        if (seen.contains(id)) continue;
        seen.insert(id);
        settled.push_back(row);
    }
    std::stable_sort(settled.begin(), settled.end(),
                     [](const QJsonObject& a, const QJsonObject& b) {
                         return a.value(QStringLiteral("ts_ms")).toDouble() <
                                b.value(QStringLiteral("ts_ms")).toDouble();
                     });

    double net_pnl = 0.0;
    double fees = 0.0;
    double stake = 0.0;
    double running = 0.0;
    double peak = 0.0;  // the record starts flat: a first losing bid IS a drawdown
    double max_drawdown = 0.0;
    int wins = 0;
    int losses = 0;
    double brier_bot_sum = 0.0;
    double brier_market_sum = 0.0;
    int scored = 0;
    int unscored = 0;

    std::vector<double> per_bid_pnl;
    per_bid_pnl.reserve(settled.size());
    for (const QJsonObject& row : settled) {
        const double pnl = row.value(QStringLiteral("realized_pnl")).toDouble();
        per_bid_pnl.push_back(pnl);
        net_pnl += pnl;
        fees += row.value(QStringLiteral("fee_usd")).toDouble();
        stake += row.value(QStringLiteral("stake_usd")).toDouble();
        running += pnl;
        peak = std::max(peak, running);
        max_drawdown = std::max(max_drawdown, peak - running);
        const QJsonValue won = row.value(QStringLiteral("won"));
        if (won.isBool()) (won.toBool() ? wins : losses)++;

        // Brier on the same contracts, for both forecasters: the bot's
        // calibrated P(YES) and the market mid it was measured against, taken
        // from the decision row that opened this very position.
        const auto bid = bid_by_position.constFind(row.value(QStringLiteral("position_id")).toString());
        const QString result = row.value(QStringLiteral("market_result")).toString().trimmed().toUpper();
        if (bid == bid_by_position.constEnd() ||
            (result != QStringLiteral("YES") && result != QStringLiteral("NO"))) {
            ++unscored;
            continue;
        }
        const QJsonValue calibrated = bid->value(QStringLiteral("calibrated_p"));
        const QJsonValue mid = bid->value(QStringLiteral("market_mid"));
        if (!is_number(calibrated) || !is_number(mid)) {
            ++unscored;
            continue;
        }
        const double outcome = result == QStringLiteral("YES") ? 1.0 : 0.0;
        brier_bot_sum += (calibrated.toDouble() - outcome) * (calibrated.toDouble() - outcome);
        brier_market_sum += (mid.toDouble() - outcome) * (mid.toDouble() - outcome);
        ++scored;
    }

    const int settled_bids = static_cast<int>(settled.size());
    net_pnl = round_cents(net_pnl);
    max_drawdown = round_cents(max_drawdown);

    // --- the four preregistered criteria ------------------------------------
    QJsonObject settled_criterion =
        criterion(kCriterionSettled,
                  QStringLiteral("settled paper bids >= preregistered minimum"),
                  settled_bids >= min_settled);
    settled_criterion.insert(QStringLiteral("observed"), settled_bids);
    settled_criterion.insert(QStringLiteral("required"), min_settled);
    settled_criterion.insert(QStringLiteral("comparison"), QStringLiteral("observed >= required"));

    QJsonObject pnl_criterion =
        criterion(kCriterionNetPnl, QStringLiteral("net paper P&L after fees > preregistered minimum"),
                  net_pnl > min_net_pnl);
    pnl_criterion.insert(QStringLiteral("observed"), net_pnl);
    pnl_criterion.insert(QStringLiteral("required"), min_net_pnl);
    pnl_criterion.insert(QStringLiteral("comparison"), QStringLiteral("observed > required"));

    const bool brier_available = scored > 0;
    const double brier_bot = brier_available ? brier_bot_sum / scored : 0.0;
    const double brier_market = brier_available ? brier_market_sum / scored : 0.0;
    QJsonObject brier_criterion =
        criterion(kCriterionBrier,
                  QStringLiteral("bot Brier beats the market-baseline Brier on the same contracts"),
                  brier_available && brier_bot < brier_market - brier_margin);
    brier_criterion.insert(QStringLiteral("brier_available"), brier_available);
    brier_criterion.insert(QStringLiteral("scored_contracts"), scored);
    brier_criterion.insert(QStringLiteral("unscored_contracts"), unscored);
    if (brier_available) {
        // Only measured numbers are written. With nothing scored, a 0.0 Brier
        // would read as a perfect forecast, so no number is written at all.
        brier_criterion.insert(QStringLiteral("observed"), brier_bot);
        brier_criterion.insert(QStringLiteral("brier_bot"), brier_bot);
        brier_criterion.insert(QStringLiteral("brier_market_baseline"), brier_market);
        brier_criterion.insert(QStringLiteral("brier_margin"), brier_margin);
        brier_criterion.insert(QStringLiteral("required"), brier_market - brier_margin);
        brier_criterion.insert(QStringLiteral("comparison"),
                               QStringLiteral("brier_bot < brier_market_baseline - brier_margin"));
    } else {
        brier_criterion.insert(
            QStringLiteral("note"),
            QStringLiteral("no settled bid carried both a calibrated probability and the market mid "
                           "it was measured against — nothing to score, so nothing is claimed"));
    }

    // Is the edge distinguishable from zero, or just a positive coin flip?
    const double pnl_confidence = params.value(QStringLiteral("pnl_confidence"))
                                      .toDouble(kMinPnlConfidence);
    const MeanInterval edge = bootstrap_mean_interval(per_bid_pnl, pnl_confidence,
                                                      kBootstrapSeed, kBootstrapSamples);
    QJsonObject significance_criterion =
        criterion(kCriterionPnlSignificant,
                  QStringLiteral("per-bid edge is distinguishable from zero, not merely positive"),
                  edge.available && edge.lo > 0.0);
    significance_criterion.insert(QStringLiteral("interval_available"), edge.available);
    if (edge.available) {
        significance_criterion.insert(QStringLiteral("observed"), round_cents(edge.lo));
        significance_criterion.insert(QStringLiteral("mean_per_bid_usd"), edge.mean);
        significance_criterion.insert(QStringLiteral("ci_low_per_bid_usd"), edge.lo);
        significance_criterion.insert(QStringLiteral("ci_high_per_bid_usd"), edge.hi);
        significance_criterion.insert(QStringLiteral("confidence"), pnl_confidence);
        significance_criterion.insert(QStringLiteral("required"), 0.0);
        significance_criterion.insert(QStringLiteral("comparison"),
                                      QStringLiteral("ci_low_per_bid_usd > 0"));
    } else {
        // Never a 0.0 that would read as a measured zero edge.
        significance_criterion.insert(
            QStringLiteral("note"),
            QStringLiteral("fewer than two settled bids — nothing to resample, so no claim is made "
                           "about whether an edge exists"));
    }

    // What drawdown would pure noise produce over a record this size? Reported,
    // never enforced: it is the yardstick that tells an operator whether the
    // cap they are about to seal sits above or below the noise floor. A $20 cap
    // over 300 bids at this volatility is tripped by 70.8% of NO-edge records.
    const DrawdownNoise noise =
        bootstrap_drawdown_noise(per_bid_pnl, settled_bids, kBootstrapSeed, kBootstrapSamples);

    QJsonObject drawdown_criterion =
        criterion(kCriterionDrawdown,
                  QStringLiteral("worst peak-to-trough realized drawdown <= preregistered limit"),
                  max_drawdown <= max_drawdown_limit);
    drawdown_criterion.insert(QStringLiteral("observed"), max_drawdown);
    drawdown_criterion.insert(QStringLiteral("required"), max_drawdown_limit);
    drawdown_criterion.insert(QStringLiteral("comparison"), QStringLiteral("observed <= required"));
    drawdown_criterion.insert(QStringLiteral("noise_floor_available"), noise.available);
    if (noise.available) {
        drawdown_criterion.insert(QStringLiteral("noise_implied_p50_usd"), round_cents(noise.p50));
        drawdown_criterion.insert(QStringLiteral("noise_implied_p95_usd"), round_cents(noise.p95));
        if (max_drawdown_limit < noise.p50)
            drawdown_criterion.insert(
                QStringLiteral("note"),
                QStringLiteral("the preregistered limit sits BELOW the median drawdown a no-edge "
                               "record of this size would produce (%1) — this criterion is "
                               "measuring volatility, not skill")
                    .arg(round_cents(noise.p50)));
    }

    const QJsonArray criteria{settled_criterion, pnl_criterion, significance_criterion,
                              brier_criterion, drawdown_criterion};
    bool all_met = true;
    for (const auto& value : criteria)
        if (!value.toObject().value(QStringLiteral("met")).toBool()) all_met = false;

    QJsonObject out{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("event"), QString::fromLatin1(kVerdictEvent)},
        {QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
        {QStringLiteral("ts"), iso(now_ms)},
        {QStringLiteral("mode"), QStringLiteral("paper")},
        {QStringLiteral("verdict"), QString::fromLatin1(all_met ? kVerdictPass : kVerdictFail)},
        {QStringLiteral("evaluated"), true},
        {QStringLiteral("gate_id"), record.value(QStringLiteral("gate_id"))},
        {QStringLiteral("sealed_at_ms"), record.value(QStringLiteral("sealed_at_ms"))},
        {QStringLiteral("seal_sha256"), record.value(QString::fromLatin1(kSealField))},
        {QStringLiteral("params"), params},
        {QStringLiteral("criteria"), criteria},
        {QStringLiteral("ledger"),
         QJsonObject{{QStringLiteral("settled_bids"), settled_bids},
                     {QStringLiteral("wins"), wins},
                     {QStringLiteral("losses"), losses},
                     {QStringLiteral("net_pnl_usd"), net_pnl},
                     {QStringLiteral("stake_usd"), round_cents(stake)},
                     {QStringLiteral("fees_usd"), round_cents(fees)},
                     {QStringLiteral("max_drawdown_usd"), max_drawdown},
                     {QStringLiteral("scored_contracts"), scored},
                     {QStringLiteral("unscored_contracts"), unscored},
                     {QStringLiteral("malformed_settlement_rows"), malformed},
                     {QStringLiteral("first_settled_ts_ms"),
                      settled.empty() ? QJsonValue()
                                      : settled.front().value(QStringLiteral("ts_ms"))},
                     {QStringLiteral("last_settled_ts_ms"),
                      settled.empty() ? QJsonValue()
                                      : settled.back().value(QStringLiteral("ts_ms"))}}}};

    // --- per-family verdicts -------------------------------------------------
    // The top-level verdict above stays POOLED: it is the whole-record answer,
    // and existing consumers must not silently change meaning. Admission is a
    // separate question, answered per family by KalshiBotLive::permit(), which
    // reads `by_family` and refuses any family absent from it.
    //
    // Only preregistered families are scored. With none preregistered there is
    // no per-family admission at all — see parse_params.
    if (with_families) {
        const QJsonArray eligible = params.value(QStringLiteral("families")).toArray();
        QJsonObject by_family;
        for (const auto& value : eligible) {
            const QString name = value.toString();
            QJsonArray family_settlements;
            for (const auto& row : settlement_rows)
                if (family_of(row.toObject().value(QStringLiteral("ticker")).toString()) == name)
                    family_settlements.append(row);
            // Decision rows are passed whole: the scorer pairs them to
            // settlements by position_id, so rows from other families simply
            // never match. Filtering them here would risk dropping the bid row
            // a kept settlement needs.
            const QJsonObject scored_family = evaluate_scoped(
                params_record, decision_rows, family_settlements, now_ms, ledger_record, false);
            by_family.insert(name,
                             QJsonObject{{QStringLiteral("verdict"),
                                          scored_family.value(QStringLiteral("verdict"))},
                                         {QStringLiteral("criteria"),
                                          scored_family.value(QStringLiteral("criteria"))},
                                         {QStringLiteral("ledger"),
                                          scored_family.value(QStringLiteral("ledger"))}});
        }
        out.insert(QStringLiteral("by_family"), by_family);
        out.insert(QStringLiteral("by_family_eligible"), !eligible.isEmpty());
        if (eligible.isEmpty())
            out.insert(QStringLiteral("by_family_note"),
                       QStringLiteral("no families preregistered — no family is admissible; seal a "
                                      "'families' param to enable per-family admission"));
    }
    return out;
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
