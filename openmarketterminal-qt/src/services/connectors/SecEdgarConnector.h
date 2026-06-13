#pragma once
// OpenMarketTerminal — SEC EDGAR connector (direct, keyless, public).
//
// Company filing history from https://data.sec.gov/submissions — keyless (SEC
// only requires a descriptive User-Agent, which HttpClient supplies). Results
// carry full Provenance for the badge.

#include "services/connectors/Provenance.h"

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

namespace openmarketterminal::connectors {

/// One filing in a company's recent history.
struct EdgarFiling {
    QString form;         ///< Form type, e.g. "10-K", "8-K", "4".
    QString filing_date;  ///< ISO date.
    QString accession;    ///< Accession number.
    QString primary_doc;  ///< Primary document filename.
};

class SecEdgarConnector : public QObject {
    Q_OBJECT
  public:
    static SecEdgarConnector& instance();
    explicit SecEdgarConnector(QObject* parent = nullptr);

    /// Fetch recent filings for a numeric CIK (zero-padding handled internally).
    void fetch_filings(const QString& cik, int limit = 40);

    /// Resolve a ticker to its CIK via SEC's company_tickers map, then fetch
    /// filings. SEC's www host requires a contact-style User-Agent, supplied
    /// internally. Emits filings_failed if the ticker can't be resolved.
    void fetch_filings_by_ticker(const QString& ticker, int limit = 40);

  signals:
    void filings_ready(QString cik, QString company, QVector<openmarketterminal::connectors::EdgarFiling> filings,
                       openmarketterminal::connectors::Provenance provenance);
    void filings_failed(QString error, openmarketterminal::connectors::Provenance provenance);

  private:
    Q_DISABLE_COPY(SecEdgarConnector)
};

} // namespace openmarketterminal::connectors

Q_DECLARE_METATYPE(openmarketterminal::connectors::EdgarFiling)
Q_DECLARE_METATYPE(QVector<openmarketterminal::connectors::EdgarFiling>)
