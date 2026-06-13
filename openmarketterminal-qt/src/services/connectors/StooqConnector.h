#pragma once
// OpenMarketTerminal — Stooq connector (direct, keyless, public, CSV).
//
// Light price quotes from https://stooq.com (CSV) via the connector framework's
// text() fetch. Keyless. NOTE: Stooq blocks some cloud/data-centre IPs (returns
// a 404 page); from a normal residential/office network it serves CSV quotes.
// A failed fetch surfaces gracefully through the ProvenanceBadge.

#include "services/connectors/Provenance.h"

#include <QMetaType>
#include <QObject>
#include <QString>

namespace openmarketterminal::connectors {

struct StooqQuote {
    QString symbol;
    QString date;
    QString time;
    double  open = 0, high = 0, low = 0, close = 0, volume = 0;
    bool    ok = false;
};

class StooqConnector : public QObject {
    Q_OBJECT
  public:
    static StooqConnector& instance();
    explicit StooqConnector(QObject* parent = nullptr);

    /// Fetch a light quote. Plain symbols default to the US market (".us").
    void fetch_quote(const QString& symbol);

  signals:
    void quote_ready(openmarketterminal::connectors::StooqQuote quote, openmarketterminal::connectors::Provenance provenance);
    void quote_failed(QString error, openmarketterminal::connectors::Provenance provenance);

  private:
    Q_DISABLE_COPY(StooqConnector)
};

} // namespace openmarketterminal::connectors

Q_DECLARE_METATYPE(openmarketterminal::connectors::StooqQuote)
