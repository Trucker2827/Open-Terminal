// src/screens/code_editor/CodeEditorScreen_Library.cpp
// Research overview backed by the active profile's real evidence store.

#include "screens/code_editor/CodeEditorScreen.h"

#include "core/config/AppPaths.h"
#include "services/notebooks/NotebookLibraryService.h"
#include "storage/sqlite/Database.h"
#include "ui/theme/Theme.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSplitter>
#include <QSqlQuery>
#include <QTableWidgetItem>
#include <QTimeZone>
#include <QVBoxLayout>

namespace openmarketterminal::screens {

using namespace openmarketterminal::ui;
using openmarketterminal::services::NotebookLibraryService;

namespace {

QPushButton* research_button(const QString& text, QWidget* parent, const char* object_name = "nbSecondaryBtn") {
    auto* button = new QPushButton(text, parent);
    button->setObjectName(object_name);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QTableWidget* research_table(const QStringList& headers, QWidget* parent) {
    auto* table = new QTableWidget(parent);
    table->setObjectName("nbLabTable");
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(28);
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}

QTableWidgetItem* research_item(const QString& text, const QString& tooltip = {}) {
    auto* item = new QTableWidgetItem(text);
    if (!tooltip.isEmpty())
        item->setToolTip(tooltip);
    return item;
}

QWidget* section(QWidget* parent, const QString& title, QTableWidget* table) {
    auto* panel = new QWidget(parent);
    panel->setObjectName("nbResearchSection");
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* label = new QLabel(title, panel);
    label->setObjectName("nbSectionTitle");
    label->setFixedHeight(30);
    layout->addWidget(label);
    layout->addWidget(table, 1);
    return panel;
}

QWidget* metric_panel(QWidget* parent, const QString& label, QLabel*& value) {
    auto* panel = new QWidget(parent);
    panel->setObjectName("nbMetricPanel");
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 7, 12, 7);
    layout->setSpacing(1);
    value = new QLabel(QStringLiteral("--"), panel);
    value->setObjectName("nbMetricValue");
    layout->addWidget(value);
    auto* caption = new QLabel(label, panel);
    caption->setObjectName("nbMetricLabel");
    layout->addWidget(caption);
    return panel;
}

QString utc_time(qint64 ms) {
    if (ms <= 0)
        return QStringLiteral("--");
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(QStringLiteral("MM-dd HH:mm:ss"));
}

QString money(double value) {
    return QStringLiteral("%1$%2").arg(value > 0.0 ? QStringLiteral("+") : QString()).arg(value, 0, 'f', 2);
}

QString percent(double value) {
    if (value < 0.0)
        return QStringLiteral("--");
    return QStringLiteral("%1%").arg(value * 100.0, 0, 'f', 1);
}

QString freshness(qint64 created_at) {
    if (created_at <= 0)
        return QStringLiteral("NO DATA");
    const qint64 age_ms = QDateTime::currentMSecsSinceEpoch() - created_at;
    if (age_ms <= 5 * 60 * 1000)
        return QStringLiteral("LIVE");
    if (age_ms <= 24 * 60 * 60 * 1000)
        return QStringLiteral("RECENT");
    return QStringLiteral("STALE");
}

QVariant scalar(const QString& sql) {
    auto result = Database::instance().execute(sql);
    if (result.is_ok() && result.value().next())
        return result.value().value(0);
    return {};
}

} // namespace

QWidget* CodeEditorScreen::build_library_page() {
    auto* page = new QWidget(this);
    page->setObjectName("nbLabPage");
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* command_bar = new QWidget(page);
    command_bar->setObjectName("nbLabBand");
    auto* commands = new QHBoxLayout(command_bar);
    commands->setContentsMargins(14, 6, 12, 6);
    commands->setSpacing(6);
    auto* title = new QLabel(tr("OPEN TERMINAL EVIDENCE WORKSPACE"), command_bar);
    title->setObjectName("nbLabTitle");
    commands->addWidget(title);
    research_status_summary_ = new QLabel(command_bar);
    research_status_summary_->setObjectName("nbLabSummary");
    commands->addWidget(research_status_summary_);
    commands->addStretch(1);
    auto* refresh = research_button(tr("REFRESH"), command_bar);
    connect(refresh, &QPushButton::clicked, this, &CodeEditorScreen::refresh_research_overview);
    commands->addWidget(refresh);
    auto* strategies = research_button(tr("OPEN STRATEGIES"), command_bar, "nbAccentBtn");
    connect(strategies, &QPushButton::clicked, this, &CodeEditorScreen::open_strategy_workspace);
    commands->addWidget(strategies);
    root->addWidget(command_bar);

    auto* workflow_bar = new QWidget(page);
    workflow_bar->setObjectName("nbWorkflowBand");
    auto* workflows = new QHBoxLayout(workflow_bar);
    workflows->setContentsMargins(14, 5, 14, 5);
    workflows->setSpacing(5);
    const QList<QPair<QString, QString>> actions = {
        {tr("KALSHI CODEX V3"), QStringLiteral("kalshi_advisor_v3")},
        {tr("KALSHI CALIBRATION"), QStringLiteral("kalshi_calibration")},
        {tr("COINBASE SPOT"), QStringLiteral("coinbase_spot")},
        {tr("KRAKEN MAKER"), QStringLiteral("kraken_maker")},
        {tr("CHRONOS-2 HORIZONS"), QStringLiteral("chronos_horizons")},
        {tr("AI DECISION AUDIT"), QStringLiteral("ai_decision_audit")},
        {tr("TRADE POSTMORTEM"), QStringLiteral("trade_postmortem")},
    };
    for (const auto& action : actions) {
        auto* button = research_button(action.first, workflow_bar, "nbWorkflowBtn");
        button->setToolTip(tr("Create a read-only notebook from this profile's real %1 evidence").arg(action.first));
        connect(button, &QPushButton::clicked, this, [this, type = action.second]() {
            QString reference;
            if (type == QStringLiteral("trade_postmortem")) {
                const int row = research_decisions_table_ ? research_decisions_table_->currentRow() : -1;
                if (row < 0) {
                    set_view(TradeReviewsView);
                    return;
                }
                reference = research_decisions_table_->item(row, 0)->data(Qt::UserRole).toString();
            }
            create_research_notebook(type, reference);
        });
        workflows->addWidget(button);
    }
    workflows->addStretch(1);
    root->addWidget(workflow_bar);

    auto* metrics = new QWidget(page);
    metrics->setObjectName("nbMetricsBand");
    auto* metrics_layout = new QHBoxLayout(metrics);
    metrics_layout->setContentsMargins(0, 0, 0, 0);
    metrics_layout->setSpacing(0);
    const QStringList metric_labels = {tr("ACTIVE PROOF BOOKS"), tr("DECISIONS"), tr("RESOLVED SAMPLES"),
                                       tr("OPEN PAPER POSITIONS"), tr("COST-NET PAPER P/L"), tr("LOCAL REPORTS")};
    for (const QString& label : metric_labels) {
        QLabel* value = nullptr;
        metrics_layout->addWidget(metric_panel(metrics, label, value), 1);
        research_metric_values_.append(value);
    }
    root->addWidget(metrics);

    research_books_table_ = research_table({tr("PROOF BOOK"), tr("LANE"), tr("UNIVERSE"), tr("STATE"),
                                             tr("POSITIONS"), tr("RESOLVED"), tr("OPEN"), tr("NET P/L"),
                                             tr("LATEST")}, page);
    research_books_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    research_books_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    connect(research_books_table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const QString id = research_books_table_->item(row, 0)->data(Qt::UserRole).toString();
        create_research_notebook(QStringLiteral("strategy_experiment"), id);
    });

    research_decisions_table_ = research_table({tr("TIME"), tr("SOURCE"), tr("VENUE"), tr("SYMBOL"),
                                                 tr("HORIZON"), tr("CALL"), tr("GATE"), tr("MODEL"),
                                                 tr("MARKET"), tr("NET EDGE"), tr("CONF"), tr("WHY")}, page);
    research_decisions_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    research_decisions_table_->horizontalHeader()->setSectionResizeMode(11, QHeaderView::Stretch);
    connect(research_decisions_table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const QString id = research_decisions_table_->item(row, 0)->data(Qt::UserRole).toString();
        create_research_notebook(QStringLiteral("trade_postmortem"), id);
    });

    auto* live_split = new QSplitter(Qt::Horizontal, page);
    live_split->setHandleWidth(1);
    live_split->addWidget(section(live_split, tr("PROOF BOOKS · DOUBLE-CLICK TO OPEN AN IMMUTABLE EXPERIMENT"),
                                  research_books_table_));
    live_split->addWidget(section(live_split, tr("LATEST DECISION EVIDENCE · MODEL VS MARKET AFTER COST"),
                                  research_decisions_table_));
    live_split->setStretchFactor(0, 4);
    live_split->setStretchFactor(1, 6);

    research_outputs_table_ = research_table({tr("RESEARCH OUTPUT"), tr("TYPE"), tr("DATA CUTOFF"),
                                               tr("MODIFIED"), tr("PATH")}, page);
    research_outputs_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    research_outputs_table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    connect(research_outputs_table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const QString path = research_outputs_table_->item(row, 0)->data(Qt::UserRole).toString();
        if (!path.isEmpty())
            open_notebook_path(path);
    });

    auto* vertical = new QSplitter(Qt::Vertical, page);
    vertical->setHandleWidth(1);
    vertical->addWidget(live_split);
    vertical->addWidget(section(vertical, tr("RECENT LOCAL RESEARCH OUTPUTS · DOUBLE-CLICK TO OPEN"),
                                research_outputs_table_));
    vertical->setStretchFactor(0, 4);
    vertical->setStretchFactor(1, 1);
    vertical->setSizes({650, 190});
    root->addWidget(vertical, 1);
    return page;
}

void CodeEditorScreen::refresh_research_overview() {
    if (!research_books_table_ || !research_decisions_table_ || !research_outputs_table_)
        return;

    const QString filter = lib_search_text_.trimmed();
    const QString like = QStringLiteral("%") + filter + QStringLiteral("%");
    research_books_table_->setRowCount(0);
    research_decisions_table_->setRowCount(0);
    research_outputs_table_->setRowCount(0);

    const int books = scalar(QStringLiteral("SELECT COUNT(*) FROM sandbox_strategy WHERE status='active'")).toInt();
    const int decisions = scalar(QStringLiteral("SELECT COUNT(*) FROM edge_decision_journal")).toInt();
    const int resolved = scalar(QStringLiteral("SELECT COUNT(*) FROM sandbox_position WHERE state='closed' "
                                                       "AND realized_pnl IS NOT NULL AND data_quality='ok'")).toInt();
    const int open = scalar(QStringLiteral("SELECT COUNT(*) FROM sandbox_position "
                                                   "WHERE state IN ('open','pending_fill')")).toInt();
    const double net_pnl = scalar(QStringLiteral("SELECT COALESCE(SUM(realized_pnl),0) FROM sandbox_position "
                                                         "WHERE state='closed' AND data_quality='ok'")).toDouble();
    const QDir report_dir(AppPaths::data() + QStringLiteral("/notebooks"));
    const QFileInfoList report_files = report_dir.entryInfoList({QStringLiteral("*.ipynb")}, QDir::Files, QDir::Time);

    const QStringList metric_values = {QString::number(books), QString::number(decisions), QString::number(resolved),
                                       QString::number(open), money(net_pnl), QString::number(report_files.size())};
    for (int i = 0; i < research_metric_values_.size() && i < metric_values.size(); ++i) {
        research_metric_values_[i]->setText(metric_values[i]);
        if (i == 4)
            research_metric_values_[i]->setProperty("positive", net_pnl >= 0.0);
        research_metric_values_[i]->style()->unpolish(research_metric_values_[i]);
        research_metric_values_[i]->style()->polish(research_metric_values_[i]);
    }

    auto book_rows = Database::instance().execute(
        QStringLiteral("SELECT s.strategy_id,s.kind,s.symbols,s.status,COUNT(p.position_id),"
                       "SUM(CASE WHEN p.state='closed' AND p.realized_pnl IS NOT NULL THEN 1 ELSE 0 END),"
                       "SUM(CASE WHEN p.state IN ('open','pending_fill') THEN 1 ELSE 0 END),"
                       "COALESCE(SUM(CASE WHEN p.state='closed' THEN p.realized_pnl ELSE 0 END),0),"
                       "MAX(COALESCE(p.created_at,s.created_at)) "
                       "FROM sandbox_strategy s LEFT JOIN sandbox_position p ON p.strategy_id=s.strategy_id "
                       "WHERE (?='' OR s.strategy_id LIKE ? OR s.kind LIKE ? OR s.symbols LIKE ?) "
                       "GROUP BY s.strategy_id ORDER BY MAX(COALESCE(p.created_at,s.created_at)) DESC LIMIT 120"),
        {filter, like, like, like});
    if (book_rows.is_ok()) {
        while (book_rows.value().next()) {
            const int row = research_books_table_->rowCount();
            research_books_table_->insertRow(row);
            const QString id = book_rows.value().value(0).toString();
            auto* id_item = research_item(id.left(24), id);
            id_item->setData(Qt::UserRole, id);
            research_books_table_->setItem(row, 0, id_item);
            research_books_table_->setItem(row, 1, research_item(book_rows.value().value(1).toString()));
            research_books_table_->setItem(row, 2, research_item(book_rows.value().value(2).toString()));
            research_books_table_->setItem(row, 3, research_item(book_rows.value().value(3).toString().toUpper()));
            research_books_table_->setItem(row, 4, research_item(QString::number(book_rows.value().value(4).toInt())));
            research_books_table_->setItem(row, 5, research_item(QString::number(book_rows.value().value(5).toInt())));
            research_books_table_->setItem(row, 6, research_item(QString::number(book_rows.value().value(6).toInt())));
            const double pnl = book_rows.value().value(7).toDouble();
            auto* pnl_item = research_item(money(pnl));
            pnl_item->setForeground(QColor(pnl >= 0.0 ? colors::POSITIVE() : colors::NEGATIVE()));
            research_books_table_->setItem(row, 7, pnl_item);
            research_books_table_->setItem(row, 8, research_item(utc_time(book_rows.value().value(8).toLongLong())));
        }
    }

    auto decision_rows = Database::instance().execute(
        QStringLiteral("SELECT id,created_at,source,venue,symbol,horizon,call,gate,model_probability,"
                       "market_probability,edge_after_cost,confidence,reasons FROM edge_decision_journal "
                       "WHERE (?='' OR source LIKE ? OR venue LIKE ? OR symbol LIKE ? OR call LIKE ? OR reasons LIKE ?) "
                       "ORDER BY created_at DESC LIMIT 180"),
        {filter, like, like, like, like, like});
    qint64 latest_decision = 0;
    if (decision_rows.is_ok()) {
        while (decision_rows.value().next()) {
            const int row = research_decisions_table_->rowCount();
            research_decisions_table_->insertRow(row);
            const QString id = decision_rows.value().value(0).toString();
            const qint64 created_at = decision_rows.value().value(1).toLongLong();
            latest_decision = qMax(latest_decision, created_at);
            auto* time_item = research_item(utc_time(created_at), id);
            time_item->setData(Qt::UserRole, id);
            research_decisions_table_->setItem(row, 0, time_item);
            research_decisions_table_->setItem(row, 1, research_item(decision_rows.value().value(2).toString()));
            research_decisions_table_->setItem(row, 2, research_item(decision_rows.value().value(3).toString()));
            research_decisions_table_->setItem(row, 3, research_item(decision_rows.value().value(4).toString()));
            research_decisions_table_->setItem(row, 4, research_item(decision_rows.value().value(5).toString()));
            research_decisions_table_->setItem(row, 5, research_item(decision_rows.value().value(6).toString().toUpper()));
            research_decisions_table_->setItem(row, 6, research_item(decision_rows.value().value(7).toString().toUpper()));
            research_decisions_table_->setItem(row, 7, research_item(percent(decision_rows.value().value(8).toDouble())));
            research_decisions_table_->setItem(row, 8, research_item(percent(decision_rows.value().value(9).toDouble())));
            const double edge = decision_rows.value().value(10).toDouble();
            auto* edge_item = research_item(percent(edge));
            edge_item->setForeground(QColor(edge >= 0.0 ? colors::POSITIVE() : colors::NEGATIVE()));
            research_decisions_table_->setItem(row, 9, edge_item);
            research_decisions_table_->setItem(row, 10, research_item(percent(decision_rows.value().value(11).toDouble())));
            research_decisions_table_->setItem(row, 11, research_item(decision_rows.value().value(12).toString()));
        }
    }

    for (const QFileInfo& info : report_files.mid(0, 30)) {
        QString type = QStringLiteral("NOTEBOOK");
        QString cutoff = QStringLiteral("--");
        QFile file(info.absoluteFilePath());
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonObject metadata = QJsonDocument::fromJson(file.readAll()).object()
                                             .value(QStringLiteral("metadata")).toObject()
                                             .value(QStringLiteral("openmarketterminal")).toObject();
            type = metadata.value(QStringLiteral("research_type")).toString(type).toUpper().replace('_', ' ');
            cutoff = metadata.value(QStringLiteral("data_cutoff_utc")).toString(cutoff);
        }
        if (!filter.isEmpty() && !info.completeBaseName().contains(filter, Qt::CaseInsensitive) &&
            !type.contains(filter, Qt::CaseInsensitive))
            continue;
        const int row = research_outputs_table_->rowCount();
        research_outputs_table_->insertRow(row);
        auto* output = research_item(info.completeBaseName());
        output->setData(Qt::UserRole, info.absoluteFilePath());
        research_outputs_table_->setItem(row, 0, output);
        research_outputs_table_->setItem(row, 1, research_item(type));
        research_outputs_table_->setItem(row, 2, research_item(cutoff));
        research_outputs_table_->setItem(row, 3, research_item(info.lastModified().toUTC().toString(QStringLiteral("MM-dd HH:mm"))));
        research_outputs_table_->setItem(row, 4, research_item(info.absoluteFilePath()));
    }

    research_status_summary_->setText(tr("%1 · %2 decisions shown · latest feed %3")
                                          .arg(freshness(latest_decision))
                                          .arg(research_decisions_table_->rowCount())
                                          .arg(utc_time(latest_decision)));
}

void CodeEditorScreen::populate_library() {
    refresh_research_overview();
}

void CodeEditorScreen::relayout_library_cards() {
    // The evidence workspace uses splitters and tables, so no card reflow is required.
}

void CodeEditorScreen::on_library_search(const QString& text) {
    lib_search_text_ = text;
    refresh_research_overview();
}

void CodeEditorScreen::on_open_library_entry(int catalog_index) {
    const auto& catalog = NotebookLibraryService::instance().catalog();
    if (catalog_index < 0 || catalog_index >= catalog.size())
        return;
    const QString path = NotebookLibraryService::instance().working_copy_for(catalog[catalog_index]);
    if (!path.isEmpty())
        open_notebook_path(path);
}

} // namespace openmarketterminal::screens
