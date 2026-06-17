#include "screens/dashboard/widgets/OwnershipWidget.h"

#include "ui/tables/DataTable.h"

#include <QJsonArray>
#include <QLabel>
#include <QLocale>
#include <QStackedWidget>

namespace openmarketterminal::screens::widgets {

OwnershipWidget::OwnershipWidget(const QString& title, const QString& script, const QString& command,
                                 const QStringList& args, const QStringList& headers,
                                 const QString& request_id, OwnershipRowMapper mapper,
                                 const QString& empty_msg, QWidget* parent)
    : BaseWidget(title, parent),
      script_(script), command_(command), request_id_(request_id), empty_msg_(empty_msg),
      args_(args), headers_(headers), mapper_(std::move(mapper)) {
    stack_ = new QStackedWidget;
    status_ = new QLabel(tr("Loading…"));
    status_->setAlignment(Qt::AlignCenter);
    status_->setWordWrap(true);
    stack_->addWidget(status_);   // 0
    table_ = new ui::DataTable;
    table_->set_headers(headers_);
    stack_->addWidget(table_);    // 1
    content_layout()->addWidget(stack_);
    connect(&services::GovDataService::instance(), &services::GovDataService::result_ready,
            this, &OwnershipWidget::on_result);
}

void OwnershipWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (!loaded_once_) {          // lazy first-load so a dashboard full of widgets
        loaded_once_ = true;      // doesn't fire every fetch at once on startup
        refresh();
    }
}

void OwnershipWidget::refresh() {
    status_->setText(tr("Loading…"));
    stack_->setCurrentIndex(0);
    services::GovDataService::instance().execute(script_, command_, args_, request_id_);
}

void OwnershipWidget::on_result(const QString& request_id, const services::GovDataResult& result) {
    if (request_id != request_id_)
        return;
    if (!result.success) {
        status_->setText(result.error.isEmpty() ? tr("Unavailable") : result.error);
        stack_->setCurrentIndex(0);
        return;
    }
    if (result.data.contains("error")) {
        const QString msg = result.data.value("error").toObject().value("error").toString();
        status_->setText(msg.isEmpty() ? tr("Parser unavailable") : msg);
        stack_->setCurrentIndex(0);
        return;
    }
    const QVector<QStringList> rows = mapper_(result.data);
    if (rows.isEmpty()) {
        status_->setText(empty_msg_);
        stack_->setCurrentIndex(0);
        return;
    }
    table_->clear_data();
    table_->set_headers(headers_);
    table_->set_data(rows);
    stack_->setCurrentIndex(1);
}

// ── Factories ────────────────────────────────────────────────────────────────

BaseWidget* create_insider_widget(const QJsonObject& cfg) {
    const QString ticker = cfg.value("ticker").toString("AAPL").toUpper();
    OwnershipRowMapper mapper = [](const QJsonObject& data) {
        QVector<QStringList> rows;
        for (const auto& v : data.value("data").toArray()) {
            const QJsonObject o = v.toObject();
            const double shares = o.value("shares").toDouble();
            rows.append({o.value("filing_date").toString(),
                         o.value("insider").toString(),
                         o.value("transaction_type").toString(),
                         shares != 0.0 ? QLocale().toString(shares, 'f', 0) : QString()});
        }
        return rows;
    };
    return new OwnershipWidget(
        QObject::tr("Insiders · %1").arg(ticker), "mcp/edgar/main.py", "insider_transactions",
        {ticker, "15"}, {QObject::tr("Date"), QObject::tr("Insider"), QObject::tr("Type"), QObject::tr("Shares")},
        "widget_insider", mapper, QObject::tr("No insider trades for %1.").arg(ticker));
}

BaseWidget* create_institution_widget(const QJsonObject& cfg) {
    const QString ticker = cfg.value("ticker").toString("BRK-B").toUpper();
    OwnershipRowMapper mapper = [](const QJsonObject& data) {
        QVector<QStringList> rows;
        for (const auto& v : data.value("data").toObject().value("top_holdings").toArray()) {
            const QJsonObject o = v.toObject();
            const double value = o.value("value").toDouble();
            rows.append({o.value("issuer").toString(),
                         value != 0.0 ? QString("$%L1").arg(value, 0, 'f', 0) : QString()});
        }
        return rows;
    };
    return new OwnershipWidget(
        QObject::tr("13F · %1").arg(ticker), "mcp/edgar/main.py", "13f_top_holdings",
        {ticker, "12"}, {QObject::tr("Issuer"), QObject::tr("Value")},
        "widget_institution", mapper, QObject::tr("No 13F holdings for %1.").arg(ticker));
}

BaseWidget* create_politician_widget(const QJsonObject& cfg) {
    const QString ticker = cfg.value("ticker").toString("AAPL").toUpper();
    OwnershipRowMapper mapper = [](const QJsonObject& data) {
        QVector<QStringList> rows;
        for (const auto& v : data.value("data").toArray()) {
            const QJsonObject o = v.toObject();
            rows.append({o.value("trade_date").toString(),
                         o.value("name").toString(),
                         o.value("trade_type").toString(),
                         o.value("size").toString()});
        }
        return rows;
    };
    return new OwnershipWidget(
        QObject::tr("Politicians · %1").arg(ticker), "ainvest_data.py", "congress",
        {ticker, "20"}, {QObject::tr("Date"), QObject::tr("Politician"), QObject::tr("Type"), QObject::tr("Amount")},
        "widget_politician", mapper, QObject::tr("No congress trades for %1.").arg(ticker));
}

} // namespace openmarketterminal::screens::widgets
