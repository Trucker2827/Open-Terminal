// src/screens/ownership/InsiderTradesScreen.cpp
#include "screens/ownership/InsiderTradesScreen.h"

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

InsiderTradesScreen::InsiderTradesScreen(QWidget* parent) : QWidget(parent) {
    build_ui();
    connect(&services::GovDataService::instance(), &services::GovDataService::result_ready,
            this, &InsiderTradesScreen::on_result);
    load();  // default ticker
}

void InsiderTradesScreen::build_ui() {
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

    connect(load_btn_, &QPushButton::clicked, this, &InsiderTradesScreen::load);
    connect(ticker_edit_, &QLineEdit::returnPressed, this, &InsiderTradesScreen::load);

    stack_ = new QStackedWidget;
    status_ = new QLabel(tr("Enter a ticker and press Load."));
    status_->setAlignment(Qt::AlignCenter);
    status_->setWordWrap(true);
    stack_->addWidget(status_);              // index 0
    table_ = new ui::DataTable;
    table_->set_headers({tr("Date"), tr("Insider"), tr("Position"), tr("Type"),
                         tr("Code"), tr("Shares"), tr("Price"), tr("Value")});
    stack_->addWidget(table_);               // index 1
    root->addWidget(stack_, 1);
}

void InsiderTradesScreen::show_message(const QString& msg) {
    status_->setText(msg);
    stack_->setCurrentIndex(0);
}

void InsiderTradesScreen::load() {
    const QString ticker = ticker_edit_->text().trimmed().toUpper();
    if (ticker.isEmpty()) {
        show_message(tr("Enter a ticker."));
        return;
    }
    show_message(tr("Loading insider trades for %1…").arg(ticker));
    services::GovDataService::instance().execute(
        "mcp/edgar/main.py", "insider_transactions", {ticker, "30"}, request_id_);
}

void InsiderTradesScreen::on_result(const QString& request_id, const services::GovDataResult& result) {
    if (request_id != request_id_)
        return;
    // Verified-contract handling (condition #5): a parser failure surfaces as a
    // clear message, never a blank table that looks like success.
    if (!result.success) {
        show_message(tr("Insider data unavailable: %1")
                         .arg(result.error.isEmpty() ? tr("service error") : result.error));
        return;
    }
    // The script signals a parse failure as {"error": {...}} with exit 0; the human
    // message lives at error.error (EdgarError.to_dict).
    if (result.data.contains("error")) {
        const QString msg = result.data.value("error").toObject().value("error").toString();
        show_message(tr("Insider parser unavailable: %1").arg(msg.isEmpty() ? tr("no parsed rows") : msg));
        return;
    }
    populate(result.data);
}

void InsiderTradesScreen::populate(const QJsonObject& data) {
    const QJsonArray rows = data.value("data").toArray();
    if (rows.isEmpty()) {
        show_message(tr("No insider (Form 4) transactions found for %1.")
                         .arg(data.value("ticker").toString()));
        return;
    }
    table_->clear_data();
    table_->set_headers({tr("Date"), tr("Insider"), tr("Position"), tr("Type"),
                         tr("Code"), tr("Shares"), tr("Price"), tr("Value")});
    int r = 0;
    for (const auto& v : rows) {
        const QJsonObject o = v.toObject();
        const double shares = o.value("shares").toDouble();
        const double price = o.value("price").toDouble();
        const double value = o.value("value").toDouble();
        table_->add_row({
            o.value("filing_date").toString(),
            o.value("insider").toString(),
            o.value("position").toString(),
            o.value("transaction_type").toString(),
            o.value("code").toString(),
            shares != 0.0 ? QString::number(shares, 'f', 0) : QString(),
            price != 0.0 ? QString("$%1").arg(price, 0, 'f', 2) : QString(),
            value != 0.0 ? QString("$%L1").arg(value, 0, 'f', 0) : QString(),
        });
        if (shares != 0.0) table_->set_cell_numeric(r, 5, shares);
        if (price != 0.0) table_->set_cell_numeric(r, 6, price);
        if (value != 0.0) table_->set_cell_numeric(r, 7, value);
        ++r;
    }
    stack_->setCurrentIndex(1);
}

} // namespace openmarketterminal::screens
