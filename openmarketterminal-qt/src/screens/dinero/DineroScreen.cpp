#include "screens/dinero/DineroScreen.h"

#include "services/dinero/DineroRpcClient.h"
#include "ui/theme/Theme.h"

#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#ifdef HAS_QT_WEBENGINE
#include <QWebEngineView>
#endif

namespace openmarketterminal::screens {

namespace {
const char* kReleasesUrl = "https://github.com/DineroLabs/dinero-v8/releases";
const char* kSiteUrl = "https://dinerolabs.org";
constexpr int kRefreshMs = 60 * 1000;

QPushButton* link_button(const QString& text, const QString& url, QWidget* parent) {
    auto* b = new QPushButton(text, parent);
    b->setCursor(Qt::PointingHandCursor);
    b->setObjectName("dineroLinkBtn");
    QObject::connect(b, &QPushButton::clicked, parent,
                     [url] { QDesktopServices::openUrl(QUrl(url)); });
    return b;
}
} // namespace

DineroScreen::DineroScreen(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);

    // ── Header ──
    auto* title = new QLabel(tr("DINERO"), this);
    title->setObjectName("dineroTitle");
    auto* tagline = new QLabel(
        tr("Post-quantum money for free people — a no-premine, open-source PoW chain. "
           "Read-only overview; OpenMarket Terminal does not buy, sell, or trade DIN."),
        this);
    tagline->setObjectName("dineroTagline");
    tagline->setWordWrap(true);
    root->addWidget(title);
    root->addWidget(tagline);

    // ── Live stat strip ──
    auto* strip = new QHBoxLayout();
    strip->setSpacing(8);
    strip->addWidget(make_stat_tile(tr("Block Height"), &stat_height_));
    strip->addWidget(make_stat_tile(tr("Circulating Supply"), &stat_supply_));
    strip->addWidget(make_stat_tile(tr("Block Time"), &stat_blocktime_));
    strip->addWidget(make_stat_tile(tr("Block Reward"), &stat_reward_));
    strip->addStretch();
    root->addLayout(strip);

    // ── What is Dinero (honest disclosure) ──
    auto* blurb = new QLabel(
        tr("Dinero is the maker of OpenMarket Terminal's own blockchain project: a "
           "post-quantum-native chain (ML-DSA signatures from genesis), proof-of-work with "
           "tail emission, no premine and no token sale. You participate by running a node or "
           "mining — not by buying. The data below is public chain information from the project's "
           "own explorer."),
        this);
    blurb->setObjectName("dineroBlurb");
    blurb->setWordWrap(true);
    root->addWidget(blurb);

    // ── Links ──
    auto* links = new QHBoxLayout();
    links->setSpacing(8);
    links->addWidget(link_button(tr("Block Explorer  ↗"),
                                 services::dinero::DineroRpcClient::explorerUrl(), this));
    links->addWidget(link_button(tr("Downloads & Releases  ↗"), kReleasesUrl, this));
    links->addWidget(link_button(tr("Project Site  ↗"), kSiteUrl, this));
    links->addStretch();
    root->addLayout(links);

    // ── Embedded explorer (or fallback) ──
#ifdef HAS_QT_WEBENGINE
    auto* web = new QWebEngineView(this);
    web->setUrl(QUrl(services::dinero::DineroRpcClient::explorerUrl()));
    root->addWidget(web, 1);
#else
    auto* fallback = new QFrame(this);
    auto* fl = new QVBoxLayout(fallback);
    fl->setAlignment(Qt::AlignCenter);
    auto* msg = new QLabel(tr("Embedded explorer needs Qt WebEngine."), fallback);
    msg->setAlignment(Qt::AlignCenter);
    fl->addWidget(msg);
    fl->addWidget(link_button(tr("Open Explorer in Browser  ↗"),
                              services::dinero::DineroRpcClient::explorerUrl(), fallback));
    root->addWidget(fallback, 1);
#endif

    setStyleSheet(
        QString("QLabel#dineroTitle { color:%1; font-size:22px; font-weight:700; letter-spacing:2px; }"
                "QLabel#dineroTagline { color:%2; font-size:12px; }"
                "QLabel#dineroBlurb { color:%2; font-size:12px; }"
                "QPushButton#dineroLinkBtn { color:%3; background:transparent; border:1px solid %3;"
                " border-radius:3px; padding:6px 10px; font-size:12px; }"
                "QPushButton#dineroLinkBtn:hover { color:%1; border-color:%1; }")
            .arg(ui::colors::TEXT_PRIMARY(), ui::colors::TEXT_TERTIARY(), ui::colors::AMBER()));

    // ── Live data ──
    client_ = new services::dinero::DineroRpcClient(this);
    connect(client_, &services::dinero::DineroRpcClient::statsReady, this, &DineroScreen::on_stats);

    auto* timer = new QTimer(this);
    timer->setInterval(kRefreshMs);
    connect(timer, &QTimer::timeout, client_, &services::dinero::DineroRpcClient::refresh);
    client_->refresh();
    timer->start();
}

QWidget* DineroScreen::make_stat_tile(const QString& label, QLabel** value_out) {
    auto* tile = new QFrame(this);
    auto* vl = new QVBoxLayout(tile);
    vl->setContentsMargins(10, 8, 10, 8);
    vl->setSpacing(2);
    auto* value = new QLabel(QStringLiteral("—"), tile);
    value->setObjectName("dineroTileValue");
    value->setStyleSheet(QString("color:%1; font-size:16px; font-weight:700;")
                             .arg(ui::colors::TEXT_PRIMARY()));
    auto* lab = new QLabel(label, tile);
    lab->setObjectName("dineroTileLabel");
    lab->setStyleSheet(QString("color:%1; font-size:10px;").arg(ui::colors::TEXT_TERTIARY()));
    vl->addWidget(value);
    vl->addWidget(lab);
    tile->setStyleSheet(QString("QFrame { background:%1; border:1px solid %2; border-radius:4px; }")
                            .arg(ui::colors::BG_SURFACE(), ui::colors::BORDER_DIM()));
    if (value_out)
        *value_out = value;
    return tile;
}

void DineroScreen::on_stats(const services::dinero::DineroStats& s) {
    if (!s.valid)
        return;
    const QLocale loc;
    stat_height_->setText(loc.toString(s.height));
    stat_supply_->setText(loc.toString(s.supply_din, 'f', 0) + QStringLiteral(" DIN"));
    stat_blocktime_->setText(tr("%1 min").arg(s.block_time_sec / 60));
    stat_reward_->setText(QString::number(s.reward_din, 'f', 0) + QStringLiteral(" DIN"));
}

} // namespace openmarketterminal::screens
