#include "screens/weather/WeatherEvidenceReader.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <limits>

namespace openmarketterminal::screens {

namespace {

/// JSON `null`/absent-key both read as QJsonValue::toDouble() == 0.0, which
/// would misrepresent "no edge computed" as a real zero edge. Only a
/// present, non-null number is trusted; everything else becomes NaN so
/// callers can std::isnan()-guard before formatting/color-coding it.
double number_or_nan(const QJsonValue& value) {
    if (!value.isDouble())
        return std::numeric_limits<double>::quiet_NaN();
    return value.toDouble();
}

} // namespace

QHash<QString, BracketForecast> WeatherEvidenceReader::load(const QString& path) {
    QHash<QString, BracketForecast> out;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return out;

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return out;

    const QJsonArray brackets = document.object().value(QStringLiteral("brackets")).toArray();
    for (const QJsonValue& entry : brackets) {
        if (!entry.isObject())
            continue;
        const QJsonObject record = entry.toObject();
        const QString ticker = record.value(QStringLiteral("ticker")).toString();
        if (ticker.isEmpty())
            continue;

        BracketForecast forecast;
        forecast.ticker = ticker;
        forecast.forecast_high_f = number_or_nan(record.value(QStringLiteral("forecast_high_f")));
        forecast.forecast_p = number_or_nan(record.value(QStringLiteral("forecast_p")));
        forecast.edge = number_or_nan(record.value(QStringLiteral("edge")));
        forecast.in_window = record.value(QStringLiteral("in_window")).toBool();

        out.insert(ticker, forecast);
    }

    return out;
}

} // namespace openmarketterminal::screens
