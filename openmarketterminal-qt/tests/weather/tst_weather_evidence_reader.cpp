#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cmath>

#include "screens/weather/WeatherEvidenceReader.h"

using openmarketterminal::screens::BracketForecast;
using openmarketterminal::screens::WeatherEvidenceReader;

namespace {

// Mirrors the shape weather_producer.py's bracket_record() writes (Task 1):
// the full key set plus two brackets exercising both book states — one
// in-window bracket whose book was fetched (edge is a real number) and one
// out-of-window bracket whose book was never fetched (edge is JSON null).
QByteArray sample_evidence() {
    return QByteArrayLiteral(R"({
  "event": "kalshi-weather-plan",
  "generated_at_ms": 1785000000000,
  "brackets": [
    {
      "series": "KXHIGHNY",
      "city": "NYC",
      "ticker": "KXHIGHNY-26AUG03-T80",
      "floor": 80.0,
      "cap": null,
      "strike_type": "greater",
      "forecast_high_f": 82.0,
      "forecast_p": 0.7123,
      "market_bid": 0.55,
      "market_ask": 0.6,
      "market_mid": 0.575,
      "edge": 0.1123,
      "in_window": true,
      "seconds_left": 3600,
      "generated_at_ms": 1785000000000
    },
    {
      "series": "KXHIGHCHI",
      "city": "CHI",
      "ticker": "KXHIGHCHI-26AUG04-T75",
      "floor": 75.0,
      "cap": 80.0,
      "strike_type": "between",
      "forecast_high_f": 77.5,
      "forecast_p": 0.4,
      "market_bid": null,
      "market_ask": null,
      "market_mid": null,
      "edge": null,
      "in_window": false,
      "seconds_left": 90000,
      "generated_at_ms": 1785000000000
    }
  ]
})");
}

} // namespace

class TstWeatherEvidenceReader : public QObject {
    Q_OBJECT

  private slots:
    void missing_file_returns_empty_map() {
        const auto map = WeatherEvidenceReader::load(QStringLiteral("/nonexistent/kalshi-weather-plan.json"));
        QVERIFY(map.isEmpty());
    }

    void garbage_file_returns_empty_map() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir(dir.path()).filePath(QStringLiteral("garbage.json"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("not json");
        f.close();

        const auto map = WeatherEvidenceReader::load(path);
        QVERIFY(map.isEmpty());
    }

    void loads_brackets_keyed_by_ticker() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir(dir.path()).filePath(QStringLiteral("kalshi-weather-plan.json"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(sample_evidence());
        f.close();

        const auto map = WeatherEvidenceReader::load(path);
        QCOMPARE(map.size(), 2);
        QVERIFY(map.contains(QStringLiteral("KXHIGHNY-26AUG03-T80")));
        QVERIFY(map.contains(QStringLiteral("KXHIGHCHI-26AUG04-T75")));

        const BracketForecast& nyc = map.value(QStringLiteral("KXHIGHNY-26AUG03-T80"));
        QCOMPARE(nyc.ticker, QStringLiteral("KXHIGHNY-26AUG03-T80"));
        QCOMPARE(nyc.forecast_high_f, 82.0);
        QCOMPARE(nyc.forecast_p, 0.7123);
        QCOMPARE(nyc.edge, 0.1123);
        QVERIFY(nyc.in_window);
    }

    void null_edge_is_nan_not_zero() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir(dir.path()).filePath(QStringLiteral("kalshi-weather-plan.json"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(sample_evidence());
        f.close();

        const auto map = WeatherEvidenceReader::load(path);
        const BracketForecast& chi = map.value(QStringLiteral("KXHIGHCHI-26AUG04-T75"));
        QCOMPARE(chi.forecast_p, 0.4);
        QVERIFY(!chi.in_window);
        // A null `edge` must never silently read as 0.0 (a legitimate "fair
        // price" edge value) — it must be distinguishably absent.
        QVERIFY(std::isnan(chi.edge));
    }
};

QTEST_MAIN(TstWeatherEvidenceReader)
#include "tst_weather_evidence_reader.moc"
