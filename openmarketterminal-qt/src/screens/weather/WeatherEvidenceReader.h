#pragma once

#include <QHash>
#include <QString>

namespace openmarketterminal::screens {

/// One evaluated bracket's forecast-vs-market evidence, as written by
/// weather_producer.py's bracket_record() (Task 1) into
/// evidence_file("kalshi-weather-plan.json")'s `brackets` array.
///
/// forecast_high_f/forecast_p are pure forecast math and always present.
/// edge is only computed by the producer when the bracket's order book was
/// actually fetched (in-window brackets); for out-of-window brackets the
/// JSON `edge` is `null`. That null is surfaced here as NaN (edge fields are
/// doubles per this struct's contract, so there is no separate has_edge
/// flag) — callers must check `std::isnan(edge)` before formatting or
/// color-coding it, never trust a raw 0.0 as "no edge" since that is a
/// legitimate value.
struct BracketForecast {
    QString ticker;
    double forecast_high_f = 0.0;
    double forecast_p = 0.0;
    double edge = 0.0;
    bool in_window = false;
};

/// Pure reader for the weather producer's evidence JSON. No path resolution
/// of its own — callers pass the already-resolved evidence file path (e.g.
/// via cli::kalshi_evidence_path("kalshi-weather-plan.json"), the same
/// helper KalshiScreen uses for calibrator.json) so this stays Qt6::Core-only
/// and independently testable against a fixture file.
class WeatherEvidenceReader {
  public:
    /// Parses the `brackets` array at `path` into a map keyed by ticker.
    /// Returns an empty map on a missing file, unreadable file, or
    /// unparseable/non-object JSON — fails closed, never throws.
    static QHash<QString, BracketForecast> load(const QString& path);
};

} // namespace openmarketterminal::screens
