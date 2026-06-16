// AiActivitySection.cpp — Settings → "AI Activity" tab.
// Loads TradeAuditRepository::recent(200) on construction and prepends live rows
// when AiActivityNotifier::activity fires.

#include "screens/settings/AiActivitySection.h"

#include "app/AiActivityNotifier.h"
#include "screens/settings/SettingsStyles.h"
#include "storage/repositories/TradeAuditRepository.h"
#include "trading/ai_activity/AiActivityFormat.h"
#include "ui/theme/Theme.h"

#include <QBrush>
#include <QColor>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

namespace openmarketterminal::screens::settings {

using namespace openmarketterminal::ui;
using namespace openmarketterminal::trading;

namespace {

constexpr int kFlashMs = 600;

// Map Severity → foreground color for the Decision cell.
QColor severity_color(ActivityView::Severity sev) {
    switch (sev) {
        case ActivityView::Severity::Success: return QColor(colors::POSITIVE());
        case ActivityView::Severity::Error:   return QColor(colors::NEGATIVE());
        case ActivityView::Severity::Warning: return QColor(colors::AMBER());
        case ActivityView::Severity::Info:
        default:                              return QColor(colors::CYAN());
    }
}

// Brief tint color for the new-row flash.
QColor severity_tint(ActivityView::Severity sev) {
    QColor c = severity_color(sev);
    c.setAlpha(70);
    return c;
}

} // namespace

AiActivitySection::AiActivitySection(QWidget* parent) : QWidget(parent) {
    using namespace settings_styles;

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(8);

    // Section title
    auto* title = new QLabel(tr("AI ACTIVITY"));
    title->setStyleSheet(section_title_ss());
    root->addWidget(title);

    auto* sub = new QLabel(tr("Log of every AI trading decision. Newest actions appear at the top."));
    sub->setWordWrap(true);
    sub->setStyleSheet(label_ss());
    root->addWidget(sub);

    // Table
    table_ = new QTableWidget;
    table_->setColumnCount(6);
    table_->setHorizontalHeaderLabels(
        {tr("Time"), tr("Tool"), tr("Account / Mode"), tr("Action"), tr("Decision"), tr("Reason")});
    table_->horizontalHeader()->setStretchLastSection(true);  // Reason column stretches
    table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setShowGrid(false);
    table_->setSortingEnabled(false);
    table_->setStyleSheet(
        QString("QTableWidget {"
                "  background: %1; color: %2; border: none; font-size: 11px;"
                "}"
                "QTableWidget::item {"
                "  padding: 4px 8px; border-bottom: 1px solid %3;"
                "}"
                "QTableWidget::item:selected { background: %4; color: %2; }"
                "QHeaderView::section {"
                "  background: %5; color: %6; border: none;"
                "  border-bottom: 1px solid %3;"
                "  padding: 5px 8px; font-size: 9px; font-weight: 700; letter-spacing: 0.5px;"
                "}"
                "QScrollBar:vertical { background: %1; width: 4px; border: none; }"
                "QScrollBar::handle:vertical { background: %3; min-height: 20px; }"
                "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }")
            .arg(colors::BG_BASE(), colors::TEXT_PRIMARY(), colors::BORDER_DIM(),
                 colors::BG_HOVER(), colors::BG_RAISED(), colors::TEXT_SECONDARY()));
    root->addWidget(table_, 1);

    load_recent();

    // Connect to the singleton notifier so live rows appear in real time.
    connect(&app::AiActivityNotifier::instance(), &app::AiActivityNotifier::activity,
            this, &AiActivitySection::add_activity);
}

void AiActivitySection::load_recent() {
    auto result = TradeAuditRepository::instance().recent(kCap);
    if (result.is_err())
        return;  // leave table empty on error — no crash

    const QVector<TradeAuditRow>& rows = result.value();
    table_->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        const ActivityView view = format_activity(rows[i]);
        insert_row(i, view);
    }
    table_->resizeColumnToContents(0);  // Time
    table_->resizeColumnToContents(1);  // Tool
    table_->resizeColumnToContents(2);  // Account/Mode
    table_->resizeColumnToContents(3);  // Action
    table_->resizeColumnToContents(4);  // Decision
    // Reason stretches via horizontalHeader()->setStretchLastSection(true)
}

void AiActivitySection::insert_row(int at, const ActivityView& view) {
    table_->insertRow(at);

    auto make_item = [](const QString& text) -> QTableWidgetItem* {
        return new QTableWidgetItem(text);
    };

    table_->setItem(at, 0, make_item(view.time));
    table_->setItem(at, 1, make_item(view.tool));
    table_->setItem(at, 2, make_item(view.account_mode));
    table_->setItem(at, 3, make_item(view.action));

    auto* decision_item = make_item(view.decision);
    decision_item->setForeground(QBrush(severity_color(view.severity)));
    table_->setItem(at, 4, decision_item);

    table_->setItem(at, 5, make_item(view.reason));
}

void AiActivitySection::add_activity(const ActivityView& view) {
    // Prepend the new row at index 0 (newest first).
    insert_row(0, view);

    // Trim rows beyond the cap — remove the last (oldest) entry.
    if (table_->rowCount() > kCap)
        table_->removeRow(table_->rowCount() - 1);

    // Flash the new row briefly per severity.
    const QColor tint = severity_tint(view.severity);
    for (int c = 0; c < table_->columnCount(); ++c) {
        if (auto* it = table_->item(0, c))
            it->setBackground(QBrush(tint));
    }
    QPointer<QTableWidget> guard = table_;
    QTimer::singleShot(kFlashMs, table_, [guard]() {
        if (!guard || guard->rowCount() == 0) return;
        for (int c = 0; c < guard->columnCount(); ++c) {
            if (auto* it = guard->item(0, c))
                it->setBackground(QBrush(Qt::NoBrush));
        }
    });
}

} // namespace openmarketterminal::screens::settings
