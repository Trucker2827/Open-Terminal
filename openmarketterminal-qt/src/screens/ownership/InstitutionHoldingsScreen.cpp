// src/screens/ownership/InstitutionHoldingsScreen.cpp
#include "screens/ownership/InstitutionHoldingsScreen.h"

#include "ui/tables/DataTable.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace openmarketterminal::screens {

InstitutionHoldingsScreen::InstitutionHoldingsScreen(QWidget* parent) : QWidget(parent) {
    build_ui();
    connect(&services::GovDataService::instance(), &services::GovDataService::result_ready,
            this, &InstitutionHoldingsScreen::on_result);
    load();  // default manager
}

void InstitutionHoldingsScreen::build_ui() {
    auto* root = new QVBoxLayout(this);

    auto* bar = new QHBoxLayout();
    bar->addWidget(new QLabel(tr("Manager ticker:")));
    ticker_edit_ = new QLineEdit("BRK-B");
    ticker_edit_->setMaximumWidth(120);
    bar->addWidget(ticker_edit_);
    load_btn_ = new QPushButton(tr("Load"));
    bar->addWidget(load_btn_);
    bar->addStretch();
    root->addLayout(bar);

    connect(load_btn_, &QPushButton::clicked, this, &InstitutionHoldingsScreen::load);
    connect(ticker_edit_, &QLineEdit::returnPressed, this, &InstitutionHoldingsScreen::load);

    header_ = new QLabel;
    header_->setWordWrap(true);
    root->addWidget(header_);

    stack_ = new QStackedWidget;
    status_ = new QLabel(tr("Enter a manager ticker (e.g. BRK-B) and press Load."));
    status_->setAlignment(Qt::AlignCenter);
    status_->setWordWrap(true);
    stack_->addWidget(status_);   // 0
    table_ = new ui::DataTable;
    table_->set_headers({tr("Issuer"), tr("Ticker"), tr("Value"), tr("Shares")});
    stack_->addWidget(table_);    // 1
    root->addWidget(stack_, 1);
}

void InstitutionHoldingsScreen::show_message(const QString& msg) {
    header_->clear();
    status_->setText(msg);
    stack_->setCurrentIndex(0);
}

void InstitutionHoldingsScreen::load() {
    const QString ticker = ticker_edit_->text().trimmed().toUpper();
    if (ticker.isEmpty()) {
        show_message(tr("Enter a manager ticker."));
        return;
    }
    show_message(tr("Loading 13F holdings for %1…").arg(ticker));
    services::GovDataService::instance().execute(
        "mcp/edgar/main.py", "13f_top_holdings", {ticker, "25"}, request_id_);
}

void InstitutionHoldingsScreen::on_result(const QString& request_id, const services::GovDataResult& result) {
    if (request_id != request_id_)
        return;
    if (!result.success) {
        show_message(tr("13F data unavailable: %1")
                         .arg(result.error.isEmpty() ? tr("service error") : result.error));
        return;
    }
    if (result.data.contains("error")) {
        const QString msg = result.data.value("error").toObject().value("error").toString();
        show_message(tr("13F parser unavailable: %1").arg(msg.isEmpty() ? tr("no parsed holdings") : msg));
        return;
    }
    populate(result.data);
}

void InstitutionHoldingsScreen::populate(const QJsonObject& data) {
    const QJsonObject d = data.value("data").toObject();
    const QJsonArray holdings = d.value("top_holdings").toArray();
    if (holdings.isEmpty()) {
        show_message(tr("No 13F holdings found."));
        return;
    }
    const QString manager = d.value("manager_name").toString();
    const double total = d.value("total_value").toDouble();
    const QString period = d.value("report_period").toString();
    header_->setText(tr("<b>%1</b> · reported %2 · total portfolio %3")
                         .arg(manager.isEmpty() ? tr("(manager)") : manager,
                              period.isEmpty() ? tr("n/a") : period,
                              total != 0.0 ? QString("$%L1").arg(total, 0, 'f', 0) : tr("n/a")));

    table_->clear_data();
    table_->set_headers({tr("Issuer"), tr("Ticker"), tr("Value"), tr("Shares")});
    int r = 0;
    for (const auto& v : holdings) {
        const QJsonObject o = v.toObject();
        const double value = o.value("value").toDouble();
        const double shares = o.value("shares").toDouble();
        table_->add_row({
            o.value("issuer").toString(),
            o.value("ticker").toString(),
            value != 0.0 ? QString("$%L1").arg(value, 0, 'f', 0) : QString(),
            shares != 0.0 ? QLocale().toString(shares, 'f', 0) : QString(),
        });
        if (value != 0.0) table_->set_cell_numeric(r, 2, value);
        if (shares != 0.0) table_->set_cell_numeric(r, 3, shares);
        ++r;
    }
    stack_->setCurrentIndex(1);
}

} // namespace openmarketterminal::screens
