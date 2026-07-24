#include "services/prediction/kalshi/KalshiBotGate.h"

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

QJsonObject refusal(const char* verdict, const QString& reason, qint64 now_ms) {
    // A refusal carries NO criteria and NO ledger numbers: the gate did not
    // evaluate, and an empty criteria array must not read as "nothing passed".
    return QJsonObject{{QStringLiteral("schema"), 1},
                       {QStringLiteral("event"), QString::fromLatin1(kVerdictEvent)},
                       {QStringLiteral("ts_ms"), static_cast<double>(now_ms)},
                       {QStringLiteral("ts"), iso(now_ms)},
                       {QStringLiteral("mode"), QStringLiteral("paper")},
                       {QStringLiteral("verdict"), QString::fromLatin1(verdict)},
                       {QStringLiteral("evaluated"), false},
                       {QStringLiteral("reason"), reason}};
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
                                          QStringLiteral("min_brier_margin")};
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

    if (error) error->clear();
    return QJsonObject{{QStringLiteral("min_settled_bids"), min_settled},
                       {QStringLiteral("min_net_pnl_usd"), min_net_pnl},
                       {QStringLiteral("max_drawdown_usd"), max_drawdown},
                       {QStringLiteral("min_brier_margin"), margin}};
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

QJsonObject KalshiBotGate::evaluate(const QJsonValue& params_record,
                                    const QJsonArray& decision_rows,
                                    const QJsonArray& settlement_rows,
                                    qint64 now_ms) {
    // --- the criteria must be trustworthy before the ledger is read at all --
    if (params_record.isUndefined() || params_record.isNull())
        return refusal(kVerdictNotPreregistered,
                       QStringLiteral("no sealed gate params exist — nothing was preregistered, so "
                                      "there is nothing to judge the paper record against"),
                       now_ms);
    if (!params_record.isObject())
        return refusal(kVerdictTampered,
                       QStringLiteral("the gate params file is not a JSON object — refusing to guess "
                                      "what was preregistered"),
                       now_ms);

    const QJsonObject record = params_record.toObject();
    if (!seal_valid(record))
        return refusal(kVerdictTampered,
                       QStringLiteral("the gate params failed their seal check — the preregistered "
                                      "criteria were altered (or never sealed) after preregistration"),
                       now_ms);

    // Re-validated on every read, not only at seal time: a correctly sealed
    // file that preregisters below-floor criteria is still not a gate.
    QString parse_error;
    const QJsonObject params =
        parse_params(record.value(QStringLiteral("params")).toObject(), &parse_error);
    if (params.isEmpty())
        return refusal(kVerdictTampered,
                       QStringLiteral("the sealed gate params are not valid criteria: %1")
                           .arg(parse_error),
                       now_ms);

    const int min_settled = params.value(QStringLiteral("min_settled_bids")).toInt();
    const double min_net_pnl = params.value(QStringLiteral("min_net_pnl_usd")).toDouble();
    const double max_drawdown_limit = params.value(QStringLiteral("max_drawdown_usd")).toDouble();
    const double brier_margin = params.value(QStringLiteral("min_brier_margin")).toDouble();

    // --- the bot's own bids, indexed for the Brier join --------------------
    QHash<QString, QJsonObject> bid_by_position;
    for (const auto& value : decision_rows) {
        const QJsonObject row = value.toObject();
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

    for (const QJsonObject& row : settled) {
        const double pnl = row.value(QStringLiteral("realized_pnl")).toDouble();
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

    QJsonObject drawdown_criterion =
        criterion(kCriterionDrawdown,
                  QStringLiteral("worst peak-to-trough realized drawdown <= preregistered limit"),
                  max_drawdown <= max_drawdown_limit);
    drawdown_criterion.insert(QStringLiteral("observed"), max_drawdown);
    drawdown_criterion.insert(QStringLiteral("required"), max_drawdown_limit);
    drawdown_criterion.insert(QStringLiteral("comparison"), QStringLiteral("observed <= required"));

    const QJsonArray criteria{settled_criterion, pnl_criterion, brier_criterion, drawdown_criterion};
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
    return out;
}

} // namespace openmarketterminal::services::prediction::kalshi_ns
