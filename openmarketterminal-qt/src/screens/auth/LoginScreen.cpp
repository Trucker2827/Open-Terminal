#include "screens/auth/LoginScreen.h"

#include "ui/theme/Theme.h"
#include "ui/widgets/LanguageSwitcher.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

namespace openmarketterminal::screens {

// ── Obsidian Design System Styles ────────────────────────────────────────────

static QString card_style() {
    return QString("background: %1; border: 1px solid %2;").arg(ui::colors::BG_SURFACE(), ui::colors::BORDER_DIM());
}

static QString btn_primary() {
    return QString("QPushButton {"
                   "  background: rgba(217,119,6,0.1); color: %1;"
                   "  border: 1px solid %2;"
                   "  padding: 0 16px; font-size: 14px; font-weight: 700;"
                   "  font-family: 'Consolas','Courier New',monospace;"
                   "}"
                   "QPushButton:hover { background: %1; color: %3; }")
        .arg(ui::colors::AMBER(), ui::colors::AMBER_DIM(), ui::colors::BG_BASE());
}

static QString muted_style() {
    return QString("color: %1; font-size: 13px; background: transparent;"
                   "font-family: 'Consolas','Courier New',monospace;")
        .arg(ui::colors::TEXT_TERTIARY());
}

static QFrame* make_separator() {
    auto* sep = new QFrame;
    sep->setFixedHeight(1);
    sep->setStyleSheet(QString("background: %1; border: none;").arg(ui::colors::BORDER_DIM()));
    return sep;
}

// ── Constructor ──────────────────────────────────────────────────────────────

LoginScreen::LoginScreen(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    // Top-right language picker — available before entering the shell so users
    // can change the UI language. Posts QEvent::LanguageChange app-wide, which
    // retranslates this screen via changeEvent below.
    auto* top_row = new QHBoxLayout;
    top_row->setContentsMargins(20, 20, 20, 0);
    top_row->addStretch();
    top_row->addWidget(new ui::LanguageSwitcher(this));
    root->addLayout(top_row);

    auto* overlay = new QVBoxLayout;
    overlay->setAlignment(Qt::AlignCenter);
    root->addLayout(overlay, 1);

    auto* card = new QWidget(this);
    card->setStyleSheet(card_style());
    card->setMinimumWidth(480);
    card->setMaximumWidth(620);

    auto* vl = new QVBoxLayout(card);
    vl->setContentsMargins(28, 22, 28, 22);
    vl->setSpacing(10);

    // Header bar
    auto* header = new QWidget(this);
    header->setFixedHeight(38);
    header->setStyleSheet(QString("background: %1; border: none;").arg(ui::colors::BG_RAISED()));
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(14, 0, 14, 0);

    login_title_ = new QLabel;
    login_title_->setStyleSheet(QString("color: %1; font-size: 14px; font-weight: 700;"
                                        "background: transparent; letter-spacing: 1px;"
                                        "font-family: 'Consolas','Courier New',monospace;")
                                    .arg(ui::colors::AMBER()));
    hl->addWidget(login_title_);
    hl->addStretch();

    // Brand mark — not translated.
    auto* brand = new QLabel(QStringLiteral("OPENMARKETTERMINAL"));
    brand->setStyleSheet(QString("color: %1; font-size: 12px; font-weight: 700;"
                                 "background: transparent; letter-spacing: 0.5px;"
                                 "font-family: 'Consolas','Courier New',monospace;")
                             .arg(ui::colors::TEXT_DIM()));
    hl->addWidget(brand);
    vl->addWidget(header);

    login_subtitle_ = new QLabel;
    login_subtitle_->setWordWrap(true);
    login_subtitle_->setStyleSheet(muted_style());
    vl->addWidget(login_subtitle_);

    vl->addWidget(make_separator());

    guest_btn_ = new QPushButton;
    guest_btn_->setFixedHeight(38);
    guest_btn_->setStyleSheet(btn_primary());
    guest_btn_->setCursor(Qt::PointingHandCursor);
    connect(guest_btn_, &QPushButton::clicked, this, &LoginScreen::continue_as_guest);
    vl->addWidget(guest_btn_);

    overlay->addWidget(card, 0, Qt::AlignCenter);

    retranslateUi();
}

// ── Background Paint ─────────────────────────────────────────────────────────

void LoginScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    QWidget::changeEvent(event);
}

void LoginScreen::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.fillRect(rect(), QColor(ui::colors::BG_BASE()));

    p.setPen(QPen(QColor(ui::colors::BG_RAISED()), 1));
    const int step = 24;
    for (int x = 0; x < width(); x += step)
        p.drawLine(x, 0, x, height());
    for (int y = 0; y < height(); y += step)
        p.drawLine(0, y, width(), y);

    int cx = width() / 2;
    int cy = height() / 2;
    p.setPen(QPen(QColor(ui::colors::BORDER_DIM()), 1));
    p.drawLine(cx - 60, cy, cx + 60, cy);
    p.drawLine(cx, cy - 60, cx, cy + 60);
}

// ── Re-translation ───────────────────────────────────────────────────────────

void LoginScreen::retranslateUi() {
    if (login_title_)    login_title_->setText(tr("WELCOME"));
    if (login_subtitle_) login_subtitle_->setText(tr("Your local-first markets terminal. No account required."));
    if (guest_btn_)      guest_btn_->setText(tr("  CONTINUE AS GUEST  "));
}

} // namespace openmarketterminal::screens
