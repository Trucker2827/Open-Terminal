#pragma once
// OpenMarketTerminal — Yahoo Finance connector (direct, keyless, public).
//
// Replaces the former OpenMarketTerminal `/market/search` dependency with a direct call to
// Yahoo's public search endpoint. Results carry full Provenance for the badge.

#include "services/connectors/Provenance.h"

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

namespace openmarketterminal::connectors {

/// One symbol-search result row.
struct YahooSearchHit {
    QString symbol;
    QString name;
    QString exchange;
    QString type; ///< EQUITY / ETF / INDEX / CRYPTOCURRENCY / CURRENCY ...
};

class YahooConnector : public QObject {
    Q_OBJECT
  public:
    static YahooConnector& instance();
    explicit YahooConnector(QObject* parent = nullptr);

    /// Symbol/name search via https://query2.finance.yahoo.com/v1/finance/search.
    void search(const QString& query, int limit = 10);

  signals:
    void search_ready(QVector<openmarketterminal::connectors::YahooSearchHit> hits,
                      openmarketterminal::connectors::Provenance provenance);
    void search_failed(QString error, openmarketterminal::connectors::Provenance provenance);

  private:
    Q_DISABLE_COPY(YahooConnector)
};

} // namespace openmarketterminal::connectors

Q_DECLARE_METATYPE(openmarketterminal::connectors::YahooSearchHit)
Q_DECLARE_METATYPE(QVector<openmarketterminal::connectors::YahooSearchHit>)
