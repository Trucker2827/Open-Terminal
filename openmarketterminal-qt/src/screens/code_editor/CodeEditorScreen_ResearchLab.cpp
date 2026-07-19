// Project-connected Research Lab pages for the notebook screen.

#include "screens/code_editor/CodeEditorScreen.h"

#include "core/config/AppPaths.h"
#include "core/config/ProfileManager.h"
#include "core/events/EventBus.h"
#include "core/logging/Logger.h"
#include "services/sandbox/SandboxEligibility.h"
#include "services/sandbox/SandboxRegistry.h"
#include "services/sandbox/SandboxScorer.h"
#include "storage/sqlite/Database.h"
#include "ui/theme/Theme.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlQuery>
#include <QTableWidgetItem>
#include <QTimeZone>
#include <QVBoxLayout>

#include <algorithm>

namespace openmarketterminal::screens {

using namespace openmarketterminal::ui;
namespace sandbox = openmarketterminal::services::sandbox;

namespace {

QPushButton* lab_button(const QString& text, QWidget* parent, const char* object_name = "nbSecondaryBtn") {
    auto* button = new QPushButton(text, parent);
    button->setObjectName(object_name);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QTableWidget* lab_table(const QStringList& headers, QWidget* parent) {
    auto* table = new QTableWidget(parent);
    table->setObjectName("nbLabTable");
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->setAlternatingRowColors(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setShowGrid(false);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(30);
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}

QWidget* lab_band(QWidget* parent, const QString& title, QLabel*& summary, QHBoxLayout*& actions) {
    auto* band = new QWidget(parent);
    band->setObjectName("nbLabBand");
    auto* layout = new QHBoxLayout(band);
    layout->setContentsMargins(14, 7, 12, 7);
    layout->setSpacing(8);
    auto* title_label = new QLabel(title, band);
    title_label->setObjectName("nbLabTitle");
    layout->addWidget(title_label);
    summary = new QLabel(band);
    summary->setObjectName("nbLabSummary");
    layout->addWidget(summary);
    layout->addStretch(1);
    actions = layout;
    return band;
}

QTableWidgetItem* table_item(const QString& text, const QString& tooltip = {}) {
    auto* item = new QTableWidgetItem(text);
    if (!tooltip.isEmpty())
        item->setToolTip(tooltip);
    return item;
}

QString money(double value) {
    const QString sign = value > 0.0 ? QStringLiteral("+") : QString();
    return QStringLiteral("%1$%2").arg(sign).arg(value, 0, 'f', 2);
}

QString utc_time(qint64 ms) {
    if (ms <= 0)
        return QStringLiteral("--");
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'"));
}

QJsonArray source_lines(const QString& text) {
    QJsonArray result;
    const QStringList lines = text.split('\n');
    for (int i = 0; i < lines.size(); ++i)
        result.append(i + 1 < lines.size() ? lines[i] + QLatin1Char('\n') : lines[i]);
    return result;
}

QJsonObject markdown_cell(const QString& text) {
    return QJsonObject{{QStringLiteral("cell_type"), QStringLiteral("markdown")},
                       {QStringLiteral("metadata"), QJsonObject{}},
                       {QStringLiteral("source"), source_lines(text)}};
}

QJsonObject code_cell(const QString& text) {
    return QJsonObject{{QStringLiteral("cell_type"), QStringLiteral("code")},
                       {QStringLiteral("execution_count"), QJsonValue::Null},
                       {QStringLiteral("metadata"), QJsonObject{}},
                       {QStringLiteral("outputs"), QJsonArray{}},
                       {QStringLiteral("source"), source_lines(text)}};
}

QString notebook_slug(QString value) {
    value = value.toLower();
    value.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
    return value.trimmed().remove(QRegularExpression(QStringLiteral("^_+|_+$")));
}

QString python_literal(QString value) {
    value.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    value.replace(QStringLiteral("'"), QStringLiteral("\\'"));
    return QStringLiteral("'%1'").arg(value);
}

} // namespace

QWidget* CodeEditorScreen::build_experiments_page() {
    auto* page = new QWidget(this);
    page->setObjectName("nbLabPage");
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    QHBoxLayout* actions = nullptr;
    root->addWidget(lab_band(page, tr("LIVE EXPERIMENTS"), experiments_summary_, actions));
    auto* create = lab_button(tr("CREATE EXPERIMENT NOTEBOOK"), page, "nbPrimaryBtn");
    connect(create, &QPushButton::clicked, this, [this]() {
        QString reference;
        if (experiments_table_ && experiments_table_->currentRow() >= 0)
            reference = experiments_table_->item(experiments_table_->currentRow(), 0)->data(Qt::UserRole).toString();
        create_research_notebook(QStringLiteral("strategy_experiment"), reference);
    });
    actions->addWidget(create);
    auto* strategies = lab_button(tr("OPEN STRATEGIES"), page, "nbAccentBtn");
    connect(strategies, &QPushButton::clicked, this, &CodeEditorScreen::open_strategy_workspace);
    actions->addWidget(strategies);
    auto* refresh = lab_button(tr("REFRESH"), page);
    connect(refresh, &QPushButton::clicked, this, &CodeEditorScreen::refresh_experiments);
    actions->addWidget(refresh);

    experiments_table_ = lab_table({tr("BOOK"), tr("KIND"), tr("SYMBOLS"), tr("SOURCE"), tr("HORIZON"),
                                    tr("RESOLVED"), tr("OPEN"), tr("NET P/L"), tr("EVIDENCE")}, page);
    experiments_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    experiments_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    experiments_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    root->addWidget(experiments_table_, 1);
    connect(experiments_table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const QString id = experiments_table_->item(row, 0)->data(Qt::UserRole).toString();
        create_research_notebook(QStringLiteral("strategy_experiment"), id);
    });
    return page;
}

QWidget* CodeEditorScreen::build_trade_reviews_page() {
    auto* page = new QWidget(this);
    page->setObjectName("nbLabPage");
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    QHBoxLayout* actions = nullptr;
    root->addWidget(lab_band(page, tr("TRADE REVIEWS"), reviews_summary_, actions));
    auto* postmortem = lab_button(tr("CREATE POSTMORTEM"), page, "nbPrimaryBtn");
    connect(postmortem, &QPushButton::clicked, this, [this]() {
        QString reference;
        if (trade_reviews_table_ && trade_reviews_table_->currentRow() >= 0)
            reference = trade_reviews_table_->item(trade_reviews_table_->currentRow(), 0)->data(Qt::UserRole).toString();
        create_research_notebook(QStringLiteral("trade_postmortem"), reference);
    });
    actions->addWidget(postmortem);
    auto* replay = lab_button(tr("MARKET REPLAY"), page, "nbAccentBtn");
    connect(replay, &QPushButton::clicked, this,
            [this]() { create_research_notebook(QStringLiteral("market_replay")); });
    actions->addWidget(replay);
    auto* refresh = lab_button(tr("REFRESH"), page);
    connect(refresh, &QPushButton::clicked, this, &CodeEditorScreen::refresh_trade_reviews);
    actions->addWidget(refresh);

    trade_reviews_table_ = lab_table({tr("TIME"), tr("MODE"), tr("BOOK / HANDLER"), tr("SYMBOL"), tr("SIDE"),
                                      tr("STATE"), tr("NOTIONAL"), tr("P/L"), tr("QUALITY"), tr("RATIONALE")}, page);
    trade_reviews_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    trade_reviews_table_->horizontalHeader()->setSectionResizeMode(9, QHeaderView::Stretch);
    root->addWidget(trade_reviews_table_, 1);
    connect(trade_reviews_table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const QString id = trade_reviews_table_->item(row, 0)->data(Qt::UserRole).toString();
        create_research_notebook(QStringLiteral("trade_postmortem"), id);
    });
    return page;
}

QWidget* CodeEditorScreen::build_reports_page() {
    auto* page = new QWidget(this);
    page->setObjectName("nbLabPage");
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    QHBoxLayout* actions = nullptr;
    root->addWidget(lab_band(page, tr("REPORTS & SCHEDULES"), reports_summary_, actions));
    auto* calibration = lab_button(tr("CALIBRATION REPORT"), page, "nbPrimaryBtn");
    connect(calibration, &QPushButton::clicked, this,
            [this]() { create_research_notebook(QStringLiteral("calibration_report")); });
    actions->addWidget(calibration);
    auto* cohort = lab_button(tr("COHORT ANALYSIS"), page, "nbAccentBtn");
    connect(cohort, &QPushButton::clicked, this,
            [this]() { create_research_notebook(QStringLiteral("cohort_analysis")); });
    actions->addWidget(cohort);
    auto* schedule = lab_button(tr("SCHEDULE WITH DAEMON"), page);
    connect(schedule, &QPushButton::clicked, this, &CodeEditorScreen::open_automation_workspace);
    actions->addWidget(schedule);
    auto* refresh = lab_button(tr("REFRESH"), page);
    connect(refresh, &QPushButton::clicked, this, &CodeEditorScreen::refresh_reports);
    actions->addWidget(refresh);

    reports_table_ = lab_table({tr("REPORT"), tr("TYPE"), tr("DATA CUTOFF"), tr("MODIFIED"), tr("SIZE"),
                                tr("LOCAL PATH")}, page);
    reports_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    reports_table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    root->addWidget(reports_table_, 1);
    connect(reports_table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const QString path = reports_table_->item(row, 0)->data(Qt::UserRole).toString();
        if (!path.isEmpty())
            open_notebook_path(path);
    });
    return page;
}

void CodeEditorScreen::refresh_experiments() {
    if (!experiments_table_)
        return;
    experiments_table_->setRowCount(0);
    const auto strategies = sandbox::list_strategies();
    const auto leaderboard = sandbox::leaderboard(ProfileManager::instance().active());
    if (strategies.is_err() || leaderboard.is_err()) {
        experiments_summary_->setText(tr("Local evidence database unavailable"));
        return;
    }

    QMap<QString, sandbox::LeaderboardRow> board;
    for (const auto& row : leaderboard.value())
        board.insert(row.strategy_id, row);

    int resolved_total = 0;
    int open_total = 0;
    int eligible_total = 0;
    double pnl_total = 0.0;
    for (const auto& strategy : strategies.value()) {
        const QJsonObject params = QJsonDocument::fromJson(strategy.params_json.toUtf8()).object();
        const auto score_it = board.constFind(strategy.strategy_id);
        const sandbox::LeaderboardRow score = score_it == board.cend() ? sandbox::LeaderboardRow{} : score_it.value();
        auto open_result = Database::instance().execute(
            QStringLiteral("SELECT COUNT(*) FROM sandbox_position WHERE strategy_id=? AND state IN ('open','pending_fill')"),
            {strategy.strategy_id});
        int open_count = 0;
        if (open_result.is_ok() && open_result.value().next())
            open_count = open_result.value().value(0).toInt();
        auto evidence_result = Database::instance().execute(
            QStringLiteral("SELECT MIN(created_at), COUNT(*) FROM sandbox_position WHERE strategy_id=?"),
            {strategy.strategy_id});
        qint64 first_created = 0;
        int total_positions = 0;
        if (evidence_result.is_ok() && evidence_result.value().next()) {
            first_created = evidence_result.value().value(0).toLongLong();
            total_positions = evidence_result.value().value(1).toInt();
        }
        sandbox::EligibilityInput input;
        input.active_days = first_created > 0
            ? qMax(0, static_cast<int>((QDateTime::currentMSecsSinceEpoch() - first_created) / 86400000LL)) : 0;
        input.resolved = score.resolved;
        input.degraded = score.degraded;
        input.total_positions = total_positions;
        input.net_pnl = score.net_pnl;
        input.max_drawdown = score.max_drawdown;
        input.gross_notional = score.gross_notional;
        input.hypothetical = params.value(QStringLiteral("hypothetical")).toBool(false);
        const auto verdict = sandbox::evaluate_eligibility(input);

        const int row = experiments_table_->rowCount();
        experiments_table_->insertRow(row);
        auto* id_item = table_item(strategy.strategy_id.left(16), strategy.strategy_id);
        id_item->setData(Qt::UserRole, strategy.strategy_id);
        experiments_table_->setItem(row, 0, id_item);
        experiments_table_->setItem(row, 1, table_item(strategy.kind));
        experiments_table_->setItem(row, 2, table_item(strategy.symbols));
        const QString source = params.value(QStringLiteral("journal_source"))
                                   .toString(params.value(QStringLiteral("source")).toString(QStringLiteral("local")));
        experiments_table_->setItem(row, 3, table_item(source));
        QString horizon = params.value(QStringLiteral("horizon")).toString();
        if (horizon.isEmpty() && params.contains(QStringLiteral("horizon_sec")))
            horizon = QStringLiteral("%1s").arg(params.value(QStringLiteral("horizon_sec")).toInt());
        experiments_table_->setItem(row, 4, table_item(horizon.isEmpty() ? QStringLiteral("--") : horizon));
        experiments_table_->setItem(row, 5, table_item(QString::number(score.resolved)));
        experiments_table_->setItem(row, 6, table_item(QString::number(open_count)));
        auto* pnl_item = table_item(money(score.net_pnl));
        pnl_item->setForeground(QColor(score.net_pnl >= 0.0 ? colors::POSITIVE() : colors::NEGATIVE()));
        experiments_table_->setItem(row, 7, pnl_item);
        QString evidence;
        if (input.hypothetical)
            evidence = tr("HYPOTHETICAL");
        else if (verdict.eligible)
            evidence = tr("ELIGIBLE FOR HUMAN REVIEW");
        else
            evidence = tr("COLLECTING %1/%2").arg(score.resolved).arg(sandbox::kMinResolvedSample);
        experiments_table_->setItem(row, 8, table_item(evidence, verdict.blockers.join(QStringLiteral("\n"))));

        resolved_total += score.resolved;
        open_total += open_count;
        pnl_total += score.net_pnl;
        if (verdict.eligible)
            ++eligible_total;
    }
    experiments_summary_->setText(tr("%1 books · %2 resolved · %3 open · %4 net · %5 eligible for review")
                                      .arg(strategies.value().size()).arg(resolved_total).arg(open_total)
                                      .arg(money(pnl_total)).arg(eligible_total));
}

void CodeEditorScreen::refresh_trade_reviews() {
    if (!trade_reviews_table_)
        return;
    trade_reviews_table_->setRowCount(0);
    auto rows = Database::instance().execute(
        QStringLiteral("SELECT p.decision_id, p.created_at, s.kind, p.symbol, p.side, p.state, p.notional_usd, "
                       "p.realized_pnl, p.data_quality, j.reasons "
                       "FROM sandbox_position p JOIN sandbox_strategy s ON s.strategy_id=p.strategy_id "
                       "LEFT JOIN edge_decision_journal j ON j.id=p.decision_id "
                       "ORDER BY p.created_at DESC LIMIT 250"));
    if (rows.is_err()) {
        reviews_summary_->setText(tr("Local trade evidence unavailable"));
        return;
    }
    int closed = 0;
    int open = 0;
    double pnl = 0.0;
    while (rows.value().next()) {
        const int row = trade_reviews_table_->rowCount();
        trade_reviews_table_->insertRow(row);
        const QString decision_id = rows.value().value(0).toString();
        auto* time_item = table_item(utc_time(rows.value().value(1).toLongLong()), decision_id);
        time_item->setData(Qt::UserRole, decision_id);
        trade_reviews_table_->setItem(row, 0, time_item);
        trade_reviews_table_->setItem(row, 1, table_item(QStringLiteral("PAPER"),
            tr("Sandbox evidence. This row did not submit a live order.")));
        trade_reviews_table_->setItem(row, 2, table_item(rows.value().value(2).toString()));
        trade_reviews_table_->setItem(row, 3, table_item(rows.value().value(3).toString()));
        trade_reviews_table_->setItem(row, 4, table_item(rows.value().value(4).toString().toUpper()));
        const QString state = rows.value().value(5).toString();
        trade_reviews_table_->setItem(row, 5, table_item(state.toUpper()));
        trade_reviews_table_->setItem(row, 6, table_item(money(rows.value().value(6).toDouble())));
        const bool has_pnl = !rows.value().isNull(7);
        const double row_pnl = rows.value().value(7).toDouble();
        auto* pnl_item = table_item(has_pnl ? money(row_pnl) : QStringLiteral("--"));
        if (has_pnl)
            pnl_item->setForeground(QColor(row_pnl >= 0.0 ? colors::POSITIVE() : colors::NEGATIVE()));
        trade_reviews_table_->setItem(row, 7, pnl_item);
        trade_reviews_table_->setItem(row, 8, table_item(rows.value().value(8).toString().toUpper()));
        trade_reviews_table_->setItem(row, 9, table_item(rows.value().value(9).toString()));
        if (state == QStringLiteral("closed")) {
            ++closed;
            if (has_pnl)
                pnl += row_pnl;
        } else if (state == QStringLiteral("open") || state == QStringLiteral("pending_fill")) {
            ++open;
        }
    }
    reviews_summary_->setText(tr("%1 recent records · %2 open · %3 closed · %4 realized")
                                 .arg(trade_reviews_table_->rowCount()).arg(open).arg(closed).arg(money(pnl)));
}

void CodeEditorScreen::refresh_reports() {
    if (!reports_table_)
        return;
    reports_table_->setRowCount(0);
    const QString dir_path = AppPaths::data() + QStringLiteral("/notebooks");
    QDir dir(dir_path);
    dir.mkpath(QStringLiteral("."));
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.ipynb")}, QDir::Files, QDir::Time);
    for (const QFileInfo& info : files) {
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
        const int row = reports_table_->rowCount();
        reports_table_->insertRow(row);
        auto* name = table_item(info.completeBaseName());
        name->setData(Qt::UserRole, info.absoluteFilePath());
        reports_table_->setItem(row, 0, name);
        reports_table_->setItem(row, 1, table_item(type));
        reports_table_->setItem(row, 2, table_item(cutoff));
        reports_table_->setItem(row, 3, table_item(info.lastModified().toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm 'UTC'"))));
        reports_table_->setItem(row, 4, table_item(QStringLiteral("%1 KB").arg(qMax<qint64>(1, info.size() / 1024))));
        reports_table_->setItem(row, 5, table_item(info.absoluteFilePath()));
    }
    reports_summary_->setText(tr("%1 local reports · double-click to open · daemon runs use Notebook Run jobs")
                                 .arg(files.size()));
}

void CodeEditorScreen::create_research_notebook(const QString& type, const QString& reference) {
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const QString cutoff = utc_time(now_ms);
    const QString db_path = Database::instance().path().isEmpty()
        ? AppPaths::data() + QStringLiteral("/openmarketterminal.db") : Database::instance().path();

    const QMap<QString, QString> titles{{QStringLiteral("trade_postmortem"), tr("Trade Postmortem")},
                                       {QStringLiteral("strategy_experiment"), tr("Strategy Experiment")},
                                       {QStringLiteral("calibration_report"), tr("Calibration Report")},
                                       {QStringLiteral("market_replay"), tr("Market Replay")},
                                       {QStringLiteral("cohort_analysis"), tr("Cohort Analysis")},
                                       {QStringLiteral("portfolio_research"), tr("Portfolio Research")},
                                       {QStringLiteral("kalshi_calibration"), tr("Kalshi Calibration & Cohorts")},
                                       {QStringLiteral("coinbase_spot"), tr("Coinbase Spot Evidence")},
                                       {QStringLiteral("kraken_maker"), tr("Kraken Maker-Lane Evidence")},
                                       {QStringLiteral("chronos_horizons"), tr("Chronos-2 Horizon Audit")},
                                       {QStringLiteral("ai_decision_audit"), tr("AI Decision & Risk Audit")}};
    const QString title = titles.value(type, tr("Research Notebook"));
    const QString ref_line = reference.isEmpty() ? tr("all current project evidence") : reference;
    const QString markdown = QStringLiteral(
        "# %1\n\n"
        "**Research type:** `%2`  \n"
        "**Data cutoff:** %3  \n"
        "**Reference:** `%4`  \n"
        "**Sources:** local OpenTerminal SQLite evidence store  \n"
        "**Execution policy:** read-only analysis; this notebook cannot submit live orders.\n\n"
        "The creation cutoff records when this notebook was authored. Each query run prints its own UTC runtime; "
        "re-running reads the latest local evidence without changing the stored creation metadata."
    ).arg(title, type, cutoff, ref_line);

    const QString setup = QStringLiteral(
        "from pathlib import Path\n"
        "import sqlite3, json, datetime\n\n"
        "DB_PATH = %1\n"
        "DATA_CUTOFF_UTC = %2\n"
        "REFERENCE = %3\n"
        "QUERY_RUN_UTC = datetime.datetime.now(datetime.timezone.utc).isoformat()\n\n"
        "connection = sqlite3.connect(Path(DB_PATH).as_uri() + '?mode=ro', uri=True)\n"
        "connection.row_factory = sqlite3.Row\n\n"
        "def query(sql, params=()):\n"
        "    return [dict(row) for row in connection.execute(sql, params).fetchall()]\n\n"
        "print(f'Read-only evidence store: {DB_PATH}')\n"
        "print(f'Notebook created: {DATA_CUTOFF_UTC}')\n"
        "print(f'Query runtime: {QUERY_RUN_UTC}')"
    ).arg(python_literal(db_path), python_literal(cutoff), python_literal(reference));

    QString query_code;
    if (type == QStringLiteral("trade_postmortem")) {
        query_code = QStringLiteral(
            "sql = '''SELECT p.decision_id, p.strategy_id, s.kind, p.symbol, p.side, p.state,\n"
            "                p.limit_price, p.qty, p.notional_usd, p.entry_fee, p.exit_fee,\n"
            "                p.realized_pnl, p.close_reason, p.data_quality, p.created_at, p.closed_at,\n"
            "                j.model_probability, j.market_probability, j.edge_after_cost, j.confidence,\n"
            "                j.freshness_json, j.features_json, j.reasons, j.source\n"
            "         FROM sandbox_position p JOIN sandbox_strategy s ON s.strategy_id=p.strategy_id\n"
            "         LEFT JOIN edge_decision_journal j ON j.id=p.decision_id\n"
            "         WHERE (?='' OR p.decision_id=?) ORDER BY p.created_at DESC LIMIT 100'''\n"
            "records = query(sql, (REFERENCE, REFERENCE))\n"
            "print(json.dumps(records, indent=2, default=str))");
    } else if (type == QStringLiteral("strategy_experiment")) {
        query_code = QStringLiteral(
            "sql = '''SELECT s.strategy_id, s.kind, s.symbols, s.params_json, s.status, s.created_at,\n"
            "                COUNT(p.position_id) AS positions,\n"
            "                SUM(CASE WHEN p.state='closed' AND p.realized_pnl IS NOT NULL THEN 1 ELSE 0 END) AS resolved,\n"
            "                SUM(CASE WHEN p.state IN ('open','pending_fill') THEN 1 ELSE 0 END) AS open_positions,\n"
            "                COALESCE(SUM(CASE WHEN p.state='closed' THEN p.realized_pnl ELSE 0 END),0) AS net_pnl\n"
            "         FROM sandbox_strategy s LEFT JOIN sandbox_position p ON p.strategy_id=s.strategy_id\n"
            "         WHERE (?='' OR s.strategy_id=?) GROUP BY s.strategy_id ORDER BY s.created_at DESC'''\n"
            "records = query(sql, (REFERENCE, REFERENCE))\n"
            "print(json.dumps(records, indent=2, default=str))");
    } else if (type == QStringLiteral("market_replay")) {
        query_code = QStringLiteral(
            "records = query('''SELECT created_at, venue, symbol, horizon, market_id, direction, side, call, gate,\n"
            "                          market_probability, model_probability, edge_after_cost, spread_cost, fee_cost,\n"
            "                          confidence, seconds_left, data_status, freshness_json, features_json, reasons, source\n"
            "                   FROM edge_decision_journal\n"
            "                   WHERE (?='' OR id=? OR market_id=(SELECT market_id FROM edge_decision_journal WHERE id=?))\n"
            "                   ORDER BY created_at DESC LIMIT 1000''', (REFERENCE, REFERENCE, REFERENCE))\n"
            "records.reverse()\n"
            "print(json.dumps(records, indent=2, default=str))");
    } else if (type == QStringLiteral("kalshi_calibration")) {
        query_code = QStringLiteral(
            "records = query('''SELECT source, venue, horizon,\n"
            "  CASE WHEN seconds_left BETWEEN 0 AND 60 THEN 'FINAL_60S'\n"
            "       WHEN seconds_left BETWEEN 61 AND 300 THEN '1_5M'\n"
            "       WHEN seconds_left BETWEEN 301 AND 900 THEN '5_15M' ELSE '15M_PLUS' END AS time_bucket,\n"
            "  COUNT(*) AS observations, COUNT(DISTINCT market_id) AS independent_events,\n"
            "  AVG((model_probability-outcome)*(model_probability-outcome)) AS model_brier,\n"
            "  AVG((market_probability-outcome)*(market_probability-outcome)) AS market_brier,\n"
            "  AVG(edge_after_cost) AS avg_edge_after_cost\n"
            "FROM edge_decision_journal WHERE outcome IN (0,1)\n"
            "  AND (venue LIKE '%kalshi%' OR source LIKE '%kalshi%')\n"
            "GROUP BY source,venue,horizon,time_bucket\n"
            "ORDER BY independent_events DESC, observations DESC''')\n"
            "print(json.dumps(records, indent=2, default=str))");
    } else if (type == QStringLiteral("coinbase_spot")) {
        query_code = QStringLiteral(
            "records = query('''SELECT s.strategy_id,s.kind,s.symbols,s.params_json,p.decision_id,p.symbol,p.side,\n"
            "  p.state,p.notional_usd,p.entry_fee,p.exit_fee,p.realized_pnl,p.data_quality,p.created_at,p.closed_at\n"
            "FROM sandbox_strategy s LEFT JOIN sandbox_position p ON p.strategy_id=s.strategy_id\n"
            "WHERE s.params_json LIKE '%coinbase%' OR s.kind LIKE '%spot%' OR s.kind LIKE '%scalp%'\n"
            "ORDER BY p.created_at DESC LIMIT 1000''')\n"
            "print(json.dumps(records, indent=2, default=str))");
    } else if (type == QStringLiteral("kraken_maker")) {
        query_code = QStringLiteral(
            "records = query('''SELECT s.strategy_id,s.kind,s.symbols,s.params_json,p.decision_id,p.symbol,p.side,\n"
            "  p.state,p.limit_price,p.qty,p.notional_usd,p.entry_fee,p.exit_fee,p.realized_pnl,p.close_reason,\n"
            "  p.data_quality,p.created_at,p.closed_at\n"
            "FROM sandbox_strategy s LEFT JOIN sandbox_position p ON p.strategy_id=s.strategy_id\n"
            "WHERE s.params_json LIKE '%kraken%' OR (s.kind LIKE '%maker%' AND s.symbols LIKE '%BTC%')\n"
            "ORDER BY p.created_at DESC LIMIT 1000''')\n"
            "print(json.dumps(records, indent=2, default=str))");
    } else if (type == QStringLiteral("chronos_horizons")) {
        query_code = QStringLiteral(
            "records = query('''SELECT source,symbol,horizon,COUNT(*) AS forecasts,\n"
            "  SUM(CASE WHEN outcome IN (0,1) THEN 1 ELSE 0 END) AS resolved,\n"
            "  AVG(CASE WHEN outcome IN (0,1) THEN (model_probability-outcome)*(model_probability-outcome) END) AS brier,\n"
            "  AVG(confidence) AS avg_confidence,AVG(edge_after_cost) AS avg_edge_after_cost,MAX(created_at) AS latest\n"
            "FROM edge_decision_journal WHERE source LIKE '%chronos%'\n"
            "GROUP BY source,symbol,horizon ORDER BY symbol,horizon''')\n"
            "print(json.dumps(records, indent=2, default=str))");
    } else if (type == QStringLiteral("ai_decision_audit")) {
        query_code = QStringLiteral(
            "fills = query('''SELECT handler,symbol,side,COUNT(*) AS fills,SUM(quantity) AS quantity,\n"
            "  SUM(fee) AS fees,SUM(realized_pnl) AS realized_pnl,MAX(ts) AS latest_fill\n"
            "FROM ai_fill GROUP BY handler,symbol,side ORDER BY latest_fill DESC''')\n"
            "decisions = query('''SELECT created_at,source,venue,symbol,horizon,side,call,gate,model_probability,\n"
            "  market_probability,edge_after_cost,confidence,data_status,reasons\n"
            "FROM edge_decision_journal WHERE source LIKE '%ai%' OR source LIKE '%llm%'\n"
            "ORDER BY created_at DESC LIMIT 500''')\n"
            "print(json.dumps({'fills': fills, 'decisions': decisions}, indent=2, default=str))");
    } else if (type == QStringLiteral("calibration_report") || type == QStringLiteral("cohort_analysis")) {
        query_code = QStringLiteral(
            "records = query('''SELECT source, venue, symbol, horizon, COUNT(*) AS observations,\n"
            "                          COUNT(DISTINCT market_id) AS independent_markets,\n"
            "                          SUM(CASE WHEN outcome IN (0,1) THEN 1 ELSE 0 END) AS resolved,\n"
            "                          AVG(CASE WHEN outcome IN (0,1) THEN\n"
            "                              (model_probability-outcome)*(model_probability-outcome) END) AS model_brier,\n"
            "                          AVG(CASE WHEN outcome IN (0,1) THEN\n"
            "                              (market_probability-outcome)*(market_probability-outcome) END) AS market_brier,\n"
            "                          AVG(edge_after_cost) AS avg_edge_after_cost, AVG(confidence) AS avg_confidence\n"
            "                   FROM edge_decision_journal\n"
            "                   GROUP BY source, venue, symbol, horizon\n"
            "                   ORDER BY resolved DESC, observations DESC''')\n"
            "print(json.dumps(records, indent=2, default=str))");
    } else {
        query_code = QStringLiteral(
            "records = query('''SELECT handler, symbol, side, COUNT(*) AS fills, SUM(quantity) AS quantity,\n"
            "                          SUM(fee) AS fees, SUM(realized_pnl) AS realized_pnl, MAX(ts) AS latest_fill\n"
            "                   FROM ai_fill GROUP BY handler, symbol, side ORDER BY latest_fill DESC''')\n"
            "print(json.dumps(records, indent=2, default=str))");
    }

    const QString analysis = QStringLiteral(
        "## Review checklist\n\n"
        "- Was every feature available at the decision timestamp?\n"
        "- Were source freshness, spread, fees, and slippage acceptable?\n"
        "- Did the result remain positive after costs?\n"
        "- Is this an independent event, or correlated with an existing sample?\n"
        "- What specific hypothesis should the next immutable strategy version test?\n\n"
        "> A notebook may recommend a **paper** candidate. Live execution remains outside the Research Lab."
    );

    QJsonObject research_metadata{{QStringLiteral("research_type"), type},
                                  {QStringLiteral("data_cutoff_utc"), cutoff},
                                  {QStringLiteral("reference"), reference},
                                  {QStringLiteral("database"), db_path},
                                  {QStringLiteral("source_freshness"), QStringLiteral("captured at notebook creation")},
                                  {QStringLiteral("model_version"), QStringLiteral("recorded in queried decision features")},
                                  {QStringLiteral("cost_policy"), QStringLiteral("use stored cost-net evidence")},
                                  {QStringLiteral("live_order_capability"), false}};
    QJsonArray cells{markdown_cell(markdown), code_cell(setup), code_cell(query_code), markdown_cell(analysis)};
    const QJsonObject notebook{{QStringLiteral("cells"), cells},
                               {QStringLiteral("metadata"), QJsonObject{
                                    {QStringLiteral("kernelspec"), QJsonObject{{QStringLiteral("display_name"), QStringLiteral("Python 3")},
                                                                               {QStringLiteral("language"), QStringLiteral("python")},
                                                                               {QStringLiteral("name"), QStringLiteral("python3")}}},
                                    {QStringLiteral("language_info"), QJsonObject{{QStringLiteral("name"), QStringLiteral("python")}}},
                                    {QStringLiteral("openmarketterminal"), research_metadata}}},
                               {QStringLiteral("nbformat"), 4},
                               {QStringLiteral("nbformat_minor"), 5}};

    const QString dir = AppPaths::data() + QStringLiteral("/notebooks");
    QDir().mkpath(dir);
    const QString path = QStringLiteral("%1/%2_%3.ipynb")
                             .arg(dir, notebook_slug(type), QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(notebook).toJson(QJsonDocument::Indented)) < 0 ||
        !file.commit()) {
        LOG_WARN("ResearchLab", "Could not create research notebook: " + path);
        return;
    }
    open_notebook_path(path);
}

void CodeEditorScreen::open_strategy_workspace() {
    EventBus::instance().publish(QStringLiteral("nav.switch_screen"),
                                 QVariantMap{{QStringLiteral("screen_id"), QStringLiteral("algo_trading")},
                                             {QStringLiteral("source"), QStringLiteral("research_lab")}});
}

void CodeEditorScreen::open_automation_workspace() {
    EventBus::instance().publish(QStringLiteral("nav.switch_screen"),
                                 QVariantMap{{QStringLiteral("screen_id"), QStringLiteral("profile")},
                                             {QStringLiteral("source"), QStringLiteral("research_lab")},
                                             {QStringLiteral("tab"), QStringLiteral("automation")}});
}

} // namespace openmarketterminal::screens
