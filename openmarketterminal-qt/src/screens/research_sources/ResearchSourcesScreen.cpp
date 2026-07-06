#include "screens/research_sources/ResearchSourcesScreen.h"

#include "core/research/ResearchSourceCatalog.h"
#include "ui/theme/Theme.h"

#include <QEvent>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace openmarketterminal::screens {

namespace {

QString root_style() {
    return QString("QWidget#ResearchSourcesRoot { background: %1; }"
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
    if (status.startsWith(QLatin1String("CONFIRMED")))
        return ui::colors::POSITIVE;
    if (status == QLatin1String("OPTIONAL_KEY") || status == QLatin1String("OPTIONAL_CONFIG"))
        return ui::colors::WARNING;
    if (status == QLatin1String("LOCAL") || status == QLatin1String("LOCAL_DEMO"))
        return ui::colors::CYAN;
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

QString key_label(const research_sources::Entry& e) {
    const QString config = QString::fromLatin1(e.config_key);
    const QString credential = QString::fromLatin1(e.credential_key);
    if (!config.isEmpty())
        return config;
    if (!credential.isEmpty())
        return credential;
    return e.keyless ? QStringLiteral("KEYLESS") : QStringLiteral("-");
}

} // namespace

ResearchSourcesScreen::ResearchSourcesScreen(QWidget* parent) : QWidget(parent) {
    setup_ui();
    populate();
}

void ResearchSourcesScreen::setup_ui() {
    setObjectName(QStringLiteral("ResearchSourcesRoot"));
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
    title_ = new QLabel(tr("RESEARCH SOURCES"));
    title_->setStyleSheet(QString("color: %1; font-size: 16px; font-weight: 700; background: transparent;")
                              .arg(ui::colors::AMBER()));
    subtitle_ = new QLabel(tr("Feed provenance, local/remote status, CLI parity, MCP readiness, and daemon candidates"));
    subtitle_->setStyleSheet(QString("color: %1; font-size: 11px; background: transparent;")
                                 .arg(ui::colors::TEXT_SECONDARY()));
    title_box->addWidget(title_);
    title_box->addWidget(subtitle_);
    h->addLayout(title_box, 1);

    confirmed_stat_ = make_stat(tr("CONFIRMED"), QStringLiteral("0"), ui::colors::POSITIVE);
    keyless_stat_ = make_stat(tr("KEYLESS"), QStringLiteral("0"), ui::colors::CYAN);
    cli_stat_ = make_stat(tr("CLI READY"), QStringLiteral("0"), ui::colors::AMBER);
    mcp_stat_ = make_stat(tr("MCP READY"), QStringLiteral("0"), ui::colors::AMBER);
    h->addWidget(confirmed_stat_);
    h->addWidget(keyless_stat_);
    h->addWidget(cli_stat_);
    h->addWidget(mcp_stat_);
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

QLabel* ResearchSourcesScreen::make_stat(const QString& label, const QString& value, const QString& color) {
    auto* stat = new QLabel(QStringLiteral("%1\n%2").arg(value, label), this);
    stat->setAlignment(Qt::AlignCenter);
    stat->setMinimumWidth(96);
    stat->setStyleSheet(QString("color: %1; background: %2; border: 1px solid %3; "
                                "padding: 6px 10px; font-size: 10px; font-weight: 700;")
                            .arg(color, ui::colors::BG_BASE(), ui::colors::BORDER_DIM()));
    return stat;
}

void ResearchSourcesScreen::populate() {
    int confirmed = 0;
    int keyless = 0;
    int cli = 0;
    int mcp = 0;
    const auto& rows = research_sources::entries();
    table_->setRowCount(rows.size());
    for (int r = 0; r < rows.size(); ++r) {
        const auto& e = rows.at(r);
        const QString status = QString::fromLatin1(e.status);
        if (status.startsWith(QLatin1String("CONFIRMED")))
            ++confirmed;
        if (e.keyless)
            ++keyless;
        if (e.cli)
            ++cli;
        if (e.mcp)
            ++mcp;

        table_->setItem(r, 0, item(status, status_color(status)));
        table_->setItem(r, 1, item(QString::fromLatin1(e.title)));
        table_->setItem(r, 2, item(QString::fromLatin1(e.panel), ui::colors::TEXT_SECONDARY));
        table_->setItem(r, 3, item(QString::fromLatin1(e.source_type), ui::colors::TEXT_SECONDARY));
        table_->setItem(r, 4, item(key_label(e), e.keyless ? ui::colors::POSITIVE : ui::colors::WARNING));
        table_->setItem(r, 5, item(yn(e.cli), e.cli ? ui::colors::POSITIVE : ui::colors::NEGATIVE));
        table_->setItem(r, 6, item(yn(e.mcp), e.mcp ? ui::colors::POSITIVE : ui::colors::NEGATIVE));
        table_->setItem(r, 7, item(yn(e.daemon_candidate),
                                   e.daemon_candidate ? ui::colors::POSITIVE : ui::colors::TEXT_TERTIARY));
        table_->setItem(r, 8, item(QStringLiteral("%1 | %2").arg(QString::fromLatin1(e.verify_command),
                                                                 QString::fromLatin1(e.notes)),
                                   ui::colors::TEXT_PRIMARY));
    }

    confirmed_stat_->setText(QStringLiteral("%1\n%2").arg(confirmed).arg(tr("CONFIRMED")));
    keyless_stat_->setText(QStringLiteral("%1\n%2").arg(keyless).arg(tr("KEYLESS")));
    cli_stat_->setText(QStringLiteral("%1\n%2").arg(cli).arg(tr("CLI READY")));
    mcp_stat_->setText(QStringLiteral("%1\n%2").arg(mcp).arg(tr("MCP READY")));
}

void ResearchSourcesScreen::retranslateUi() {
    if (title_)
        title_->setText(tr("RESEARCH SOURCES"));
    if (subtitle_)
        subtitle_->setText(tr("Feed provenance, local/remote status, CLI parity, MCP readiness, and daemon candidates"));
    if (table_) {
        table_->setHorizontalHeaderLabels({tr("Status"), tr("Source"), tr("Panel"), tr("Type"), tr("Key"),
                                           tr("CLI"), tr("MCP"), tr("Daemon"), tr("Verify / Notes")});
    }
}

void ResearchSourcesScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
        populate();
    }
    QWidget::changeEvent(event);
}

} // namespace openmarketterminal::screens
