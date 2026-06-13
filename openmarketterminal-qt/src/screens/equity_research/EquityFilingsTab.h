#pragma once
// OpenMarketTerminal — Equity Research "Filings" tab.
// Company SEC filings direct from EDGAR (data.sec.gov) via the connector
// framework, with a ProvenanceBadge. Ticker→CIK resolved by the connector.

#include "services/connectors/Provenance.h"
#include "services/connectors/SecEdgarConnector.h"

#include <QWidget>

class QLabel;
class QTableWidget;

namespace openmarketterminal::ui {
class ProvenanceBadge;
}

namespace openmarketterminal::screens {

class EquityFilingsTab : public QWidget {
    Q_OBJECT
  public:
    explicit EquityFilingsTab(QWidget* parent = nullptr);

    /// Called by EquityResearchScreen when the active symbol changes.
    void set_symbol(const QString& symbol);

  private:
    void populate(const QVector<openmarketterminal::connectors::EdgarFiling>& filings, const QString& company);

    QString current_symbol_;
    QLabel* title_ = nullptr;
    QTableWidget* table_ = nullptr;
    openmarketterminal::ui::ProvenanceBadge* badge_ = nullptr;
};

} // namespace openmarketterminal::screens
