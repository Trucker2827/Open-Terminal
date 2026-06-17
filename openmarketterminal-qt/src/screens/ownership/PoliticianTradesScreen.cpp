// src/screens/ownership/PoliticianTradesScreen.cpp
#include "screens/ownership/PoliticianTradesScreen.h"

#include "ui/tables/DataTable.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace openmarketterminal::screens {

PoliticianTradesScreen::PoliticianTradesScreen(QWidget* parent) : QWidget(parent) {
    build_ui();
    connect(&services::GovDataService::instance(), &services::GovDataService::result_ready,
            this, &PoliticianTradesScreen::on_result);
    load();
}

void PoliticianTradesScreen::build_ui() {
    auto* root = new QVBoxLayout(this);

    auto* bar = new QHBoxLayout();
    bar->addWidget(new QLabel(tr("Ticker:")));
    ticker_edit_ = new QLineEdit("AAPL");
    ticker_edit_->setMaximumWidth(120);
    bar->addWidget(ticker_edit_);
    load_btn_ = new QPushButton(tr("Load"));
    bar->addWidget(load_btn_);
    bar->addStretch();
    root->addLayout(bar);

    connect(load_btn_, &QPushButton::clicked, this, &PoliticianTradesScreen::load);
    connect(ticker_edit_, &QLineEdit::returnPressed, this, &PoliticianTradesScreen::load);

    stack_ = new QStackedWidget;
    status_ = new QLabel(tr("Enter a ticker and press Load."));
    status_->setAlignment(Qt::AlignCenter);
    status_->setWordWrap(true);
    stack_->addWidget(status_);   // 0
    table_ = new ui::DataTable;
    table_->set_headers({tr("Trade Date"), tr("Politician"), tr("Party"), tr("State"),
                         tr("Type"), tr("Amount"), tr("Filed")});
    stack_->addWidget(table_);    // 1
    root->addWidget(stack_, 1);
}

void PoliticianTradesScreen::show_message(const QString& msg) {
    status_->setText(msg);
    stack_->setCurrentIndex(0);
}

void PoliticianTradesScreen::load() {
    const QString ticker = ticker_edit_->text().trimmed().toUpper();
    if (ticker.isEmpty()) {
        show_message(tr("Enter a ticker."));
        return;
    }
    show_message(tr("Loading congress trades for %1…").arg(ticker));
    services::GovDataService::instance().execute(
        "ainvest_data.py", "congress", {ticker, "50"}, request_id_);
}

void PoliticianTradesScreen::on_result(const QString& request_id, const services::GovDataResult& result) {
    if (request_id != request_id_)
        return;
    if (!result.success) {
        show_message(tr("Politician data unavailable: %1")
                         .arg(result.error.isEmpty() ? tr("service error") : result.error));
        return;
    }
    // {"error": {...}} (exit 0) carries "not configured" and request failures; show the message.
    if (result.data.contains("error")) {
        const QString msg = result.data.value("error").toObject().value("error").toString();
        show_message(msg.isEmpty() ? tr("Politician data unavailable.") : msg);
        return;
    }
    populate(result.data);
}

void PoliticianTradesScreen::populate(const QJsonObject& data) {
    const QJsonArray rows = data.value("data").toArray();
    if (rows.isEmpty()) {
        show_message(tr("No congress trades found for %1.").arg(data.value("ticker").toString()));
        return;
    }
    table_->clear_data();
    table_->set_headers({tr("Trade Date"), tr("Politician"), tr("Party"), tr("State"),
                         tr("Type"), tr("Amount"), tr("Filed")});
    for (const auto& v : rows) {
        const QJsonObject o = v.toObject();
        table_->add_row({
            o.value("trade_date").toString(),
            o.value("name").toString(),
            o.value("party").toString(),
            o.value("state").toString(),
            o.value("trade_type").toString(),
            o.value("size").toString(),
            o.value("filing_date").toString(),
        });
    }
    stack_->setCurrentIndex(1);
}

} // namespace openmarketterminal::screens
