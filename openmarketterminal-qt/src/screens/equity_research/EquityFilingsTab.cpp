#include "screens/equity_research/EquityFilingsTab.h"

#include "ui/components/ProvenanceBadge.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

namespace openmarketterminal::screens {

EquityFilingsTab::EquityFilingsTab(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto* header = new QHBoxLayout;
    title_ = new QLabel(tr("SEC FILINGS — select a symbol"));
    title_->setStyleSheet(QStringLiteral("font-weight:700; letter-spacing:1px;"));
    header->addWidget(title_);
    header->addStretch();
    badge_ = new openmarketterminal::ui::ProvenanceBadge(this);
    header->addWidget(badge_);
    root->addLayout(header);

    table_ = new QTableWidget(this);
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels({tr("Form"), tr("Date"), tr("Accession"), tr("Document")});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    root->addWidget(table_, 1);

    // Wire connector results (this tab is the sole consumer).
    auto& conn = connectors::SecEdgarConnector::instance();
    connect(&conn, &connectors::SecEdgarConnector::filings_ready, this,
            [this](QString, QString company, QVector<connectors::EdgarFiling> filings, connectors::Provenance prov) {
                if (badge_)
                    badge_->set_provenance(prov);
                populate(filings, company);
            });
    connect(&conn, &connectors::SecEdgarConnector::filings_failed, this,
            [this](QString err, connectors::Provenance prov) {
                if (badge_)
                    badge_->set_provenance(prov);
                if (title_)
                    title_->setText(tr("SEC FILINGS — %1").arg(err));
            });
}

void EquityFilingsTab::set_symbol(const QString& symbol) {
    const QString s = symbol.trimmed();
    if (s.isEmpty() || s == current_symbol_)
        return;
    current_symbol_ = s;
    if (title_)
        title_->setText(tr("SEC FILINGS — %1 (loading…)").arg(s.toUpper()));
    connectors::SecEdgarConnector::instance().fetch_filings_by_ticker(s, 60);
}

void EquityFilingsTab::populate(const QVector<connectors::EdgarFiling>& filings, const QString& company) {
    if (title_)
        title_->setText(tr("SEC FILINGS — %1 (%2)").arg(company, current_symbol_.toUpper()));
    table_->setRowCount(filings.size());
    for (int i = 0; i < filings.size(); ++i) {
        const auto& f = filings.at(i);
        table_->setItem(i, 0, new QTableWidgetItem(f.form));
        table_->setItem(i, 1, new QTableWidgetItem(f.filing_date));
        table_->setItem(i, 2, new QTableWidgetItem(f.accession));
        table_->setItem(i, 3, new QTableWidgetItem(f.primary_doc));
    }
}

} // namespace openmarketterminal::screens
