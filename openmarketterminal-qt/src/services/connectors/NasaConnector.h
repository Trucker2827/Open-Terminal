#pragma once
// OpenMarketTerminal — NASA connector (direct, keyless, public).
//
// Earth natural-event tracking via NASA EONET (https://eonet.gsfc.nasa.gov) —
// keyless JSON. Surfaces open natural events (storms, wildfires, volcanoes …)
// as alt-data, each with full Provenance for the badge.

#include "services/connectors/Provenance.h"

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

namespace openmarketterminal::connectors {

struct NaturalEvent {
    QString id;
    QString title;
    QString category;
    QString date;       ///< Most recent observation timestamp.
    double  lon = 0, lat = 0;
    bool    has_coord = false;
};

class NasaConnector : public QObject {
    Q_OBJECT
  public:
    static NasaConnector& instance();
    explicit NasaConnector(QObject* parent = nullptr);

    /// Fetch currently-open Earth natural events.
    void fetch_events(int limit = 30);

  signals:
    void events_ready(QVector<openmarketterminal::connectors::NaturalEvent> events,
                      openmarketterminal::connectors::Provenance provenance);
    void events_failed(QString error, openmarketterminal::connectors::Provenance provenance);

  private:
    Q_DISABLE_COPY(NasaConnector)
};

} // namespace openmarketterminal::connectors

Q_DECLARE_METATYPE(openmarketterminal::connectors::NaturalEvent)
Q_DECLARE_METATYPE(QVector<openmarketterminal::connectors::NaturalEvent>)
