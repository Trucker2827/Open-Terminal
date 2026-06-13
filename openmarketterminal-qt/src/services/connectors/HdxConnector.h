#pragma once
// OpenMarketTerminal — HDX connector (direct, keyless, public).
//
// Humanitarian Data Exchange dataset search via the CKAN API
// (https://data.humdata.org/api/3/action/package_search) — keyless JSON.
// Results carry full Provenance for the badge.

#include "services/connectors/Provenance.h"

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

namespace openmarketterminal::connectors {

struct HdxDataset {
    QString title;
    QString organization;
    QString updated;   ///< metadata_modified date.
    QString name;      ///< Dataset slug.
    int     resources = 0;
};

class HdxConnector : public QObject {
    Q_OBJECT
  public:
    static HdxConnector& instance();
    explicit HdxConnector(QObject* parent = nullptr);

    /// Search HDX datasets by free-text query.
    void search(const QString& query, int rows = 20);

  signals:
    void datasets_ready(QString query, QVector<openmarketterminal::connectors::HdxDataset> datasets,
                        openmarketterminal::connectors::Provenance provenance);
    void datasets_failed(QString error, openmarketterminal::connectors::Provenance provenance);

  private:
    Q_DISABLE_COPY(HdxConnector)
};

} // namespace openmarketterminal::connectors

Q_DECLARE_METATYPE(openmarketterminal::connectors::HdxDataset)
Q_DECLARE_METATYPE(QVector<openmarketterminal::connectors::HdxDataset>)
