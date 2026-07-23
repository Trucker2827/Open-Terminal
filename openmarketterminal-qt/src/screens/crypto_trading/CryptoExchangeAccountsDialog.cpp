#include "screens/crypto_trading/CryptoExchangeAccountsDialog.h"

#include "core/logging/Logger.h"
#include "screens/crypto_trading/CryptoCredentials.h"
#include "trading/ExchangeSession.h"
#include "trading/ExchangeSessionManager.h"
#include "ui/theme/Theme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

using openmarketterminal::trading::ExchangeCredentials;
using openmarketterminal::trading::ExchangeSessionManager;

namespace {
constexpr auto kTag = "CryptoExchangeAccounts";

QString display_name(const QString& id) {
    if (id == QLatin1String("coinbase"))
        return QStringLiteral("Coinbase Advanced");
    if (id == QLatin1String("kraken"))
        return QStringLiteral("Kraken Pro");
    if (id == QLatin1String("hyperliquid"))
        return QStringLiteral("Hyperliquid");
    if (id == QLatin1String("cryptocom"))
        return QStringLiteral("Crypto.com");
    return id.toUpper();
}

bool configured(const ExchangeCredentials& credentials) {
    return !credentials.api_key.trimmed().isEmpty() || !credentials.wallet_address.trimmed().isEmpty();
}

QString dialog_style() {
    using namespace openmarketterminal::ui;
    return QString(
               "QDialog { background:%1; color:%2; }"
               "QLabel#venueTitle { color:%3; font-size:18px; font-weight:800; }"
               "QLabel#venueIntro { color:%4; font-size:12px; }"
               "QFrame#venueFeatured { background:%5; border:1px solid %6; }"
               "QFrame#venueRow { background:%7; border:1px solid %8; }"
               "QLabel#venueName { color:%2; font-size:14px; font-weight:750; }"
               "QLabel#venueConnected { color:%9; font-weight:700; }"
               "QLabel#venueMissing { color:%10; font-weight:700; }"
               "QLabel#venueActive { color:%3; font-weight:800; }"
               "QLabel#venueStandby { color:%4; }"
               "QPushButton { min-height:28px; padding:0 12px; border:1px solid %8; "
               "color:%2; background:%7; font-weight:700; }"
               "QPushButton:hover { border-color:%3; color:%3; }"
               "QPushButton:disabled { color:%4; border-color:%8; }"
               "QPushButton#venueUse { color:%3; border-color:%6; }")
        .arg(colors::BG_BASE(), colors::TEXT_PRIMARY(), colors::AMBER(), colors::TEXT_SECONDARY(),
             colors::BG_RAISED(), colors::AMBER_DIM(), colors::BG_SURFACE(), colors::BORDER_DIM(),
             colors::POSITIVE(), colors::WARNING());
}
} // namespace

namespace openmarketterminal::screens::crypto {

CryptoExchangeAccountsDialog::CryptoExchangeAccountsDialog(const QString& active_exchange, QWidget* parent)
    : QDialog(parent), active_exchange_(active_exchange) {
    setWindowTitle(tr("Crypto Exchange Accounts"));
    setMinimumSize(650, 560);
    resize(720, 680);
    setStyleSheet(dialog_style());

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 18, 18, 18);
    root->setSpacing(10);

    auto* title = new QLabel(tr("CRYPTO EXCHANGE ACCOUNTS"));
    title->setObjectName(QStringLiteral("venueTitle"));
    root->addWidget(title);

    auto* intro = new QLabel(
        tr("Configure each venue once, then choose which venue the Crypto workspace uses for live trading. "
           "API secrets stay encrypted in that venue's secure storage and are never displayed here."));
    intro->setObjectName(QStringLiteral("venueIntro"));
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    auto* list = new QVBoxLayout(content);
    list->setContentsMargins(0, 4, 0, 4);
    list->setSpacing(8);

    // Put the two principal spot venues first. The remaining supported venues
    // stay available without making Coinbase/Kraken hard-coded special cases
    // in the session or credential layers.
    add_exchange_row(QStringLiteral("coinbase"), true);
    list->addWidget(findChild<QFrame*>(QStringLiteral("venue_coinbase")));
    add_exchange_row(QStringLiteral("kraken"), true);
    list->addWidget(findChild<QFrame*>(QStringLiteral("venue_kraken")));

    for (const QString& id : ExchangeSessionManager::supported_exchange_ids()) {
        if (id == QLatin1String("coinbase") || id == QLatin1String("kraken"))
            continue;
        add_exchange_row(id, false);
        list->addWidget(findChild<QFrame*>(QStringLiteral("venue_") + id));
    }
    list->addStretch();
    scroll->setWidget(content);
    root->addWidget(scroll, 1);

    auto* footer = new QHBoxLayout;
    auto* note = new QLabel(tr("Changing venue does not copy credentials or stop other market-data sessions."));
    note->setObjectName(QStringLiteral("venueIntro"));
    footer->addWidget(note, 1);
    auto* close = new QPushButton(tr("DONE"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    footer->addWidget(close);
    root->addLayout(footer);

    refresh_rows();
}

void CryptoExchangeAccountsDialog::add_exchange_row(const QString& exchange_id, bool featured) {
    auto* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("venue_") + exchange_id);
    frame->setProperty("class", featured ? QStringLiteral("featured") : QStringLiteral("standard"));
    // Qt selectors cannot match the dynamic class above consistently across
    // styles, so use the named row style directly.
    frame->setStyleSheet(featured
                             ? QStringLiteral("QFrame { background:rgba(217,119,6,0.07); border:1px solid rgba(217,119,6,0.45); }")
                             : QStringLiteral("QFrame { background:rgba(255,255,255,0.02); border:1px solid rgba(255,255,255,0.10); }"));

    auto* layout = new QHBoxLayout(frame);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(12);

    auto* identity = new QVBoxLayout;
    auto* name = new QLabel(display_name(exchange_id));
    name->setObjectName(QStringLiteral("venueName"));
    identity->addWidget(name);
    auto* status = new QLabel;
    identity->addWidget(status);
    layout->addLayout(identity, 1);

    auto* role = new QLabel;
    role->setMinimumWidth(90);
    layout->addWidget(role);

    auto* configure = new QPushButton(tr("CONFIGURE"));
    connect(configure, &QPushButton::clicked, this, [this, exchange_id]() { configure_exchange(exchange_id); });
    layout->addWidget(configure);

    auto* use = new QPushButton(tr("USE FOR TRADING"));
    use->setObjectName(QStringLiteral("venueUse"));
    connect(use, &QPushButton::clicked, this, [this, exchange_id]() { select_exchange(exchange_id); });
    layout->addWidget(use);

    rows_.insert(exchange_id, RowWidgets{status, role, use, configure});
}

void CryptoExchangeAccountsDialog::configure_exchange(const QString& exchange_id) {
    auto* session = ExchangeSessionManager::instance().session(exchange_id);
    const ExchangeCredentials existing = session->get_credentials();
    CryptoCredentials dialog(exchange_id, this);
    if (configured(existing))
        dialog.mark_connected(existing.api_key, existing.password, existing.wallet_address);

    connect(&dialog, &CryptoCredentials::credentials_saved, this,
            [session, existing, exchange_id](const QString& key, const QString& secret, const QString& password,
                                             const QString& wallet, const QString& private_key) {
                ExchangeCredentials updated;
                updated.api_key = key;
                updated.secret = secret.isEmpty() ? existing.secret : secret;
                updated.password = password;
                updated.wallet_address = wallet;
                updated.private_key = private_key.isEmpty() ? existing.private_key : private_key;
                session->set_credentials(updated);
                LOG_INFO(kTag, QStringLiteral("Credentials saved for %1").arg(exchange_id));
            });
    dialog.exec();
    refresh_rows();
}

void CryptoExchangeAccountsDialog::select_exchange(const QString& exchange_id) {
    if (exchange_id == active_exchange_)
        return;
    active_exchange_ = exchange_id;
    refresh_rows();
    emit active_exchange_requested(exchange_id);
}

void CryptoExchangeAccountsDialog::refresh_rows() {
    for (auto it = rows_.begin(); it != rows_.end(); ++it) {
        const bool has_credentials =
            configured(ExchangeSessionManager::instance().session(it.key())->get_credentials());
        it->status->setText(has_credentials ? tr("CONNECTED · credentials stored")
                                            : tr("NOT CONFIGURED · market data only"));
        it->status->setObjectName(has_credentials ? QStringLiteral("venueConnected")
                                                  : QStringLiteral("venueMissing"));
        it->status->style()->unpolish(it->status);
        it->status->style()->polish(it->status);

        const bool active = it.key() == active_exchange_;
        it->role->setText(active ? tr("ACTIVE VENUE") : tr("STANDBY"));
        it->role->setObjectName(active ? QStringLiteral("venueActive") : QStringLiteral("venueStandby"));
        it->role->style()->unpolish(it->role);
        it->role->style()->polish(it->role);
        it->use->setEnabled(!active);
        it->use->setText(active ? tr("IN USE") : tr("USE FOR TRADING"));
        it->configure->setText(has_credentials ? tr("EDIT CREDENTIALS") : tr("ADD CREDENTIALS"));
    }
}

} // namespace openmarketterminal::screens::crypto
