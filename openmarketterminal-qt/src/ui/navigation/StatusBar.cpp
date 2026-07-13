#include "ui/navigation/StatusBar.h"

#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QCoreApplication>
#include <QEvent>

namespace openmarketterminal::ui {

StatusBar::StatusBar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(26);
    setObjectName("appStatusBar");

    auto* hl = new QHBoxLayout(this);
    hl->setContentsMargins(12, 0, 12, 0);
    hl->setSpacing(0);

    auto mk = [](const QString& t, const QString& name) {
        auto* l = new QLabel(t);
        l->setObjectName(name);
        return l;
    };

    hl->addWidget(mk("v" + QCoreApplication::applicationVersion(), "sbVersion"));
    hl->addWidget(mk("  |  ", "sbSep"));
    const char* feeds[] = {"EQ", "FX", "CM", "FI", "CR"};
    for (auto& f : feeds) {
        hl->addWidget(mk(f, "sbFeed"));
        hl->addWidget(mk(" ", "sbSpacer"));
    }
    hl->addStretch();

    ready_label_ = mk({}, "sbReady");
    hl->addWidget(ready_label_);

    retranslateUi();

    connect(&ThemeManager::instance(), &ThemeManager::theme_changed, this,
            [this](const ThemeTokens&) { refresh_theme(); });
    refresh_theme();
}

void StatusBar::changeEvent(QEvent* e) {
    if (e->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QWidget::changeEvent(e);
}

void StatusBar::retranslateUi() {
    if (ready_label_)
        ready_label_->setText(ready_state_ ? tr("READY") : tr("BUSY"));
}

void StatusBar::refresh_theme() {
    setStyleSheet(QString("#appStatusBar { background:%1; border-top:1px solid %2; }"
                          "#sbVersion { color:%3; background:transparent; }"
                          "#sbSep { color:%2; background:transparent; }"
                          "#sbFeed { color:%3; background:transparent; }"
                          "#sbSpacer { background:transparent; }"
                          "#sbReady { color:%4; font-weight:700; background:transparent; }")
                      .arg(colors::BG_BASE())
                      .arg(colors::BORDER_DIM())
                      .arg(colors::TEXT_DIM())
                      .arg(colors::POSITIVE()));
}

void StatusBar::set_ready(bool ready) {
    ready_state_ = ready;
    ready_label_->setText(ready ? tr("READY") : tr("BUSY"));
    ready_label_->setStyleSheet(QString("color:%1;font-weight:700;background:transparent;")
                                    .arg(ready ? colors::POSITIVE() : colors::TEXT_TERTIARY()));
}

} // namespace openmarketterminal::ui
