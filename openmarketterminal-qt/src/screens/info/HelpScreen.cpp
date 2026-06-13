#include "screens/info/HelpScreen.h"

#include "ui/theme/Theme.h"

#include <QCoreApplication>
#include <QEvent>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

namespace openmarketterminal::screens {

using namespace openmarketterminal::ui;

static const char* MF = "font-family:'Consolas','Courier New',monospace;";

HelpScreen::HelpScreen(QWidget* parent) : QWidget(parent) {
    setObjectName("HelpRoot");
    setStyleSheet(QString("QWidget#HelpRoot { background: %1; }").arg(colors::BG_BASE()));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    scroll_ = new QScrollArea;
    scroll_->setWidgetResizable(true);
    scroll_->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    scroll_->setWidget(build_page());
    root->addWidget(scroll_, 1);

    connect(&ui::ThemeManager::instance(), &ui::ThemeManager::theme_changed, this, [this](const ui::ThemeTokens&) {
        setStyleSheet(QString("QWidget#HelpRoot { background: %1; }").arg(colors::BG_BASE()));
        if (scroll_)
            scroll_->setWidget(build_page());
    });
}

void HelpScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange && scroll_)
        scroll_->setWidget(build_page());
    QWidget::changeEvent(event);
}

QWidget* HelpScreen::build_page() {
    auto* page = new QWidget;
    page->setStyleSheet(QString("background: %1;").arg(colors::BG_BASE()));

    auto* vl = new QVBoxLayout(page);
    vl->setContentsMargins(32, 28, 32, 32);
    vl->setSpacing(14);

    auto* title = new QLabel(tr("Open Terminal"));
    title->setStyleSheet(QString("color: %1; font-size: 26px; font-weight: 800; background: transparent; %2")
                             .arg(colors::AMBER(), MF));
    vl->addWidget(title);

    const QString version = QCoreApplication::applicationVersion().isEmpty()
                                ? QStringLiteral("0.1.0")
                                : QCoreApplication::applicationVersion();
    auto* version_lbl = new QLabel(tr("Version v%1").arg(version));
    version_lbl->setStyleSheet(QString("color: %1; font-size: 13px; font-weight: 700; background: transparent; %2")
                                   .arg(colors::TEXT_PRIMARY(), MF));
    vl->addWidget(version_lbl);

    auto* body = new QLabel(
        tr("Open Terminal is a local-first, open source market terminal for personal research, "
           "watchlists, portfolios, charts, news, and analytics."));
    body->setWordWrap(true);
    body->setStyleSheet(QString("color: %1; font-size: 13px; line-height: 1.4; background: transparent; %2")
                            .arg(colors::TEXT_SECONDARY(), MF));
    vl->addWidget(body);

    auto* note = new QLabel(tr("This build is yours to inspect, modify, and personalize."));
    note->setWordWrap(true);
    note->setStyleSheet(QString("color: %1; font-size: 12px; background: transparent; %2")
                            .arg(colors::TEXT_TERTIARY(), MF));
    vl->addWidget(note);

    vl->addStretch();
    return page;
}

} // namespace openmarketterminal::screens
