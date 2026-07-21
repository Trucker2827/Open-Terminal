#include "screens/algo_trading/StrategyWorkspacePanels.h"

#include "core/config/ProfileManager.h"
#include "core/events/EventBus.h"
#include "mcp/tools/SettingsGate.h"
#include "screens/algo_trading/StrategyAutomationPanel.h"
#include "services/ai_ledger/AiLedger.h"
#include "storage/repositories/AiFillRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/sqlite/Database.h"
#include "ui/theme/Theme.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QSet>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidgetItem>
#include <QTimeZone>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::screens {
namespace {

constexpr auto kMono = "font-family:'Consolas',monospace;";

QString button_style(const QString& color) {
    return QStringLiteral("QPushButton{background:transparent;color:%1;border:1px solid %1;"
                          "font-size:10px;font-weight:700;padding:5px 12px;%2}"
                          "QPushButton:hover{background:rgba(217,119,6,0.12);}"
                          "QPushButton:disabled{color:%3;border-color:%3;}")
        .arg(color, QString::fromLatin1(kMono), ui::colors::TEXT_TERTIARY());
}

QString table_style() {
    return QStringLiteral("QTableWidget{background:%1;color:%2;border:1px solid %3;gridline-color:%3;font-size:10px;%4}"
                          "QHeaderView::section{background:%5;color:%6;border:1px solid "
                          "%3;padding:6px;font-size:9px;font-weight:700;%4}"
                          "QTableWidget::item{padding:5px "
                          "7px;}QTableWidget::item:selected{background:rgba(20,184,166,0.16);color:%2;}")
        .arg(ui::colors::BG_BASE(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(), QString::fromLatin1(kMono),
             ui::colors::BG_RAISED(), ui::colors::TEXT_SECONDARY());
}

QWidget* stat_box(QWidget* parent, const QString& caption, QLabel*& value) {
    auto* box = new QWidget(parent);
    box->setStyleSheet(
        QStringLiteral("background:%1;border:1px solid %2;").arg(ui::colors::BG_SURFACE(), ui::colors::BORDER_DIM()));
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(9, 5, 9, 5);
    layout->setSpacing(1);
    value = new QLabel(QStringLiteral("-"), box);
    value->setAlignment(Qt::AlignCenter);
    value->setWordWrap(true);
    value->setMinimumWidth(0);
    value->setStyleSheet(QStringLiteral("color:%1;font-size:16px;font-weight:800;%2")
                             .arg(ui::colors::TEXT_PRIMARY(), QString::fromLatin1(kMono)));
    auto* label = new QLabel(caption, box);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("color:%1;font-size:8px;font-weight:700;%2")
                             .arg(ui::colors::TEXT_TERTIARY(), QString::fromLatin1(kMono)));
    layout->addWidget(value);
    layout->addWidget(label);
    return box;
}

QString shared_cli_path() {
    const QString app_dir = QCoreApplication::applicationDirPath();
    const QStringList candidates{app_dir + QStringLiteral("/openterminalcli"),
                                 QDir::cleanPath(app_dir + QStringLiteral("/../../../openterminalcli")),
                                 QStandardPaths::findExecutable(QStringLiteral("openterminalcli")),
                                 QStandardPaths::findExecutable(QStringLiteral("ot"))};
    for (const auto& path : candidates)
        if (!path.isEmpty() && QFileInfo(path).isExecutable())
            return path;
    return {};
}

QString relative_time(qint64 ms) {
    if (ms <= 0)
        return QStringLiteral("-");
    const qint64 seconds = QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).secsTo(QDateTime::currentDateTimeUtc());
    if (seconds < 90)
        return QObject::tr("%1s ago").arg(qMax<qint64>(0, seconds));
    if (seconds < 5400)
        return QObject::tr("%1m ago").arg(seconds / 60);
    if (seconds < 129600)
        return QObject::tr("%1h ago").arg(seconds / 3600);
    return QObject::tr("%1d ago").arg(seconds / 86400);
}

QString yes_no(bool value) {
    return value ? QObject::tr("YES") : QObject::tr("NO");
}

QJsonObject parse_object(const QString& json) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json.toUtf8(), &error);
    return error.error == QJsonParseError::NoError && document.isObject() ? document.object() : QJsonObject{};
}

} // namespace

// ── Handlers ─────────────────────────────────────────────────────────────────

StrategyHandlersPanel::StrategyHandlersPanel(QWidget* parent) : QWidget(parent) {
    build_ui();
}

void StrategyHandlersPanel::build_ui() {
    setObjectName(QStringLiteral("strategyHandlersPanel"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 10);
    root->setSpacing(8);

    auto* controls = new QHBoxLayout;
    auto* heading = new QLabel(tr("PAPER HANDLERS"), this);
    heading->setStyleSheet(QStringLiteral("color:%1;font-size:13px;font-weight:800;%2")
                               .arg(ui::colors::AMBER(), QString::fromLatin1(kMono)));
    controls->addWidget(heading);

    handler_combo_ = new QComboBox(this);
    handler_combo_->addItem(tr("Mean Reversion"), QStringLiteral("meanrev"));
    handler_combo_->addItem(tr("Local / Cloud LLM"), QStringLiteral("claude"));
    handler_combo_->setFixedWidth(170);
    symbols_edit_ = new QLineEdit(QStringLiteral("AAPL"), this);
    symbols_edit_->setPlaceholderText(tr("AAPL,MSFT,BTC-USD"));
    symbols_edit_->setMinimumWidth(210);
    interval_spin_ = new QSpinBox(this);
    interval_spin_->setRange(1, 3600);
    interval_spin_->setValue(15);
    interval_spin_->setSuffix(tr(" sec"));
    interval_spin_->setFixedWidth(95);
    aggregate_cap_spin_ = new QDoubleSpinBox(this);
    aggregate_cap_spin_->setRange(0.01, 1000000.0);
    aggregate_cap_spin_->setDecimals(4);
    aggregate_cap_spin_->setValue(1.0);
    aggregate_cap_spin_->setPrefix(tr("CAP "));
    aggregate_cap_spin_->setFixedWidth(125);
    floor_check_ = new QCheckBox(tr("DETERMINISTIC FLOOR"), this);
    floor_check_->setChecked(true);
    floor_check_->setStyleSheet(QStringLiteral("color:%1;font-size:9px;font-weight:700;%2")
                                    .arg(ui::colors::POSITIVE(), QString::fromLatin1(kMono)));
    controls->addWidget(handler_combo_);
    controls->addWidget(symbols_edit_, 1);
    controls->addWidget(interval_spin_);
    controls->addWidget(aggregate_cap_spin_);
    controls->addWidget(floor_check_);
    run_button_ = new QPushButton(tr("RUN ONE PAPER CYCLE"), this);
    run_button_->setCursor(Qt::PointingHandCursor);
    run_button_->setStyleSheet(button_style(ui::colors::CYAN()));
    connect(run_button_, &QPushButton::clicked, this, &StrategyHandlersPanel::run_once);
    controls->addWidget(run_button_);
    auto* refresh_button = new QPushButton(tr("REFRESH"), this);
    refresh_button->setCursor(Qt::PointingHandCursor);
    refresh_button->setStyleSheet(button_style(ui::colors::TEXT_PRIMARY()));
    connect(refresh_button, &QPushButton::clicked, this, &StrategyHandlersPanel::refresh);
    controls->addWidget(refresh_button);
    root->addLayout(controls);

    activity_label_ = new QLabel(tr("Ready. Runs are paper-only and remain behind the global kill switch."), this);
    activity_label_->setStyleSheet(
        QStringLiteral("color:%1;font-size:9px;%2").arg(ui::colors::TEXT_SECONDARY(), QString::fromLatin1(kMono)));
    root->addWidget(activity_label_);

    auto* stats = new QHBoxLayout;
    stats->addWidget(stat_box(this, tr("HANDLERS"), handler_count_), 1);
    stats->addWidget(stat_box(this, tr("LEDGER FILLS"), fill_count_), 1);
    stats->addWidget(stat_box(this, tr("OPEN POSITIONS"), open_count_), 1);
    stats->addWidget(stat_box(this, tr("REALIZED P/L"), realized_total_), 1);
    root->addLayout(stats);

    table_ = new QTableWidget(this);
    table_->setColumnCount(8);
    table_->setHorizontalHeaderLabels({tr("Handler"), tr("Engine"), tr("Mode"), tr("Symbols"), tr("Fills"),
                                       tr("Gross Qty"), tr("Realized P/L"), tr("Last Activity")});
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setStyleSheet(table_style());
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    for (int column = 4; column < 8; ++column)
        table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    root->addWidget(table_, 1);

    refresh();
}

void StrategyHandlersPanel::set_activity(const QString& text, const QString& color) {
    activity_label_->setText(text);
    activity_label_->setStyleSheet(QStringLiteral("color:%1;font-size:9px;%2").arg(color, QString::fromLatin1(kMono)));
}

QString StrategyHandlersPanel::cli_path() const {
    return shared_cli_path();
}

void StrategyHandlersPanel::run_once() {
    const QString symbols = symbols_edit_->text().trimmed();
    if (symbols.isEmpty()) {
        set_activity(tr("Enter at least one symbol."), ui::colors::NEGATIVE());
        return;
    }
    QStringList args{QStringLiteral("ai"),
                     QStringLiteral("run"),
                     QStringLiteral("strategy"),
                     handler_combo_->currentData().toString(),
                     QStringLiteral("--mode"),
                     QStringLiteral("paper"),
                     QStringLiteral("--max-iters"),
                     QStringLiteral("1"),
                     QStringLiteral("--interval-sec"),
                     QString::number(interval_spin_->value()),
                     QStringLiteral("--symbols"),
                     symbols,
                     QStringLiteral("--max-aggregate-qty"),
                     QString::number(aggregate_cap_spin_->value(), 'f', 4)};
    if (!floor_check_->isChecked())
        args << QStringLiteral("--no-floor");
    run_cli(args);
}

void StrategyHandlersPanel::run_cli(const QStringList& args) {
    const QString cli = cli_path();
    if (cli.isEmpty()) {
        set_activity(tr("openterminalcli was not found."), ui::colors::NEGATIVE());
        return;
    }
    run_button_->setEnabled(false);
    set_activity(tr("Running one gated paper cycle..."), ui::colors::CYAN());
    auto* process = new QProcess(this);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process](int code, QProcess::ExitStatus status) {
                const QString output = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
                const QString error = QString::fromUtf8(process->readAllStandardError()).trimmed();
                process->deleteLater();
                run_button_->setEnabled(true);
                if (status == QProcess::NormalExit && code == 0)
                    set_activity(output.isEmpty() ? tr("Paper cycle completed.") : output.left(800),
                                 ui::colors::POSITIVE());
                else
                    set_activity(error.isEmpty() ? tr("Paper cycle failed.") : error.left(800), ui::colors::NEGATIVE());
                refresh();
            });
    QStringList full{QStringLiteral("--profile"), ProfileManager::instance().active()};
    full << args;
    process->start(cli, full);
}

void StrategyHandlersPanel::refresh() {
    struct HandlerStats {
        QString name;
        QSet<QString> symbols;
        int fills = 0;
        double gross_qty = 0.0;
        double realized = 0.0;
        qint64 last_ts = 0;
    };

    QHash<QString, HandlerStats> stats;
    for (const QString& name : {QStringLiteral("meanrev"), QStringLiteral("claude")}) {
        HandlerStats item;
        item.name = name;
        stats.insert(name, item);
    }

    int total_fills = 0;
    auto fills = AiFillRepository::instance().list({}, {}, 0);
    if (fills.is_ok()) {
        for (const auto& fill : fills.value()) {
            auto& item = stats[fill.handler];
            item.name = fill.handler;
            item.symbols.insert(fill.symbol);
            ++item.fills;
            ++total_fills;
            item.realized += fill.realized_pnl;
            item.last_ts = qMax(item.last_ts, fill.ts);
        }
    }

    const auto positions = ai_ledger::positions_of();
    for (const auto& position : positions) {
        auto& item = stats[position.handler];
        item.name = position.handler;
        item.symbols.insert(position.symbol);
        item.gross_qty += std::abs(position.position.net_qty);
    }

    QList<HandlerStats> rows = stats.values();
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
        if (left.last_ts != right.last_ts)
            return left.last_ts > right.last_ts;
        return left.name < right.name;
    });

    table_->setRowCount(rows.size());
    double realized = 0.0;
    for (int row = 0; row < rows.size(); ++row) {
        const auto& item = rows.at(row);
        QStringList symbols = item.symbols.values();
        std::sort(symbols.begin(), symbols.end());
        realized += item.realized;
        const QString engine = item.name == QStringLiteral("claude") ? tr("Configured LLM") : tr("Deterministic");
        const QStringList values{item.name,
                                 engine,
                                 tr("PAPER"),
                                 symbols.isEmpty() ? tr("Not run") : symbols.join(QStringLiteral(", ")),
                                 QString::number(item.fills),
                                 QString::number(item.gross_qty, 'f', 4),
                                 QStringLiteral("$%1").arg(item.realized, 0, 'f', 2),
                                 relative_time(item.last_ts)};
        for (int column = 0; column < values.size(); ++column) {
            auto* cell = new QTableWidgetItem(values.at(column));
            if (column == 2)
                cell->setForeground(QColor(ui::colors::POSITIVE()));
            if (column == 6)
                cell->setForeground(QColor(item.realized >= 0.0 ? ui::colors::POSITIVE() : ui::colors::NEGATIVE()));
            table_->setItem(row, column, cell);
        }
    }

    handler_count_->setText(QString::number(rows.size()));
    fill_count_->setText(QString::number(total_fills));
    open_count_->setText(QString::number(positions.size()));
    realized_total_->setText(QStringLiteral("$%1").arg(realized, 0, 'f', 2));
    realized_total_->setStyleSheet(
        QStringLiteral("color:%1;font-size:16px;font-weight:800;%2")
            .arg(realized >= 0.0 ? ui::colors::POSITIVE() : ui::colors::NEGATIVE(), QString::fromLatin1(kMono)));
}

// ── Risk & Safety ────────────────────────────────────────────────────────────

StrategyRiskPanel::StrategyRiskPanel(QWidget* parent) : QWidget(parent) {
    build_ui();
}

void StrategyRiskPanel::build_ui() {
    setObjectName(QStringLiteral("strategyRiskPanel"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 10);
    root->setSpacing(8);

    auto* command_row = new QHBoxLayout;
    auto* heading = new QLabel(tr("RISK & SAFETY"), this);
    heading->setStyleSheet(QStringLiteral("color:%1;font-size:13px;font-weight:800;%2")
                               .arg(ui::colors::AMBER(), QString::fromLatin1(kMono)));
    command_row->addWidget(heading);
    status_label_ = new QLabel(tr("Human-owned gates and recent deterministic blockers."), this);
    status_label_->setStyleSheet(
        QStringLiteral("color:%1;font-size:9px;%2").arg(ui::colors::TEXT_SECONDARY(), QString::fromLatin1(kMono)));
    command_row->addWidget(status_label_, 1);
    restart_button_ = new QPushButton(tr("RESTART DAEMON"), this);
    restart_button_->setCursor(Qt::PointingHandCursor);
    restart_button_->setStyleSheet(button_style(ui::colors::CYAN()));
    connect(restart_button_, &QPushButton::clicked, this, &StrategyRiskPanel::restart_daemon);
    command_row->addWidget(restart_button_);
    auto* kill = new QPushButton(tr("ENGAGE KILL SWITCH"), this);
    kill->setCursor(Qt::PointingHandCursor);
    kill->setStyleSheet(button_style(ui::colors::NEGATIVE()));
    connect(kill, &QPushButton::clicked, this, &StrategyRiskPanel::engage_kill_switch);
    command_row->addWidget(kill);
    auto* refresh_button = new QPushButton(tr("REFRESH"), this);
    refresh_button->setCursor(Qt::PointingHandCursor);
    refresh_button->setStyleSheet(button_style(ui::colors::TEXT_PRIMARY()));
    connect(refresh_button, &QPushButton::clicked, this, &StrategyRiskPanel::refresh);
    command_row->addWidget(refresh_button);
    root->addLayout(command_row);

    auto* gates = new QHBoxLayout;
    gates->addWidget(stat_box(this, tr("KILL SWITCH"), kill_value_), 1);
    gates->addWidget(stat_box(this, tr("PAPER GATE"), paper_value_), 1);
    gates->addWidget(stat_box(this, tr("LIVE ARM"), live_value_), 1);
    gates->addWidget(stat_box(this, tr("FAST ARM"), fast_value_), 1);
    gates->addWidget(stat_box(this, tr("DAEMON"), daemon_value_), 1);
    root->addLayout(gates);

    auto* permissions = new QHBoxLayout;
    permissions->addWidget(stat_box(this, tr("ALLOWED ACCOUNT"), account_value_), 1);
    permissions->addWidget(stat_box(this, tr("ALLOWED VENUES"), venues_value_), 2);
    root->addLayout(permissions);

    daemon_detail_ = new QLabel(tr("Checking daemon..."), this);
    daemon_detail_->setStyleSheet(
        QStringLiteral("background:%1;color:%2;border:1px solid %3;padding:7px;font-size:9px;%4")
            .arg(ui::colors::BG_SURFACE(), ui::colors::TEXT_SECONDARY(), ui::colors::BORDER_DIM(),
                 QString::fromLatin1(kMono)));
    root->addWidget(daemon_detail_);

    auto* blockers_heading = new QLabel(tr("RECENT DECISION BLOCKERS"), this);
    blockers_heading->setStyleSheet(QStringLiteral("color:%1;font-size:10px;font-weight:800;%2")
                                        .arg(ui::colors::CYAN(), QString::fromLatin1(kMono)));
    root->addWidget(blockers_heading);

    blockers_table_ = new QTableWidget(this);
    blockers_table_->setColumnCount(5);
    blockers_table_->setHorizontalHeaderLabels(
        {tr("Time"), tr("Symbol"), tr("Venue"), tr("Verdict"), tr("Primary Blocker")});
    blockers_table_->verticalHeader()->setVisible(false);
    blockers_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    blockers_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    blockers_table_->setStyleSheet(table_style());
    for (int column = 0; column < 4; ++column)
        blockers_table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    blockers_table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    root->addWidget(blockers_table_, 1);

    refresh();
}

QString StrategyRiskPanel::cli_path() const {
    return shared_cli_path();
}

void StrategyRiskPanel::set_gate(QLabel* value, const QString& text, const QString& color) {
    value->setText(text);
    value->setStyleSheet(QStringLiteral("color:%1;font-size:%2px;font-weight:800;%3")
                             .arg(color)
                             .arg(text.size() > 18   ? 10
                                  : text.size() > 11 ? 12
                                                     : 16)
                             .arg(QString::fromLatin1(kMono)));
}

void StrategyRiskPanel::refresh() {
    const bool kill = mcp::cli_kill_switch_engaged();
    const bool paper = mcp::cli_paper_trading_allowed();
    const bool live = mcp::cli_live_armed();
    const bool fast = mcp::cli_fast_live_armed();
    set_gate(kill_value_, kill ? tr("ENGAGED") : tr("CLEAR"), kill ? ui::colors::NEGATIVE() : ui::colors::POSITIVE());
    set_gate(paper_value_, yes_no(paper), paper ? ui::colors::POSITIVE() : ui::colors::AMBER());
    set_gate(live_value_, live ? tr("ARMED") : tr("OFF"), live ? ui::colors::NEGATIVE() : ui::colors::POSITIVE());
    set_gate(fast_value_, fast ? tr("ARMED") : tr("OFF"), fast ? ui::colors::NEGATIVE() : ui::colors::POSITIVE());
    const QString account = mcp::cli_allowed_account();
    const QStringList venues = mcp::cli_allowed_venues();
    set_gate(account_value_, account.isEmpty() ? tr("NONE") : account,
             account.isEmpty() ? ui::colors::AMBER() : ui::colors::TEXT_PRIMARY());
    set_gate(venues_value_, venues.isEmpty() ? tr("NONE") : venues.join(QStringLiteral(", ")),
             venues.isEmpty() ? ui::colors::AMBER() : ui::colors::TEXT_PRIMARY());

    auto query =
        Database::instance().execute(QStringLiteral("SELECT decision_ts,symbol,venue,verdict,envelope_json FROM "
                                                    "decision_envelopes ORDER BY decision_ts DESC LIMIT 100"));
    struct Blocker {
        qint64 ts;
        QString symbol;
        QString venue;
        QString verdict;
        QString reason;
    };
    QVector<Blocker> rows;
    if (query.is_ok()) {
        while (query.value().next()) {
            const QJsonObject envelope = parse_object(query.value().value(4).toString());
            const QJsonArray blockers = envelope.value(QStringLiteral("risk_blockers")).toArray();
            if (blockers.isEmpty() && query.value().value(3).toString() == QStringLiteral("TRADE_CANDIDATE"))
                continue;
            QString reason =
                blockers.isEmpty() ? envelope.value(QStringLiteral("reason")).toString() : blockers.first().toString();
            if (reason.isEmpty())
                reason = tr("No structured blocker recorded");
            rows.push_back({query.value().value(0).toLongLong(), query.value().value(1).toString(),
                            query.value().value(2).toString(), query.value().value(3).toString(), reason});
            if (rows.size() >= 30)
                break;
        }
    }
    blockers_table_->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        const auto& item = rows.at(row);
        const QStringList values{relative_time(item.ts), item.symbol, item.venue, item.verdict, item.reason};
        for (int column = 0; column < values.size(); ++column) {
            auto* cell = new QTableWidgetItem(values.at(column));
            if (column == 3)
                cell->setForeground(QColor(item.verdict == QStringLiteral("TRADE_CANDIDATE") ? ui::colors::POSITIVE()
                                                                                             : ui::colors::AMBER()));
            blockers_table_->setItem(row, column, cell);
        }
    }
    refresh_daemon();
}

void StrategyRiskPanel::focus_decision_envelopes() {
    refresh();
    status_label_->setText(tr("COCKPIT DRILL-DOWN · latest deterministic decision envelopes and named blockers"));
    status_label_->setStyleSheet(
        QStringLiteral("color:%1;font-size:9px;font-weight:800;%2")
            .arg(ui::colors::CYAN(), QString::fromLatin1(kMono)));
    if (blockers_table_->rowCount() > 0) {
        blockers_table_->selectRow(0);
        blockers_table_->scrollToItem(blockers_table_->item(0, 0));
    }
}

void StrategyRiskPanel::refresh_daemon() {
    const QString cli = cli_path();
    if (cli.isEmpty()) {
        set_gate(daemon_value_, tr("UNKNOWN"), ui::colors::AMBER());
        daemon_detail_->setText(tr("openterminalcli was not found."));
        return;
    }
    auto* process = new QProcess(this);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process](int code, QProcess::ExitStatus status) {
                const QByteArray output = process->readAllStandardOutput();
                const QString error = QString::fromUtf8(process->readAllStandardError()).trimmed();
                process->deleteLater();
                const QJsonDocument document = QJsonDocument::fromJson(output);
                if (status != QProcess::NormalExit || code != 0 || !document.isObject()) {
                    set_gate(daemon_value_, tr("OFFLINE"), ui::colors::NEGATIVE());
                    daemon_detail_->setText(error.isEmpty() ? tr("Daemon status unavailable.") : error);
                    return;
                }
                const QJsonObject object = document.object();
                const bool running = object.value(QStringLiteral("scheduler_running")).toBool() ||
                                     object.value(QStringLiteral("running")).toBool() ||
                                     object.value(QStringLiteral("profile_owner_reachable")).toBool();
                set_gate(daemon_value_, running ? tr("RUNNING") : tr("STOPPED"),
                         running ? ui::colors::POSITIVE() : ui::colors::NEGATIVE());
                daemon_detail_->setText(
                    tr("Mode: %1  |  Owner: %2  |  PID: %3  |  Endpoint: %4")
                        .arg(object.value(QStringLiteral("mode")).toString(QStringLiteral("-")),
                             object.value(QStringLiteral("active_owner_kind")).toString(QStringLiteral("-")),
                             QString::number(
                                 static_cast<qint64>(object.value(QStringLiteral("active_owner_pid")).toDouble())),
                             object.value(QStringLiteral("active_endpoint")).toString(QStringLiteral("-"))));
            });
    process->start(cli, {QStringLiteral("--json"), QStringLiteral("--profile"), ProfileManager::instance().active(),
                         QStringLiteral("daemon"), QStringLiteral("status")});
}

void StrategyRiskPanel::restart_daemon() {
    if (QMessageBox::question(
            this, tr("Restart daemon"),
            tr("Restart the local background daemon? Running jobs will resume from persisted state.")) !=
        QMessageBox::Yes)
        return;
    const QString cli = cli_path();
    if (cli.isEmpty())
        return;
    restart_button_->setEnabled(false);
    status_label_->setText(tr("Restarting daemon..."));
    auto* process = new QProcess(this);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process](int code, QProcess::ExitStatus status) {
                const QString error = QString::fromUtf8(process->readAllStandardError()).trimmed();
                process->deleteLater();
                restart_button_->setEnabled(true);
                status_label_->setText(status == QProcess::NormalExit && code == 0 ? tr("Daemon restart completed.")
                                       : error.isEmpty()                           ? tr("Daemon restart failed.")
                                                                                   : error);
                refresh_daemon();
            });
    process->start(cli, {QStringLiteral("--profile"), ProfileManager::instance().active(), QStringLiteral("daemon"),
                         QStringLiteral("restart")});
}

void StrategyRiskPanel::engage_kill_switch() {
    if (mcp::cli_kill_switch_engaged())
        return;
    if (QMessageBox::warning(this, tr("Engage kill switch"),
                             tr("Stop all AI paper and live order actions until Security explicitly resets the latch?"),
                             QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    SettingsRepository::instance().set(QStringLiteral("cli.kill_switch"), QStringLiteral("true"),
                                       QStringLiteral("cli"));
    SettingsRepository::instance().set(QStringLiteral("cli.kill_switch_latched"), QStringLiteral("true"),
                                       QStringLiteral("cli"));
    EventBus::instance().publish(QStringLiteral("settings.changed"),
                                 QVariantMap{{QStringLiteral("key"), QStringLiteral("cli.kill_switch")}});
    refresh();
}

// ── Run History ──────────────────────────────────────────────────────────────

StrategyRunHistoryPanel::StrategyRunHistoryPanel(QWidget* parent) : QWidget(parent) {
    build_ui();
    refresh_timer_.setInterval(5000);
    connect(&refresh_timer_, &QTimer::timeout, this, &StrategyRunHistoryPanel::refresh);
    refresh_timer_.start();
}

void StrategyRunHistoryPanel::build_ui() {
    setObjectName(QStringLiteral("strategyRunHistoryPanel"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    tabs_->tabBar()->setDrawBase(false);
    tabs_->tabBar()->setExpanding(false);
    tabs_->tabBar()->setUsesScrollButtons(false);
    tabs_->setStyleSheet(
        QStringLiteral("QTabWidget{background:%1;}QTabWidget::pane{border:none;background:%1;top:-1px;}"
                       "QTabWidget::tab-bar{alignment:left;}QTabBar{background:%1;}"
                       "QTabBar::tab{min-width:132px;background:%1;color:%2;border:none;border-bottom:2px solid "
                       "transparent;padding:8px 14px;font-size:9px;font-weight:700;%3}"
                       "QTabBar::tab:selected{color:%4;border-bottom-color:%4;}QTabBar::tab:hover{color:%5;}")
            .arg(ui::colors::BG_BASE(), ui::colors::TEXT_TERTIARY(), QString::fromLatin1(kMono), ui::colors::CYAN(),
                 ui::colors::TEXT_PRIMARY()));

    auto* fills_page = new QWidget(tabs_);
    auto* fills_layout = new QVBoxLayout(fills_page);
    fills_layout->setContentsMargins(12, 8, 12, 10);
    fills_layout->setSpacing(8);
    auto* summary = new QHBoxLayout;
    summary->addWidget(stat_box(fills_page, tr("FILLS"), fill_count_), 1);
    summary->addWidget(stat_box(fills_page, tr("HANDLERS"), handler_count_), 1);
    summary->addWidget(stat_box(fills_page, tr("OPEN POSITIONS"), open_count_), 1);
    summary->addWidget(stat_box(fills_page, tr("REALIZED P/L"), realized_total_), 1);
    auto* refresh_button = new QPushButton(tr("REFRESH"), fills_page);
    refresh_button->setCursor(Qt::PointingHandCursor);
    refresh_button->setStyleSheet(button_style(ui::colors::TEXT_PRIMARY()));
    connect(refresh_button, &QPushButton::clicked, this, &StrategyRunHistoryPanel::refresh);
    summary->addWidget(refresh_button);
    fills_layout->addLayout(summary);

    fills_table_ = new QTableWidget(fills_page);
    fills_table_->setColumnCount(9);
    fills_table_->setHorizontalHeaderLabels({tr("Time"), tr("Handler"), tr("Symbol"), tr("Side"), tr("Qty"),
                                             tr("Fill Price"), tr("Fee"), tr("Realized P/L"), tr("Decision / Draft")});
    fills_table_->verticalHeader()->setVisible(false);
    fills_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    fills_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    fills_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    fills_table_->setAlternatingRowColors(true);
    fills_table_->setStyleSheet(table_style());
    for (int column = 0; column < 8; ++column)
        fills_table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    fills_table_->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Stretch);
    fills_layout->addWidget(fills_table_, 1);
    detail_label_ = new QLabel(tr("Select a fill to inspect its immutable ledger identity."), fills_page);
    detail_label_->setMinimumHeight(48);
    detail_label_->setWordWrap(true);
    detail_label_->setStyleSheet(
        QStringLiteral("background:%1;color:%2;border:1px solid %3;padding:7px;font-size:9px;%4")
            .arg(ui::colors::BG_SURFACE(), ui::colors::TEXT_SECONDARY(), ui::colors::BORDER_DIM(),
                 QString::fromLatin1(kMono)));
    connect(fills_table_, &QTableWidget::itemSelectionChanged, this, [this]() {
        const int row = fills_table_->currentRow();
        if (row < 0 || !fills_table_->item(row, 0))
            return;
        detail_label_->setText(fills_table_->item(row, 0)->data(Qt::UserRole).toString());
    });
    fills_layout->addWidget(detail_label_);
    tabs_->addTab(fills_page, tr("AI FILLS"));

    automation_ = new StrategyAutomationPanel(tabs_);
    tabs_->addTab(automation_, tr("DAEMON JOBS"));

    auto* outcomes_page = new QWidget(tabs_);
    auto* outcomes_layout = new QVBoxLayout(outcomes_page);
    outcomes_layout->setContentsMargins(8, 8, 8, 8);
    outcomes_layout->setSpacing(7);
    outcomes_summary_ = new QLabel(tr("Loading immutable sandbox outcomes..."), outcomes_page);
    outcomes_summary_->setStyleSheet(QStringLiteral("color:%1;font-size:10px;font-weight:700;%2")
                                         .arg(ui::colors::CYAN(), QString::fromLatin1(kMono)));
    outcomes_layout->addWidget(outcomes_summary_);
    outcomes_table_ = new QTableWidget(outcomes_page);
    outcomes_table_->setColumnCount(10);
    outcomes_table_->setHorizontalHeaderLabels(
        {tr("Closed"), tr("Book"), tr("Symbol"), tr("Side"), tr("Qty"), tr("Entry"),
         tr("Exit"), tr("Net P/L"), tr("Reason"), tr("Data Quality")});
    outcomes_table_->verticalHeader()->setVisible(false);
    outcomes_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    outcomes_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    outcomes_table_->setAlternatingRowColors(true);
    outcomes_table_->setStyleSheet(table_style());
    for (int column = 0; column < 8; ++column)
        outcomes_table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    outcomes_table_->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Stretch);
    outcomes_table_->horizontalHeader()->setSectionResizeMode(9, QHeaderView::ResizeToContents);
    outcomes_layout->addWidget(outcomes_table_, 1);
    tabs_->addTab(outcomes_page, tr("SANDBOX OUTCOMES"));
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 0)
            refresh_fills();
        else if (index == 1 && automation_)
            automation_->refresh();
        else if (index == 2)
            refresh_outcomes();
    });
    root->addWidget(tabs_);
    refresh_fills();
}

void StrategyRunHistoryPanel::focus_outcomes() {
    if (!tabs_) return;
    tabs_->setCurrentIndex(2);
    refresh_outcomes();
}

void StrategyRunHistoryPanel::refresh_outcomes() {
    if (!outcomes_table_ || !outcomes_summary_) return;
    auto result = Database::instance().execute(QStringLiteral(
        "SELECT p.closed_at,p.strategy_id,p.symbol,p.side,p.qty,p.limit_price,"
        " (SELECT f.price FROM sandbox_fill f WHERE f.position_id=p.position_id ORDER BY f.ts DESC LIMIT 1),"
        " p.realized_pnl,p.close_reason,p.data_quality"
        " FROM sandbox_position p WHERE p.state='closed' ORDER BY p.closed_at DESC LIMIT 500"));
    if (!result.is_ok()) {
        outcomes_table_->setRowCount(0);
        outcomes_summary_->setText(tr("Sandbox outcome ledger unavailable; no result was fabricated."));
        return;
    }
    struct Outcome { QStringList cells; double pnl = 0.0; bool resolved = false; };
    QVector<Outcome> rows;
    double net = 0.0;
    int gaps = 0;
    while (result.value().next()) {
        const bool resolved = !result.value().value(7).isNull();
        const double pnl = resolved ? result.value().value(7).toDouble() : 0.0;
        net += pnl;
        if (!resolved) ++gaps;
        const qint64 closed = result.value().value(0).toLongLong();
        rows.push_back({{closed > 0 ? QDateTime::fromMSecsSinceEpoch(closed, QTimeZone::UTC).toString(QStringLiteral("MM-dd HH:mm:ss")) : QStringLiteral("-"),
                         result.value().value(1).toString(), result.value().value(2).toString(),
                         result.value().value(3).toString().toUpper(),
                         QString::number(result.value().value(4).toDouble(), 'f', 4),
                         QString::number(result.value().value(5).toDouble(), 'f', 4),
                         resolved ? QString::number(result.value().value(6).toDouble(), 'f', 4) : QStringLiteral("-"),
                         resolved ? QStringLiteral("$%1").arg(pnl, 0, 'f', 2) : tr("DATA GAP"),
                         result.value().value(8).toString(), result.value().value(9).toString()}, pnl, resolved});
    }
    outcomes_table_->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        for (int column = 0; column < rows[row].cells.size(); ++column) {
            auto* item = new QTableWidgetItem(rows[row].cells[column]);
            if (column == 7)
                item->setForeground(QColor(!rows[row].resolved ? ui::colors::AMBER()
                                                : rows[row].pnl >= 0.0 ? ui::colors::POSITIVE()
                                                                      : ui::colors::NEGATIVE()));
            outcomes_table_->setItem(row, column, item);
        }
    }
    outcomes_summary_->setText(
        tr("%1 CLOSED · %2 RESOLVED · %3 DATA GAPS · COST-NET P/L %4$%5")
            .arg(rows.size()).arg(rows.size() - gaps).arg(gaps)
            .arg(net >= 0.0 ? QStringLiteral("+") : QString()).arg(net, 0, 'f', 2));
}

void StrategyRunHistoryPanel::refresh() {
    if (!tabs_ || tabs_->currentIndex() == 0)
        refresh_fills();
    else if (tabs_->currentIndex() == 1 && automation_)
        automation_->refresh();
    else
        refresh_outcomes();
}

void StrategyRunHistoryPanel::refresh_fills() {
    auto result = AiFillRepository::instance().list({}, {}, 250);
    QVector<AiFill> fills = result.is_ok() ? result.value() : QVector<AiFill>{};
    fills_table_->setRowCount(fills.size());
    QSet<QString> handlers;
    double realized = 0.0;
    for (int row = 0; row < fills.size(); ++row) {
        const auto& fill = fills.at(row);
        handlers.insert(fill.handler);
        realized += fill.realized_pnl;
        const QString time = QDateTime::fromMSecsSinceEpoch(fill.ts, QTimeZone::UTC)
                                 .toLocalTime()
                                 .toString(QStringLiteral("MM-dd HH:mm:ss"));
        const QStringList values{time,
                                 fill.handler,
                                 fill.symbol,
                                 fill.side.toUpper(),
                                 QString::number(fill.quantity, 'f', 6),
                                 QStringLiteral("$%1").arg(fill.fill_price, 0, 'f', 4),
                                 QStringLiteral("$%1").arg(fill.fee, 0, 'f', 4),
                                 QStringLiteral("$%1").arg(fill.realized_pnl, 0, 'f', 4),
                                 fill.draft_id};
        for (int column = 0; column < values.size(); ++column) {
            auto* cell = new QTableWidgetItem(values.at(column));
            if (column == 0)
                cell->setData(
                    Qt::UserRole,
                    tr("Fill ID: %1  |  Draft: %2  |  UTC: %3")
                        .arg(fill.id, fill.draft_id,
                             QDateTime::fromMSecsSinceEpoch(fill.ts, QTimeZone::UTC).toString(Qt::ISODateWithMs)));
            if (column == 3)
                cell->setForeground(QColor(fill.side.toLower() == QStringLiteral("buy") ? ui::colors::POSITIVE()
                                                                                        : ui::colors::NEGATIVE()));
            if (column == 7 && std::abs(fill.realized_pnl) > 0.0000001)
                cell->setForeground(QColor(fill.realized_pnl > 0.0 ? ui::colors::POSITIVE() : ui::colors::NEGATIVE()));
            fills_table_->setItem(row, column, cell);
        }
    }
    fill_count_->setText(QString::number(fills.size()));
    handler_count_->setText(QString::number(handlers.size()));
    open_count_->setText(QString::number(ai_ledger::positions_of().size()));
    realized = ai_ledger::realized_total();
    realized_total_->setText(QStringLiteral("$%1").arg(realized, 0, 'f', 2));
    realized_total_->setStyleSheet(
        QStringLiteral("color:%1;font-size:16px;font-weight:800;%2")
            .arg(realized >= 0.0 ? ui::colors::POSITIVE() : ui::colors::NEGATIVE(), QString::fromLatin1(kMono)));
}

} // namespace openmarketterminal::screens
