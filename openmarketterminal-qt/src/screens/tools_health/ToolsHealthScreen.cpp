#include "screens/tools_health/ToolsHealthScreen.h"

#include "core/tools/ToolsHealthCatalog.h"
#include "ui/theme/Theme.h"

#include <QEvent>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace openmarketterminal::screens {

namespace {

QString root_style() {
    return QString("QWidget#ToolsHealthRoot { background: %1; }"
                   "QLabel { font-family: 'Consolas','Courier New',monospace; }"
                   "QTableWidget { background: %1; color: %2; border: 1px solid %3; "
                   "  gridline-color: %3; font-family: 'Consolas','Courier New',monospace; font-size: 12px; }"
                   "QTableWidget::item { padding: 4px 8px; border-bottom: 1px solid %3; }"
                   "QTableWidget::item:selected { background: %4; color: %2; }"
                   "QHeaderView::section { background: %5; color: %6; border: none; "
                   "  border-right: 1px solid %3; border-bottom: 1px solid %3; "
                   "  padding: 6px 8px; font-size: 11px; font-weight: 700; "
                   "  font-family: 'Consolas','Courier New',monospace; }")
        .arg(ui::colors::BG_BASE(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(),
             ui::colors::BG_HOVER(), ui::colors::BG_RAISED(), ui::colors::TEXT_SECONDARY());
}

QString status_color(const QString& status) {
    if (status == QLatin1String("WORKING"))
        return ui::colors::POSITIVE;
    if (status == QLatin1String("STATIC"))
        return ui::colors::NEGATIVE;
    if (status == QLatin1String("POTENTIAL"))
        return ui::colors::WARNING;
    return ui::colors::AMBER;
}

QTableWidgetItem* item(const QString& text, const QString& color = {}) {
    auto* it = new QTableWidgetItem(text);
    it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    it->setForeground(QColor(color.isEmpty() ? QString(ui::colors::TEXT_PRIMARY) : color));
    return it;
}

QString yn(bool value) {
    return value ? QStringLiteral("YES") : QStringLiteral("NO");
}

} // namespace

ToolsHealthScreen::ToolsHealthScreen(QWidget* parent) : QWidget(parent) {
    setup_ui();
    populate();
}

void ToolsHealthScreen::setup_ui() {
    setObjectName(QStringLiteral("ToolsHealthRoot"));
    setStyleSheet(root_style());

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* header = new QWidget(this);
    header->setStyleSheet(QString("background: %1; border-bottom: 1px solid %2;")
                              .arg(ui::colors::BG_RAISED(), ui::colors::BORDER_DIM()));
    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(14, 10, 14, 10);
    h->setSpacing(16);

    auto* title_box = new QVBoxLayout;
    title_box->setSpacing(2);
    title_ = new QLabel(tr("TOOLS HEALTH"));
    title_->setStyleSheet(QString("color: %1; font-size: 16px; font-weight: 700; background: transparent;")
                              .arg(ui::colors::AMBER()));
    subtitle_ = new QLabel(tr("Menu coverage, CLI parity, MCP readiness, and stale surface audit"));
    subtitle_->setStyleSheet(QString("color: %1; font-size: 11px; background: transparent;")
                                 .arg(ui::colors::TEXT_SECONDARY()));
    title_box->addWidget(title_);
    title_box->addWidget(subtitle_);
    h->addLayout(title_box, 1);

    working_stat_ = make_stat(tr("WORKING"), QStringLiteral("0"), ui::colors::POSITIVE);
    cli_stat_ = make_stat(tr("CLI READY"), QStringLiteral("0"), ui::colors::CYAN);
    mcp_stat_ = make_stat(tr("MCP READY"), QStringLiteral("0"), ui::colors::AMBER);
    gaps_stat_ = make_stat(tr("GAPS"), QStringLiteral("0"), ui::colors::NEGATIVE);
    h->addWidget(working_stat_);
    h->addWidget(cli_stat_);
    h->addWidget(mcp_stat_);
    h->addWidget(gaps_stat_);
    root->addWidget(header);

    table_ = new QTableWidget(this);
    table_->setColumnCount(9);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setStyleSheet(table_->styleSheet() +
                          QString(" QTableWidget { alternate-background-color: %1; }")
                              .arg(ui::colors::BG_SURFACE()));
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Stretch);
    root->addWidget(table_, 1);
    retranslateUi();
}

QLabel* ToolsHealthScreen::make_stat(const QString& label, const QString& value, const QString& color) {
    auto* stat = new QLabel(QStringLiteral("%1\n%2").arg(value, label), this);
    stat->setAlignment(Qt::AlignCenter);
    stat->setMinimumWidth(96);
    stat->setStyleSheet(QString("color: %1; background: %2; border: 1px solid %3; "
                                "padding: 6px 10px; font-size: 10px; font-weight: 700;")
                            .arg(color, ui::colors::BG_BASE(), ui::colors::BORDER_DIM()));
    return stat;
}

void ToolsHealthScreen::populate() {
    int working = 0;
    int cli = 0;
    int mcp = 0;
    int gaps = 0;
    const auto& rows = tools_health::tools_menu_entries();
    table_->setRowCount(rows.size());
    for (int r = 0; r < rows.size(); ++r) {
        const auto& e = rows.at(r);
        const QString status = QString::fromLatin1(e.status);
        if (status == QLatin1String("WORKING"))
            ++working;
        if (e.cli)
            ++cli;
        if (e.mcp)
            ++mcp;
        if (status != QLatin1String("WORKING") || !e.cli || !e.mcp)
            ++gaps;

        table_->setItem(r, 0, item(status, status_color(status)));
        table_->setItem(r, 1, item(QString::fromLatin1(e.title)));
        table_->setItem(r, 2, item(QString::fromLatin1(e.lane), ui::colors::TEXT_SECONDARY));
        table_->setItem(r, 3, item(yn(e.gui), e.gui ? ui::colors::POSITIVE : ui::colors::NEGATIVE));
        table_->setItem(r, 4, item(yn(e.cli), e.cli ? ui::colors::POSITIVE : ui::colors::NEGATIVE));
        table_->setItem(r, 5, item(yn(e.mcp), e.mcp ? ui::colors::POSITIVE : ui::colors::NEGATIVE));
        table_->setItem(r, 6, item(yn(e.ai), e.ai ? ui::colors::POSITIVE : ui::colors::TEXT_TERTIARY));
        table_->setItem(r, 7, item(QString::number(e.tool_count), e.tool_count > 0 ? ui::colors::CYAN : ui::colors::TEXT_TERTIARY));
        table_->setItem(r, 8, item(QStringLiteral("%1 | %2").arg(QString::fromLatin1(e.command), QString::fromLatin1(e.notes)),
                                   ui::colors::TEXT_PRIMARY));
    }

    working_stat_->setText(QStringLiteral("%1\n%2").arg(working).arg(tr("WORKING")));
    cli_stat_->setText(QStringLiteral("%1\n%2").arg(cli).arg(tr("CLI READY")));
    mcp_stat_->setText(QStringLiteral("%1\n%2").arg(mcp).arg(tr("MCP READY")));
    gaps_stat_->setText(QStringLiteral("%1\n%2").arg(gaps).arg(tr("GAPS")));
}

void ToolsHealthScreen::retranslateUi() {
    if (title_)
        title_->setText(tr("TOOLS HEALTH"));
    if (subtitle_)
        subtitle_->setText(tr("Menu coverage, CLI parity, MCP readiness, and stale surface audit"));
    if (table_) {
        table_->setHorizontalHeaderLabels({tr("Status"), tr("Tool"), tr("Lane"), tr("GUI"), tr("CLI"),
                                           tr("MCP"), tr("AI"), tr("Tools"), tr("Command / Notes")});
    }
}

void ToolsHealthScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
        populate();
    }
    QWidget::changeEvent(event);
}

} // namespace openmarketterminal::screens
