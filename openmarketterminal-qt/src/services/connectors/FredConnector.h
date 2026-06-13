#pragma once
// OpenMarketTerminal — FRED connector (direct, key-required, public).
//
// St. Louis Fed economic data (https://api.stlouisfed.org/fred). FRED requires a
// free API key; the connector reads it from settings ("connectors.fred_api_key").
// When absent, it emits a failure whose Provenance reports "key required —
// missing" so the ProvenanceBadge makes the requirement explicit to the user.

#include "services/connectors/Provenance.h"

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

namespace openmarketterminal::connectors {

/// One observation in a FRED series.
struct FredPoint {
    QString date;       ///< ISO date, e.g. "2024-01-01".
    double  value = 0;
    bool    has_value = false; ///< FRED uses "." for missing values.
};

class FredConnector : public QObject {
    Q_OBJECT
  public:
    static FredConnector& instance();
    explicit FredConnector(QObject* parent = nullptr);

    /// True once a FRED API key is configured in settings.
    static bool has_key();

    /// Fetch the latest observations for a FRED series (e.g. "CPIAUCSL", "GDP").
    void fetch_series(const QString& series_id, int points = 24);

  signals:
    void series_ready(QString series_id, QVector<openmarketterminal::connectors::FredPoint> series,
                      openmarketterminal::connectors::Provenance provenance);
    void series_failed(QString error, openmarketterminal::connectors::Provenance provenance);

  private:
    Q_DISABLE_COPY(FredConnector)
};

} // namespace openmarketterminal::connectors

Q_DECLARE_METATYPE(openmarketterminal::connectors::FredPoint)
Q_DECLARE_METATYPE(QVector<openmarketterminal::connectors::FredPoint>)
