#include "screens/algo_trading/StrategyAutomationPanel.h"

#include "core/config/ProfileManager.h"
#include "core/events/EventBus.h"
#include "storage/repositories/SettingsRepository.h"
#include "ui/theme/Theme.h"

#include <QAbstractItemView>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QProcess>
#include <QStandardPaths>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

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

QString relative_time(const QString& iso) {
    QDateTime ts = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!ts.isValid()) ts = QDateTime::fromString(iso, Qt::ISODate);
    if (!ts.isValid()) return QStringLiteral("-");
    const qint64 seconds = ts.toUTC().secsTo(QDateTime::currentDateTimeUtc());
    if (seconds < 0) {
        const qint64 future = -seconds;
        return future < 120 ? QObject::tr("in %1s").arg(future)
                            : future < 7200 ? QObject::tr("in %1m").arg(future / 60)
                                            : QObject::tr("in %1h").arg(future / 3600);
    }
    return seconds < 120 ? QObject::tr("%1s ago").arg(seconds)
                         : seconds < 7200 ? QObject::tr("%1m ago").arg(seconds / 60)
                                         : QObject::tr("%1h ago").arg(seconds / 3600);
}

QString cadence(int seconds) {
    if (seconds <= 0) return QStringLiteral("-");
    if (seconds < 60) return QObject::tr("every %1s").arg(seconds);
    if (seconds < 3600) return QObject::tr("every %1m").arg(seconds / 60);
    if (seconds < 86400) return QObject::tr("every %1h").arg(seconds / 3600);
    return QObject::tr("every %1d").arg(seconds / 86400);
}

bool strategy_job(const QJsonObject& job) {
    if (job.value(QStringLiteral("managed_by")).toString() == QStringLiteral("strategy-sandbox"))
        return true;
    const QString text = (job.value(QStringLiteral("name")).toString() + QLatin1Char(' ') +
                          job.value(QStringLiteral("description")).toString()).toLower();
    return text.contains(QStringLiteral("chronos")) || text.contains(QStringLiteral("strategy sandbox")) ||
           text.contains(QStringLiteral("bitcoin evidence"));
}

} // namespace

StrategyAutomationPanel::StrategyAutomationPanel(QWidget* parent) : QWidget(parent) {
    build_ui();
    refresh_timer_.setInterval(5000);
    connect(&refresh_timer_, &QTimer::timeout, this, &StrategyAutomationPanel::refresh);
}

void StrategyAutomationPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (first_show_) { first_show_ = false; refresh(); }
    refresh_timer_.start();
}

void StrategyAutomationPanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    refresh_timer_.stop();
}

void StrategyAutomationPanel::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 10);
    root->setSpacing(8);

    auto* command_row = new QHBoxLayout;
    auto* heading = new QLabel(tr("PAPER RESEARCH AUTOMATION"), this);
    heading->setStyleSheet(QStringLiteral("color:%1;font-size:13px;font-weight:800;%2")
                               .arg(ui::colors::AMBER(), QString::fromLatin1(kMono)));
    command_row->addWidget(heading);
    status_label_ = new QLabel(tr("Actual daemon workflows that collect, predict, resolve, and score proof books."), this);
    status_label_->setStyleSheet(QStringLiteral("color:%1;font-size:10px;%2")
                                     .arg(ui::colors::TEXT_SECONDARY(), QString::fromLatin1(kMono)));
    command_row->addWidget(status_label_, 1);
    auto add_button = [&](const QString& text, const QString& color, auto callback) {
        auto* button = new QPushButton(text, this);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(button_style(color));
        connect(button, &QPushButton::clicked, this, callback);
        command_row->addWidget(button);
        return button;
    };
    run_button_ = add_button(tr("RUN NOW"), ui::colors::CYAN(), [this]() { run_selected(); });
    toggle_button_ = add_button(tr("DISABLE"), ui::colors::AMBER(), [this]() {
        const int row = jobs_table_->currentRow();
        if (row >= 0) set_selected_enabled(!jobs_table_->item(row, 0)->data(Qt::UserRole + 1).toBool());
    });
    clear_button_ = add_button(tr("CLEAR FAILURES"), ui::colors::NEGATIVE(), [this]() { clear_selected_failures(); });
    add_button(tr("REFRESH"), ui::colors::TEXT_PRIMARY(), [this]() { refresh(); });
    root->addLayout(command_row);

    auto* program_row = new QHBoxLayout;
    auto* program_label = new QLabel(tr("PAPER PROGRAM"), this);
    program_label->setStyleSheet(QStringLiteral("color:%1;font-size:9px;font-weight:800;%2")
                                     .arg(ui::colors::TEXT_TERTIARY(), QString::fromLatin1(kMono)));
    program_row->addWidget(program_label);
    auto add_program = [&](const QString& label, const QStringList& args, const QString& activity) {
        auto* button = new QPushButton(label, this);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(button_style(ui::colors::AMBER()));
        connect(button, &QPushButton::clicked, this, [this, args, activity]() { run_cli(args, activity); });
        program_row->addWidget(button);
    };
    add_program(tr("BTC $50"),
                {QStringLiteral("automation"), QStringLiteral("start"), QStringLiteral("btc"),
                 QStringLiteral("--amount"), QStringLiteral("50")},
                tr("Starting the BTC paper program..."));
    add_program(tr("BTC / ETH / SOL $50"),
                {QStringLiteral("automation"), QStringLiteral("start"), QStringLiteral("major"),
                 QStringLiteral("--amount"), QStringLiteral("50")},
                tr("Starting the major-crypto paper program..."));
    add_program(tr("CONTINUOUS BTC"),
                {QStringLiteral("automation"), QStringLiteral("forever"), QStringLiteral("btc"),
                 QStringLiteral("--amount"), QStringLiteral("50"),
                 QStringLiteral("--min-confidence"), QStringLiteral("80")},
                tr("Configuring continuous BTC paper research..."));
    add_program(tr("STOP PROGRAM"),
                {QStringLiteral("automation"), QStringLiteral("stop")},
                tr("Stopping the paper program..."));
    program_row->addStretch(1);
    root->addLayout(program_row);

    auto* live_row = new QHBoxLayout;
    auto* live_label = new QLabel(tr("KALSHI BOUNDED AUTO"), this);
    live_label->setStyleSheet(QStringLiteral("color:%1;font-size:9px;font-weight:800;%2")
                                  .arg(ui::colors::CYAN(), QString::fromLatin1(kMono)));
    live_row->addWidget(live_label);
    for (const QString& duration : {QStringLiteral("1H"), QStringLiteral("6H"),
                                    QStringLiteral("12H"), QStringLiteral("24/7")}) {
        auto* button = new QPushButton(duration, this);
        button->setCursor(Qt::PointingHandCursor);
        button->setText(duration == QStringLiteral("1H")
                            ? tr("ARM CURRENT HOUR") : tr("ARM %1").arg(duration));
        button->setToolTip(duration == QStringLiteral("1H")
                               ? tr("Start now and stop at the next :00 clock boundary.")
                               : tr("Allow up to 10 autonomous $2-capped Kalshi orders per rolling hour."));
        button->setStyleSheet(button_style(ui::colors::CYAN()));
        connect(button, &QPushButton::clicked, this, [this, duration]() {
            const auto answer = QMessageBox::warning(
                this, tr("Arm real Kalshi automation"),
                tr("For %1, the bot may submit REAL Kalshi orders without approving each bet.%2\n\n"
                   "Limits: at most 10 orders in any rolling hour, $2 contract stake, fees may bring "
                   "all-in cost to $3, $120 experiment exposure, one bot order per contract. The kill "
                   "switch, quote freshness, depth, time-left, edge, credentials, and session expiry are "
                   "checked again before every submission.")
                    .arg(duration,
                         duration == QStringLiteral("1H")
                             ? tr(" This CURRENT HOUR session stops at the next :00 boundary.")
                             : QString()),
                QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
            if (answer != QMessageBox::Yes) return;
            run_cli({QStringLiteral("kalshi"), QStringLiteral("auto"), QStringLiteral("live"),
                     QStringLiteral("session"), duration.toLower()},
                    tr("Arming the %1 bounded autonomous Kalshi session...").arg(duration));
        });
        live_row->addWidget(button);
    }
    auto* prepare = new QPushButton(tr("MANUAL NEXT $2 BET"), this);
    prepare->setCursor(Qt::PointingHandCursor);
    prepare->setToolTip(tr("Prepare one fresh real-market draft. This does not submit money."));
    prepare->setStyleSheet(button_style(ui::colors::AMBER()));
    connect(prepare, &QPushButton::clicked, this, [this]() {
        run_cli({QStringLiteral("kalshi"), QStringLiteral("auto"), QStringLiteral("live"),
                 QStringLiteral("prepare-next"), QStringLiteral("--max-stake"), QStringLiteral("2"),
                 QStringLiteral("--experiment-cap"), QStringLiteral("120")},
                tr("Preparing the next eligible Kalshi live draft..."));
    });
    live_row->addWidget(prepare);
    auto* kill = new QPushButton(tr("KILL AUTOMATED TRADING"), this);
    kill->setCursor(Qt::PointingHandCursor);
    kill->setToolTip(tr("Immediately engage the global trading kill switch and stop this session."));
    kill->setStyleSheet(button_style(ui::colors::NEGATIVE()));
    connect(kill, &QPushButton::clicked, this, [this]() {
        if (QMessageBox::question(this, tr("Kill automated trading"),
                                  tr("Engage the global kill switch and stop the Kalshi live evidence session?"))
            != QMessageBox::Yes) return;
        SettingsRepository::instance().set(QStringLiteral("cli.kill_switch"), QStringLiteral("true"),
                                           QStringLiteral("cli"));
        SettingsRepository::instance().set(QStringLiteral("cli.kill_switch_latched"),
                                           QStringLiteral("true"), QStringLiteral("cli"));
        EventBus::instance().publish(
            QStringLiteral("settings.changed"),
            QVariantMap{{QStringLiteral("key"), QStringLiteral("cli.kill_switch")}});
        run_cli({QStringLiteral("kalshi"), QStringLiteral("auto"), QStringLiteral("live"),
                 QStringLiteral("session"), QStringLiteral("stop")},
                tr("Global kill switch engaged. Stopping the Kalshi session..."));
    });
    live_row->addWidget(kill);
    live_row->addStretch(1);
    root->addLayout(live_row);

    auto* stats = new QHBoxLayout;
    auto add_stat = [&](const QString& label, QLabel*& value) {
        auto* box = new QWidget(this);
        box->setStyleSheet(QStringLiteral("background:%1;border:1px solid %2;")
                               .arg(ui::colors::BG_SURFACE(), ui::colors::BORDER_DIM()));
        auto* layout = new QVBoxLayout(box);
        layout->setContentsMargins(10, 5, 10, 5);
        value = new QLabel(QStringLiteral("-"), box);
        value->setAlignment(Qt::AlignCenter);
        value->setStyleSheet(QStringLiteral("color:%1;font-size:17px;font-weight:800;%2")
                                 .arg(ui::colors::TEXT_PRIMARY(), QString::fromLatin1(kMono)));
        auto* caption = new QLabel(label, box);
        caption->setAlignment(Qt::AlignCenter);
        caption->setStyleSheet(QStringLiteral("color:%1;font-size:9px;font-weight:700;%2")
                                   .arg(ui::colors::TEXT_TERTIARY(), QString::fromLatin1(kMono)));
        layout->addWidget(value);
        layout->addWidget(caption);
        stats->addWidget(box, 1);
    };
    add_stat(tr("ENABLED"), enabled_count_);
    add_stat(tr("HEALTHY"), healthy_count_);
    add_stat(tr("RUNNING"), running_count_);
    add_stat(tr("NEEDS ATTENTION"), failed_count_);
    root->addLayout(stats);

    jobs_table_ = new QTableWidget(this);
    jobs_table_->setColumnCount(8);
    jobs_table_->setHorizontalHeaderLabels({tr("Workflow"), tr("Role"), tr("Cadence"), tr("State"),
                                             tr("Runs"), tr("Failures"), tr("Last"), tr("Next")});
    jobs_table_->verticalHeader()->setVisible(false);
    jobs_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    jobs_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    jobs_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    jobs_table_->setAlternatingRowColors(true);
    jobs_table_->setStyleSheet(QStringLiteral(
        "QTableWidget{background:%1;color:%2;border:1px solid %3;gridline-color:%3;font-size:11px;%4}"
        "QHeaderView::section{background:%5;color:%6;border:1px solid %3;padding:6px;font-size:9px;font-weight:700;%4}"
        "QTableWidget::item{padding:5px 7px;}QTableWidget::item:selected{background:rgba(217,119,6,0.18);color:%2;}")
        .arg(ui::colors::BG_BASE(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(),
             QString::fromLatin1(kMono), ui::colors::BG_RAISED(), ui::colors::TEXT_SECONDARY()));
    jobs_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    jobs_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int column = 2; column < 8; ++column)
        jobs_table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    connect(jobs_table_, &QTableWidget::itemSelectionChanged, this, &StrategyAutomationPanel::update_selection_detail);
    root->addWidget(jobs_table_, 1);

    detail_label_ = new QLabel(tr("Select a workflow to inspect its real command, latest output, and failure state."), this);
    detail_label_->setWordWrap(true);
    detail_label_->setMinimumHeight(78);
    detail_label_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    detail_label_->setStyleSheet(QStringLiteral("background:%1;color:%2;border:1px solid %3;padding:9px;font-size:10px;%4")
                                     .arg(ui::colors::BG_SURFACE(), ui::colors::TEXT_SECONDARY(),
                                          ui::colors::BORDER_DIM(), QString::fromLatin1(kMono)));
    root->addWidget(detail_label_);
    run_button_->setEnabled(false);
    toggle_button_->setEnabled(false);
    clear_button_->setEnabled(false);
}

QString StrategyAutomationPanel::cli_path() const {
    const QString app_dir = QCoreApplication::applicationDirPath();
    const QStringList candidates{app_dir + QStringLiteral("/openterminalcli"),
        QDir::cleanPath(app_dir + QStringLiteral("/../../../openterminalcli")),
        QStandardPaths::findExecutable(QStringLiteral("openterminalcli")),
        QStandardPaths::findExecutable(QStringLiteral("ot"))};
    for (const auto& path : candidates)
        if (!path.isEmpty() && QFileInfo(path).isExecutable()) return path;
    return {};
}

QString StrategyAutomationPanel::selected_job_id() const {
    const int row = jobs_table_->currentRow();
    return row >= 0 && jobs_table_->item(row, 0) ? jobs_table_->item(row, 0)->data(Qt::UserRole).toString() : QString{};
}

void StrategyAutomationPanel::refresh() {
    const QString cli = cli_path();
    if (cli.isEmpty()) { status_label_->setText(tr("openterminalcli was not found")); return; }
    auto* process = new QProcess(this);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process](int code, QProcess::ExitStatus status) {
                const QByteArray output = process->readAllStandardOutput();
                const QString error = QString::fromUtf8(process->readAllStandardError()).trimmed();
                process->deleteLater();
                const QJsonDocument document = QJsonDocument::fromJson(output);
                if (status != QProcess::NormalExit || code != 0 || !document.isObject()) {
                    status_label_->setText(error.isEmpty() ? tr("Could not load daemon workflows") : error);
                    return;
                }
                populate(document.object());
            });
    process->start(cli, {QStringLiteral("--json"), QStringLiteral("--profile"),
                         ProfileManager::instance().active(), QStringLiteral("daemon"),
                         QStringLiteral("jobs"), QStringLiteral("list")});
}

void StrategyAutomationPanel::populate(const QJsonObject& document) {
    QList<QJsonObject> rows;
    for (const auto& value : document.value(QStringLiteral("jobs")).toArray()) {
        const QJsonObject job = value.toObject();
        if (strategy_job(job)) rows.append(job);
    }
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
        const auto rank = [](const QJsonObject& row) {
            if (row.value(QStringLiteral("running")).toBool()) return 0;
            if (row.value(QStringLiteral("last_status")).toString() == QStringLiteral("failed")) return 1;
            if (row.value(QStringLiteral("enabled")).toBool()) return 2;
            return 3;
        };
        const int a = rank(left), b = rank(right);
        return a != b ? a < b : left.value(QStringLiteral("name")).toString() < right.value(QStringLiteral("name")).toString();
    });
    jobs_table_->setRowCount(rows.size());
    int enabled = 0, healthy = 0, running = 0, failed = 0;
    for (int row = 0; row < rows.size(); ++row) {
        const QJsonObject& job = rows.at(row);
        const bool is_enabled = job.value(QStringLiteral("enabled")).toBool();
        const bool is_running = job.value(QStringLiteral("running")).toBool();
        const QString last_status = job.value(QStringLiteral("last_status")).toString();
        if (is_enabled) ++enabled;
        if (is_running) ++running;
        if (is_enabled && last_status == QStringLiteral("ok")) ++healthy;
        if (is_enabled && last_status == QStringLiteral("failed")) ++failed;
        const QString state = !is_enabled ? tr("PAUSED") : is_running ? tr("RUNNING")
            : last_status == QStringLiteral("ok") ? tr("HEALTHY") : tr("ATTENTION");
        const QString state_color = state == tr("HEALTHY") ? ui::colors::POSITIVE()
            : state == tr("RUNNING") ? ui::colors::CYAN()
            : state == tr("ATTENTION") ? ui::colors::NEGATIVE() : ui::colors::TEXT_TERTIARY();
        const QString role = job.value(QStringLiteral("description")).toString();
        const QStringList values{job.value(QStringLiteral("name")).toString(), role,
            cadence(job.value(QStringLiteral("interval_sec")).toInt()), state,
            QString::number(job.value(QStringLiteral("run_count")).toInt()),
            QString::number(job.value(QStringLiteral("fail_count")).toInt()),
            relative_time(job.value(QStringLiteral("last_run_at")).toString()),
            relative_time(job.value(QStringLiteral("next_run_at")).toString())};
        for (int column = 0; column < values.size(); ++column) {
            auto* item = new QTableWidgetItem(values.at(column));
            if (column == 0) {
                item->setData(Qt::UserRole, job.value(QStringLiteral("id")).toString());
                item->setData(Qt::UserRole + 1, is_enabled);
                item->setData(Qt::UserRole + 2, job);
            }
            if (column == 3) item->setForeground(QColor(state_color));
            if (column == 5 && job.value(QStringLiteral("fail_count")).toInt() > 0)
                item->setForeground(QColor(ui::colors::AMBER()));
            jobs_table_->setItem(row, column, item);
        }
    }
    enabled_count_->setText(QString::number(enabled));
    healthy_count_->setText(QString::number(healthy));
    running_count_->setText(QString::number(running));
    failed_count_->setText(QString::number(failed));
    failed_count_->setStyleSheet(QStringLiteral("color:%1;font-size:17px;font-weight:800;%2")
                                     .arg(failed > 0 ? ui::colors::NEGATIVE() : ui::colors::TEXT_PRIMARY(),
                                          QString::fromLatin1(kMono)));
    status_label_->setText(failed > 0
        ? tr("%1 enabled workflows need attention. Historical failure counts remain visible until explicitly cleared.").arg(failed)
        : tr("These are the actual local daemon workflows feeding and scoring proof books."));
    update_selection_detail();
}

void StrategyAutomationPanel::update_selection_detail() {
    const int row = jobs_table_->currentRow();
    const bool selected = row >= 0 && jobs_table_->item(row, 0);
    run_button_->setEnabled(selected);
    toggle_button_->setEnabled(selected);
    clear_button_->setEnabled(selected);
    if (!selected) return;
    const QJsonObject job = jobs_table_->item(row, 0)->data(Qt::UserRole + 2).toJsonObject();
    toggle_button_->setText(job.value(QStringLiteral("enabled")).toBool() ? tr("DISABLE") : tr("ENABLE"));
    QStringList command;
    for (const auto& value : job.value(QStringLiteral("command")).toArray()) command.append(value.toString());
    const QString error = job.value(QStringLiteral("last_error")).toString();
    const QString output = job.value(QStringLiteral("last_output_tail")).toString().trimmed();
    detail_label_->setText(tr("%1\nCommand: openterminalcli %2\nLatest: %3%4")
                               .arg(job.value(QStringLiteral("description")).toString(), command.join(QLatin1Char(' ')),
                                    output.isEmpty() ? tr("no output yet") : output.left(600),
                                    error.isEmpty() ? QString() : tr("\nError: %1").arg(error)));
}

void StrategyAutomationPanel::run_cli(const QStringList& args, const QString& activity) {
    const QString cli = cli_path();
    if (cli.isEmpty()) return;
    status_label_->setText(activity);
    auto* process = new QProcess(this);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process](int code, QProcess::ExitStatus status) {
                const QString output = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
                const QString error = QString::fromUtf8(process->readAllStandardError()).trimmed();
                process->deleteLater();
                status_label_->setText(status == QProcess::NormalExit && code == 0
                    ? tr("Workflow command completed.")
                    : error.isEmpty() ? tr("Workflow command failed.") : error);
                if (!output.isEmpty())
                    detail_label_->setText(output.left(2000));
                refresh();
            });
    QStringList full{QStringLiteral("--profile"), ProfileManager::instance().active()};
    full << args;
    process->start(cli, full);
}

void StrategyAutomationPanel::run_selected() {
    const QString id = selected_job_id();
    if (!id.isEmpty()) run_cli({QStringLiteral("daemon"), QStringLiteral("jobs"), QStringLiteral("run"), id}, tr("Running selected workflow..."));
}

void StrategyAutomationPanel::set_selected_enabled(bool enabled) {
    const QString id = selected_job_id();
    if (!id.isEmpty()) run_cli({QStringLiteral("daemon"), QStringLiteral("jobs"),
                                enabled ? QStringLiteral("enable") : QStringLiteral("disable"), id},
                               enabled ? tr("Enabling workflow...") : tr("Disabling workflow..."));
}

void StrategyAutomationPanel::clear_selected_failures() {
    const QString id = selected_job_id();
    if (!id.isEmpty()) run_cli({QStringLiteral("daemon"), QStringLiteral("jobs"),
                                QStringLiteral("clear-failures"), id, QStringLiteral("--force")},
                               tr("Clearing historical failure count..."));
}

} // namespace openmarketterminal::screens
