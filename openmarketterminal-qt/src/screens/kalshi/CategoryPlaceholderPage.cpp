#include "screens/kalshi/CategoryPlaceholderPage.h"

#include "ui/theme/Theme.h"

#include <QLabel>
#include <QVBoxLayout>

namespace openmarketterminal::screens::kalshi {
using namespace openmarketterminal::ui;

CategoryPlaceholderPage::CategoryPlaceholderPage(const QString& category, QWidget* parent)
    : QWidget(parent), category_(category) {
    build_ui();
    update_text();
}

void CategoryPlaceholderPage::build_ui() {
    setObjectName(QStringLiteral("kalshiCategoryPlaceholder"));
    setStyleSheet(QStringLiteral("#kalshiCategoryPlaceholder { background:%1; }")
                      .arg(colors::BG_BASE()));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->addStretch();

    message_ = new QLabel(this);
    message_->setAlignment(Qt::AlignCenter);
    message_->setWordWrap(true);
    message_->setStyleSheet(QStringLiteral("color:%1;font-size:14px;font-weight:700;")
                                 .arg(colors::TEXT_SECONDARY()));
    layout->addWidget(message_);

    layout->addStretch();
}

void CategoryPlaceholderPage::set_category(const QString& category) {
    category_ = category;
    update_text();
}

void CategoryPlaceholderPage::update_text() {
    if (!message_) return;
    const QString category = category_.trimmed().isEmpty() ? QStringLiteral("this category") : category_;
    message_->setText(QStringLiteral("No dedicated view yet for %1").arg(category));
}

} // namespace openmarketterminal::screens::kalshi
