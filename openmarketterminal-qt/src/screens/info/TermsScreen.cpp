#include "screens/info/TermsScreen.h"

#include "ui/theme/Theme.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace openmarketterminal::screens {

using namespace openmarketterminal::ui;

static const char* MF = "font-family:'Consolas','Courier New',monospace;";

static QLabel* section_heading(const QString& number, const QString& title) {
    auto* lbl = new QLabel(QString("%1. %2").arg(number, title));
    lbl->setStyleSheet(QString("color: %1; font-size: 14px; font-weight: 700; "
                               "background: transparent; margin-top: 8px; %2")
                           .arg(colors::AMBER(), MF));
    return lbl;
}

static QLabel* body_text(const QString& text) {
    auto* lbl = new QLabel(text);
    lbl->setWordWrap(true);
    lbl->setStyleSheet(QString("color: %1; font-size: 12px; line-height: 1.5; background: transparent; %2")
                           .arg(colors::TEXT_PRIMARY(), MF));
    return lbl;
}

TermsScreen::TermsScreen(QWidget* parent) : QWidget(parent) {
    setStyleSheet(QString("QWidget#TermsRoot { background: %1; }").arg(colors::BG_BASE()));
    setObjectName("TermsRoot");

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

void TermsScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange && scroll_) {
        scroll_->setWidget(build_page());
    }
    QWidget::changeEvent(event);
}

// ── Page builder ──────────────────────────────────────────────────────────────

QWidget* TermsScreen::build_page() {
    auto* page = new QWidget(this);
    page->setStyleSheet(QString("background: %1;").arg(colors::BG_BASE()));
    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(24, 24, 24, 24);
    vl->setSpacing(6);

    // Back button
    auto* back_btn = new QPushButton(tr("< BACK"));
    back_btn->setCursor(Qt::PointingHandCursor);
    back_btn->setStyleSheet(QString("QPushButton { color: %1; background: transparent; border: none; "
                                    "font-size: 12px; %2 } QPushButton:hover { color: %3; }")
                                .arg(colors::TEXT_SECONDARY(), MF, colors::TEXT_PRIMARY()));
    connect(back_btn, &QPushButton::clicked, this, &TermsScreen::navigate_back);
    vl->addWidget(back_btn, 0, Qt::AlignLeft);

    // Title
    auto* title = new QLabel(tr("TERMS OF SERVICE"));
    title->setStyleSheet(QString("color: %1; font-size: 20px; font-weight: 700; letter-spacing: 1px; "
                                 "background: transparent; %2")
                             .arg(colors::AMBER(), MF));
    vl->addWidget(title);

    auto* updated = new QLabel(tr("Applies to the open-source release."));
    updated->setStyleSheet(
        QString("color: %1; font-size: 11px; background: transparent; %2").arg(colors::TEXT_TERTIARY(), MF));
    vl->addWidget(updated);
    vl->addSpacing(12);

    // Panel container
    auto* panel = new QWidget(this);
    panel->setStyleSheet(QString("background: %1; border: 1px solid %2; border-radius: 2px;")
                             .arg(colors::BG_SURFACE(), colors::BORDER_DIM()));
    auto* pvl = new QVBoxLayout(panel);
    pvl->setContentsMargins(20, 16, 20, 16);
    pvl->setSpacing(6);

    // Section 1
    pvl->addWidget(section_heading("1", tr("FREE AND OPEN SOURCE")));
    pvl->addWidget(body_text(tr("Open Terminal is free, open-source software released under the MIT License. There is "
                                "no subscription, fee, or billing of any kind. You may use, copy, modify, and "
                                "redistribute it under the terms of that license.")));

    // Section 2
    pvl->addWidget(section_heading("2", tr("WHAT OPEN TERMINAL IS")));
    pvl->addWidget(body_text(tr("Open Terminal is a local-first desktop application that runs entirely on your own "
                                "computer. It is a research and educational tool — it does not provide financial "
                                "advice, brokerage services, or any guarantee about the data it displays.")));

    // Section 3
    pvl->addWidget(section_heading("3", tr("PROVIDED \"AS IS\"")));
    pvl->addWidget(body_text(tr("As stated in the MIT License, the software is provided \"AS IS\", without warranty "
                                "of any kind, express or implied. The authors and contributors are not liable for "
                                "any claim, damages, or other liability arising from the use of the software.")));

    // Section 4
    pvl->addWidget(section_heading("4", tr("NOT FINANCIAL ADVICE")));
    pvl->addWidget(body_text(tr("Market data, analytics, and AI-generated output are for informational and "
                                "educational purposes only and do not constitute investment advice. You are solely "
                                "responsible for your own trading and investment decisions and their outcomes.")));

    // Section 5
    pvl->addWidget(section_heading("5", tr("THIRD-PARTY SERVICES")));
    pvl->addWidget(
        body_text(tr("When you configure API keys, the app connects directly from your machine to the brokers and "
                     "data providers you choose. You are responsible for complying with the terms, fees, and usage "
                     "policies of any third-party service you connect, and for any orders you place through them.")));

    // Section 6
    pvl->addWidget(section_heading("6", tr("PRIVACY")));
    pvl->addWidget(body_text(tr("Open Terminal has no server and collects no personal data. Your data stays on your "
                                "device. See the Privacy Policy for details.")));

    // Section 7
    pvl->addWidget(section_heading("7", tr("CONTACT")));
    pvl->addWidget(body_text(tr("For questions about these Terms, open an issue in the project repository:")));
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

    auto* privacy_link = make_link(tr("Privacy Policy"));
    connect(privacy_link, &QPushButton::clicked, this, &TermsScreen::navigate_privacy);
    fhl->addWidget(privacy_link);

    fhl->addStretch();

    auto* contact_link = make_link(tr("Contact Us"));
    connect(contact_link, &QPushButton::clicked, this, &TermsScreen::navigate_contact);
    fhl->addWidget(contact_link);

    vl->addWidget(footer);
    vl->addStretch();

    return page;
}

} // namespace openmarketterminal::screens
