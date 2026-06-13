#include "screens/info/PrivacyScreen.h"

#include "ui/theme/Theme.h"

#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace openmarketterminal::screens {

using namespace openmarketterminal::ui;

static const char* MF = "font-family:'Consolas','Courier New',monospace;";

static QLabel* section_heading(const QString& icon, const QString& title) {
    auto* lbl = new QLabel(QString("%1  %2").arg(icon, title));
    lbl->setStyleSheet(QString("color: %1; font-size: 14px; font-weight: 700; "
                               "background: transparent; margin-top: 8px; %2")
                           .arg(colors::AMBER(), MF));
    return lbl;
}

static QLabel* body_text(const QString& text) {
    auto* lbl = new QLabel(text);
    lbl->setWordWrap(true);
    lbl->setStyleSheet(
        QString("color: %1; font-size: 12px; background: transparent; %2").arg(colors::TEXT_PRIMARY(), MF));
    return lbl;
}

static QLabel* bullet(const QString& text) {
    auto* lbl = new QLabel(QString("  > %1").arg(text));
    lbl->setWordWrap(true);
    lbl->setStyleSheet(
        QString("color: %1; font-size: 12px; background: transparent; %2").arg(colors::TEXT_SECONDARY(), MF));
    return lbl;
}

static QWidget* info_card(const QString& title, const QString& desc) {
    auto* card = new QWidget(nullptr);
    card->setStyleSheet(QString("background: %1; border: 1px solid %2; border-radius: 2px;")
                            .arg(colors::BG_SURFACE(), colors::BORDER_DIM()));
    auto* vl = new QVBoxLayout(card);
    vl->setContentsMargins(12, 10, 12, 10);
    vl->setSpacing(4);

    auto* t = new QLabel(title);
    t->setStyleSheet(
        QString("color: %1; font-size: 11px; font-weight: 700; background: transparent; %2").arg(colors::AMBER(), MF));
    vl->addWidget(t);

    auto* d = new QLabel(desc);
    d->setWordWrap(true);
    d->setStyleSheet(
        QString("color: %1; font-size: 11px; background: transparent; %2").arg(colors::TEXT_SECONDARY(), MF));
    vl->addWidget(d);

    return card;
}

PrivacyScreen::PrivacyScreen(QWidget* parent) : QWidget(parent) {
    setStyleSheet(QString("QWidget#PrivacyRoot { background: %1; }").arg(colors::BG_BASE()));
    setObjectName("PrivacyRoot");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    scroll_ = new QScrollArea;
    scroll_->setWidgetResizable(true);
    scroll_->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    scroll_->setWidget(build_page());
    root->addWidget(scroll_, 1);
}

// ── Re-translation ────────────────────────────────────────────────────────────
// Static legal text — rebuild the page on language change rather than caching
// every label as a member. QScrollArea::setWidget() deletes the old content.

void PrivacyScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange && scroll_) {
        scroll_->setWidget(build_page());
    }
    QWidget::changeEvent(event);
}

// ── Page builder ──────────────────────────────────────────────────────────────

QWidget* PrivacyScreen::build_page() {
    auto* page = new QWidget(this);
    page->setStyleSheet(QString("background: %1;").arg(colors::BG_BASE()));
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(24, 24, 24, 24);
    vl->setSpacing(6);

    // Back
    auto* back_btn = new QPushButton(tr("< BACK"));
    back_btn->setCursor(Qt::PointingHandCursor);
    back_btn->setStyleSheet(QString("QPushButton { color: %1; background: transparent; border: none; "
                                    "font-size: 12px; %2 } QPushButton:hover { color: %3; }")
                                .arg(colors::TEXT_SECONDARY(), MF, colors::TEXT_PRIMARY()));
    connect(back_btn, &QPushButton::clicked, this, &PrivacyScreen::navigate_back);
    vl->addWidget(back_btn, 0, Qt::AlignLeft);

    auto* title = new QLabel(tr("PRIVACY POLICY"));
    title->setStyleSheet(QString("color: %1; font-size: 20px; font-weight: 700; letter-spacing: 1px; "
                                 "background: transparent; %2")
                             .arg(colors::AMBER(), MF));
    vl->addWidget(title);

    auto* updated = new QLabel(tr("Applies to the open-source release."));
    updated->setStyleSheet(
        QString("color: %1; font-size: 11px; background: transparent; %2").arg(colors::TEXT_TERTIARY(), MF));
    vl->addWidget(updated);
    vl->addSpacing(12);

    // Main panel
    auto* panel = new QWidget(this);
    panel->setStyleSheet(QString("background: %1; border: 1px solid %2; border-radius: 2px;")
                             .arg(colors::BG_SURFACE(), colors::BORDER_DIM()));
    auto* pvl = new QVBoxLayout(panel);
    pvl->setContentsMargins(20, 16, 20, 16);
    pvl->setSpacing(6);

    // 1 — Commitment
    pvl->addWidget(section_heading("#", tr("LOCAL-FIRST BY DESIGN")));
    pvl->addWidget(
        body_text(tr("Open Terminal is a local-first desktop app that runs entirely on your computer. There is no "
                     "Open Terminal server. We do not collect, transmit, sell, or share your personal data, and the "
                     "app contains no analytics, telemetry, or phone-home of any kind.")));

    // 2 — What stays on your device
    pvl->addWidget(section_heading("@", tr("YOUR DATA STAYS ON YOUR DEVICE")));
    pvl->addWidget(body_text(tr("Everything you create in the app is stored locally on your machine:")));
    pvl->addWidget(bullet(tr("Watchlists, portfolios, notes, and settings — in local SQLite databases")));
    pvl->addWidget(bullet(tr("Secrets such as API keys — in your operating system's keychain")));
    pvl->addWidget(bullet(tr("Nothing is uploaded to us; there is no account to register and no cloud sync")));

    // 3 — API keys and third-party services
    pvl->addWidget(section_heading("*", tr("API KEYS AND THIRD-PARTY SERVICES")));
    {
        auto* grid = new QGridLayout;
        grid->setSpacing(8);
        grid->addWidget(
            info_card(tr("STORED LOCALLY"),
                      tr("API keys you configure are kept on your device and are never sent to us")),
            0, 0);
        grid->addWidget(
            info_card(tr("DIRECT CONNECTIONS"),
                      tr("Keys are used only to connect directly from your machine to the brokers and data "
                         "providers you choose")),
            0, 1);
        grid->addWidget(info_card(tr("THEIR POLICIES APPLY"),
                                  tr("Those third parties handle your requests under their own privacy policies — "
                                     "review them separately")),
                        1, 0);
        grid->addWidget(
            info_card(tr("YOU DECIDE"),
                      tr("No connection is made unless you add a key and choose to use that service")),
            1, 1);
        pvl->addLayout(grid);
    }

    // 4 — No data collection
    pvl->addWidget(section_heading("~", tr("NO COLLECTION, NO SHARING")));
    pvl->addWidget(body_text(tr("Because there is no server and no telemetry:")));
    pvl->addWidget(bullet(tr("We do not collect names, emails, phone numbers, payment details, or location")));
    pvl->addWidget(bullet(tr("We do not track feature usage, navigation, or session activity")));
    pvl->addWidget(bullet(tr("We have no data to sell, share with partners, or hand over")));

    // 5 — Security
    pvl->addWidget(section_heading("!", tr("KEEPING YOUR DATA SAFE")));
    pvl->addWidget(body_text(tr("Your data's security rests with your own machine:")));
    pvl->addWidget(bullet(tr("Secrets are stored in the OS keychain rather than plain text")));
    pvl->addWidget(bullet(tr("Local databases live under your user profile, protected by your OS account")));
    pvl->addWidget(bullet(tr("Keeping your device and operating system secure protects your data")));

    // 6 — Transparency
    pvl->addWidget(section_heading("=", tr("OPEN AND INSPECTABLE")));
    pvl->addWidget(body_text(tr("Open Terminal is open-source software:")));
    pvl->addWidget(bullet(tr("You can read the source to see exactly how your data is handled")));
    pvl->addWidget(bullet(tr("You control your local data — back it up, export it, or delete it at any time")));
    pvl->addWidget(bullet(tr("Removing the app's data folders removes your data completely")));

    // 7 — Contact
    pvl->addWidget(section_heading("@", tr("QUESTIONS")));
    pvl->addWidget(body_text(tr("Privacy questions can be raised in the public issue tracker:")));
    pvl->addWidget(body_text(tr("github.com/Trucker2827/Open-Terminal/issues")));

    vl->addWidget(panel);

    // Footer navigation
    auto* footer = new QWidget(this);
    footer->setStyleSheet("background: transparent;");
    auto* fhl = new QHBoxLayout(footer);
    fhl->setContentsMargins(0, 12, 0, 0);

    auto make_link = [](const QString& text) {
        auto* btn = new QPushButton(text);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(QString("QPushButton { color: %1; background: transparent; border: none; "
                                   "font-size: 12px; font-family:'Consolas','Courier New',monospace; }"
                                   "QPushButton:hover { color: #38bdf8; }")
                               .arg(colors::CYAN()));
        return btn;
    };

    auto* terms_link = make_link(tr("Terms of Service"));
    connect(terms_link, &QPushButton::clicked, this, &PrivacyScreen::navigate_terms);
    fhl->addWidget(terms_link);

    fhl->addStretch();

    auto* contact_link = make_link(tr("Contact Us"));
    connect(contact_link, &QPushButton::clicked, this, &PrivacyScreen::navigate_contact);
    fhl->addWidget(contact_link);

    vl->addWidget(footer);
    vl->addStretch();

    return page;
}

} // namespace openmarketterminal::screens
