#include "services/ai_quant_lab/ScreenWatchlistFeed.h"

#include <QJsonValue>

namespace openmarketterminal::services::quant {

QString screen_ic_unmeasured_disclaimer() {
    return QStringLiteral("IC unmeasured — treat ranks as opinion");
}

QString screen_watchlist_name(const QString& model_id) {
    return QStringLiteral("Model screen: %1").arg(model_id);
}

QString screen_ic_evidence(const QJsonObject& ic_payload) {
    if (!ic_payload.value("success").toBool(false))
        return screen_ic_unmeasured_disclaimer();
    const QJsonObject results = ic_payload.value("results").toObject();
    const QJsonValue ic_mean = results.value("IC_mean");
    if (!ic_mean.isDouble())
        return screen_ic_unmeasured_disclaimer();
    const QJsonObject period = ic_payload.value("period").toObject();
    QString evidence = QStringLiteral("IC_mean %1").arg(QString::number(ic_mean.toDouble(), 'f', 4));
    const QJsonValue rank_ic = results.value("Rank_IC_mean");
    if (rank_ic.isDouble())
        evidence += QStringLiteral(", Rank_IC_mean %1").arg(QString::number(rank_ic.toDouble(), 'f', 4));
    const QString start = period.value("start").toString();
    const QString end = period.value("end").toString();
    if (!start.isEmpty() && !end.isEmpty())
        evidence += QStringLiteral(" over %1..%2").arg(start, end);
    const QJsonValue days = results.value("days");
    if (days.isDouble())
        evidence += QStringLiteral(" (%1 days)").arg(days.toInt());
    return evidence;
}

QString screen_watchlist_description(const QJsonObject& screen_payload, const QJsonObject& ic_payload) {
    const QString model_id = screen_payload.value("model_id").toString();
    const QString as_of = screen_payload.value("as_of").toString();
    QString desc = QStringLiteral("Model %1 screen as of %2").arg(
        model_id.isEmpty() ? QStringLiteral("?") : model_id, as_of.isEmpty() ? QStringLiteral("?") : as_of);
    const QJsonValue universe = screen_payload.value("universe_size");
    if (universe.isDouble())
        desc += QStringLiteral(" (universe %1)").arg(universe.toInt());
    desc += QStringLiteral(" — %1").arg(screen_ic_evidence(ic_payload));
    return desc;
}

QString screen_stock_note(const QJsonObject& row, const QJsonObject& screen_payload) {
    const QJsonValue score = row.value("score");
    const QString score_str =
        score.isDouble() ? QString::number(score.toDouble(), 'f', 6) : QStringLiteral("?");
    return QStringLiteral("score %1 (model %2, as of %3)")
        .arg(score_str, screen_payload.value("model_id").toString(),
             screen_payload.value("as_of").toString());
}

} // namespace openmarketterminal::services::quant
