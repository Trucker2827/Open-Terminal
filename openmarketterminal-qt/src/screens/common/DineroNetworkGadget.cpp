#include "screens/common/DineroNetworkGadget.h"

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

namespace openmarketterminal::screens {

namespace {

QWidget* stat_cell(const QString& caption, QLabel** value_out, QWidget* parent) {
    auto* cell = new QWidget(parent);
    auto* layout = new QVBoxLayout(cell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* caption_label = new QLabel(caption, cell);
    caption_label->setStyleSheet(QStringLiteral("color:%1;font-size:7px;font-weight:700;letter-spacing:.6px;")
                                     .arg(ui::colors::TEXT_DIM()));
    auto* value = new QLabel(QStringLiteral("—"), cell);
    value->setStyleSheet(QStringLiteral("color:%1;font-size:10px;font-weight:800;")
                             .arg(ui::colors::AMBER()));
    layout->addWidget(caption_label);
    layout->addWidget(value);
    *value_out = value;
    return cell;
}

} // namespace

DineroNetworkGadget::DineroNetworkGadget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("dineroNetworkGadget"));
    setFixedHeight(88);
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Live public Dinero network telemetry. Links open in your default browser."));
    setStyleSheet(QStringLiteral(
        "#dineroNetworkGadget{background:%1;border-top:1px solid %2;border-bottom:1px solid %2;}"
        "QPushButton{background:transparent;border:none;color:%3;font-size:8px;font-weight:800;"
        "letter-spacing:.8px;text-align:left;padding:0;}"
        "QPushButton:hover{color:%4;}")
                      .arg(ui::colors::BG_RAISED(), ui::colors::BORDER_DIM(),
                           ui::colors::AMBER(), ui::colors::TEXT_PRIMARY()));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 4, 8, 4);
    root->setSpacing(3);

    auto* links = new QHBoxLayout;
    links->setSpacing(4);
    auto* site = new QPushButton(tr("DINERO NETWORK  ↗"), this);
    site->setCursor(Qt::PointingHandCursor);
    connect(site, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(services::dinero::DineroRpcClient::siteUrl()));
    });
    links->addWidget(site, 1);
    auto* explorer = new QPushButton(tr("EXPLORER  ↗"), this);
    explorer->setCursor(Qt::PointingHandCursor);
    explorer->setToolTip(tr("Open the Dinero block explorer in your default browser"));
    connect(explorer, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(services::dinero::DineroRpcClient::explorerUrl()));
    });
    links->addWidget(explorer);
    root->addLayout(links);

    auto* stats = new QHBoxLayout;
    stats->setSpacing(8);
    stats->addWidget(stat_cell(tr("HEIGHT"), &height_, this), 1);
    stats->addWidget(stat_cell(tr("SUPPLY"), &supply_, this), 1);
    stats->addWidget(stat_cell(tr("BLOCK"), &block_time_, this), 1);
    root->addLayout(stats);

    auto* disclosure = new QLabel(tr("PUBLIC RPC  •  READ ONLY  •  NO TRADING"), this);
    disclosure->setStyleSheet(QStringLiteral("color:%1;font-size:6px;letter-spacing:.45px;")
                                  .arg(ui::colors::TEXT_DIM()));
    root->addWidget(disclosure);

    client_ = new services::dinero::DineroRpcClient(this);
    connect(client_, &services::dinero::DineroRpcClient::statsReady,
            this, &DineroNetworkGadget::apply_stats);
    timer_ = new QTimer(this);
    timer_->setInterval(60 * 1000);
    connect(timer_, &QTimer::timeout, client_, &services::dinero::DineroRpcClient::refresh);
    client_->refresh();
    timer_->start();
}

void DineroNetworkGadget::apply_stats(const services::dinero::DineroStats& stats) {
    if (!stats.valid)
        return;
    const QLocale locale;
    height_->setText(locale.toString(stats.height));
    supply_->setText(locale.toString(stats.supply_din / 1000000.0, 'f', 2) + QStringLiteral("M"));
    block_time_->setText(tr("%1 min").arg(stats.block_time_sec / 60));
}

} // namespace openmarketterminal::screens
