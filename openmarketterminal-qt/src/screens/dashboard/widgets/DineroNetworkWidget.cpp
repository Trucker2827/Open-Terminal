#include "screens/dashboard/widgets/DineroNetworkWidget.h"

#include "services/dinero/DineroRpcClient.h"
#include "ui/theme/Theme.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace openmarketterminal::screens::widgets {

DineroNetworkWidget::DineroNetworkWidget(const QJsonObject& /*cfg*/, QWidget* parent)
    : BaseWidget(tr("DINERO NETWORK"), parent) {
    auto* vl = content_layout();
    vl->setContentsMargins(14, 12, 14, 12);
    vl->setSpacing(10);

    // Hero stat — block height
    vl->addWidget(make_stat(tr("BLOCK HEIGHT"), &height_, 26));

    // Row — supply + block time
    auto* row = new QHBoxLayout();
    row->setSpacing(14);
    row->addWidget(make_stat(tr("MONEY SUPPLY"), &supply_, 15), 1);
    row->addWidget(make_stat(tr("BLOCK TIME"), &blocktime_, 15), 1);
    vl->addLayout(row);

    vl->addWidget(make_stat(tr("BLOCK REWARD"), &reward_, 15));
    vl->addStretch();

    // Prominent GET DINERO button → dinerolabs.org
    get_btn_ = new QPushButton(tr("GET DINERO  ↗"), this);
    get_btn_->setCursor(Qt::PointingHandCursor);
    connect(get_btn_, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(services::dinero::DineroRpcClient::siteUrl()));
    });
    vl->addWidget(get_btn_);

    // Secondary — open the explorer in the browser
    exp_btn_ = new QPushButton(tr("Open Block Explorer  ↗"), this);
    exp_btn_->setCursor(Qt::PointingHandCursor);
    connect(exp_btn_, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(services::dinero::DineroRpcClient::explorerUrl()));
    });
    vl->addWidget(exp_btn_);

    client_ = new services::dinero::DineroRpcClient(this);
    connect(client_, &services::dinero::DineroRpcClient::statsReady, this, &DineroNetworkWidget::on_stats);

    timer_ = new QTimer(this);
    timer_->setInterval(60 * 1000);
    connect(timer_, &QTimer::timeout, client_, &services::dinero::DineroRpcClient::refresh);
    connect(this, &BaseWidget::refresh_requested, client_, &services::dinero::DineroRpcClient::refresh);

    on_theme_changed();
    client_->refresh();
    timer_->start();
}

QWidget* DineroNetworkWidget::make_stat(const QString& label, QLabel** value_out, int value_px) {
    auto* box = new QWidget(this);
    auto* l = new QVBoxLayout(box);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(1);
    auto* lab = new QLabel(label, box);
    lab->setStyleSheet(
        QString("color:%1; font-size:10px; letter-spacing:1px;").arg(ui::colors::TEXT_TERTIARY()));
    auto* val = new QLabel(QStringLiteral("—"), box);
    val->setStyleSheet(QString("color:%1; font-size:%2px; font-weight:700;")
                           .arg(ui::colors::AMBER())
                           .arg(value_px));
    l->addWidget(lab);
    l->addWidget(val);
    if (value_out)
        *value_out = val;
    return box;
}

void DineroNetworkWidget::on_stats(const services::dinero::DineroStats& s) {
    if (!s.valid)
        return;
    const QLocale loc;
    height_->setText(loc.toString(s.height));
    supply_->setText(loc.toString(s.supply_din, 'f', 0) + QStringLiteral(" DIN"));
    blocktime_->setText(tr("%1 min").arg(s.block_time_sec / 60));
    reward_->setText(QString::number(s.reward_din, 'f', 0) + QStringLiteral(" DIN"));
}

void DineroNetworkWidget::on_theme_changed() {
    // Style the buttons DIRECTLY (not via this widget's stylesheet) — BaseWidget
    // owns the widget-level stylesheet for theming and would otherwise wipe these.
    if (get_btn_)
        get_btn_->setStyleSheet(
            QString("QPushButton { background:%1; color:%2; border:none; border-radius:5px;"
                    " padding:11px; font-size:14px; font-weight:800; letter-spacing:1px; }"
                    "QPushButton:hover { background:%3; }")
                .arg(ui::colors::AMBER(), ui::colors::BG_BASE(), ui::colors::AMBER_DIM()));
    if (exp_btn_)
        exp_btn_->setStyleSheet(
            QString("QPushButton { background:transparent; color:%1; border:1px solid %1;"
                    " border-radius:5px; padding:8px; font-size:12px; }"
                    "QPushButton:hover { color:%2; border-color:%2; }")
                .arg(ui::colors::TEXT_TERTIARY(), ui::colors::AMBER()));
}

} // namespace openmarketterminal::screens::widgets
