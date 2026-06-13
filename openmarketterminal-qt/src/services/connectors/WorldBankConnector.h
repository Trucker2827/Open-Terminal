#pragma once
// OpenMarketTerminal — World Bank connector (direct, keyless, public).
//
// Open macro indicator time series from https://api.worldbank.org/v2 — no key
// required. Results carry full Provenance for the badge.

#include "services/connectors/Provenance.h"

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

namespace openmarketterminal::connectors {

/// One observation in an indicator series.
struct WorldBankPoint {
    QString date;       ///< Year, e.g. "2023".
    double  value = 0;  ///< Indicator value.
    bool    has_value = false; ///< World Bank returns null for missing years.
};

class WorldBankConnector : public QObject {
    Q_OBJECT
  public:
    static WorldBankConnector& instance();
    explicit WorldBankConnector(QObject* parent = nullptr);

    /// Fetch an indicator series for a country.
    /// `country` is an ISO3 code (e.g. "USA") or "all"; `indicator` is a World
    /// Bank code (e.g. "NY.GDP.MKTP.CD"). Most-recent year first.
    void fetch_series(const QString& country, const QString& indicator, int points = 20);

  signals:
    void series_ready(QString country, QString indicator, QString indicator_name,
                      QVector<openmarketterminal::connectors::WorldBankPoint> series,
                      openmarketterminal::connectors::Provenance provenance);
    void series_failed(QString error, openmarketterminal::connectors::Provenance provenance);

  private:
    Q_DISABLE_COPY(WorldBankConnector)
};

} // namespace openmarketterminal::connectors

Q_DECLARE_METATYPE(openmarketterminal::connectors::WorldBankPoint)
Q_DECLARE_METATYPE(QVector<openmarketterminal::connectors::WorldBankPoint>)
