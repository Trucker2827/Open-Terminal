#include "screens/algo_trading/SandboxBooksPanel.h"

#include "core/config/ProfileManager.h"
#include "core/profile/ProfilePaths.h"
#include "services/sandbox/PaperExecutor.h"
#include "services/sandbox/SandboxEligibility.h"
#include "services/sandbox/SandboxRegistry.h"
#include "services/sandbox/SandboxScorer.h"
#include "storage/sqlite/Database.h"
#include "ui/theme/Theme.h"

#include <QAbstractItemView>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTableWidgetItem>
#include <QVariantList>
#include <QVBoxLayout>

namespace openmarketterminal::screens {

namespace {

static const char* MF = "font-family:'Consolas',monospace;";

QString button_style(const QString& color) {
    return QString("QPushButton{background:transparent;color:%1;border:1px solid %1;"
                   "font-size:10px;font-weight:700;padding:2px 10px;%2}"
                   "QPushButton:hover{background:rgba(217,119,6,0.12);}")
        .arg(color, MF);
}

QString table_style() {
    return QString("QTableWidget{background:%1;color:%2;border:none;gridline-color:%3;font-size:11px;%4}"
                   "QHeaderView::section{background:%5;color:%6;border:1px solid %3;padding:4px;font-size:10px;font-weight:700;%4}"
                   "QTableWidget::item{padding:3px 6px;border-bottom:1px solid %3;}"
                   "QTableWidget::item:selected{background:rgba(217,119,6,0.18);color:%2;}")
        .arg(ui::colors::BG_BASE(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(), MF,
             ui::colors::BG_RAISED(), ui::colors::TEXT_SECONDARY());
}

QJsonObject parse_params(const QString& json) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    return err.error == QJsonParseError::NoError && doc.isObject() ? doc.object() : QJsonObject{};
}

QString fmt_money(double value) {
    return QStringLiteral("$%1").arg(QString::number(value, 'f', 2));
}

QString fmt_pct(double value) {
    return QStringLiteral("%1%").arg(QString::number(value * 100.0, 'f', 1));
}

int scalar_query_int(const QString& sql, const QVariantList& args) {
    auto r = Database::instance().execute(sql, args);
    if (!r.is_ok() || !r.value().next())
        return 0;
    return r.value().value(0).toInt();
}

qint64 scalar_query_i64(const QString& sql, const QVariantList& args) {
    auto r = Database::instance().execute(sql, args);
    if (!r.is_ok() || !r.value().next() || r.value().value(0).isNull())
        return 0;
    return r.value().value(0).toLongLong();
}

} // namespace

SandboxBooksPanel::SandboxBooksPanel(QWidget* parent) : QWidget(parent) {
    build_ui();
}

void SandboxBooksPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (first_show_) {
        first_show_ = false;
        refresh();
    }
}

void SandboxBooksPanel::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* top = new QWidget(this);
    top->setFixedHeight(54);
    top->setStyleSheet(QString("background:%1;border-bottom:1px solid %2;")
                           .arg(ui::colors::BG_RAISED(), ui::colors::BORDER_DIM()));
    auto* top_l = new QHBoxLayout(top);
    top_l->setContentsMargins(12, 6, 12, 6);
    top_l->setSpacing(8);

    status_label_ = new QLabel(tr("Strategy proof books are paper evidence: seed books, collect decisions, score results."), top);
    status_label_->setWordWrap(true);
    status_label_->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;%2")
                                     .arg(ui::colors::TEXT_SECONDARY(), MF));
    top_l->addWidget(status_label_, 1);

    auto add_btn = [&](const QString& text, const QString& color, auto slot) {
        auto* btn = new QPushButton(text, top);
        btn->setFixedHeight(28);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(button_style(color));
        connect(btn, &QPushButton::clicked, this, slot);
        top_l->addWidget(btn);
    };
    add_btn(tr("SEED BOOKS"), ui::colors::AMBER(), [this]() { seed_books(); });
    add_btn(tr("INSTALL JOBS"), ui::colors::AMBER(), [this]() { install_jobs(); });
    add_btn(tr("TICK"), ui::colors::CYAN(), [this]() { run_tick(); });
    add_btn(tr("SCORE"), ui::colors::POSITIVE(), [this]() { run_score(); });
    add_btn(tr("REFRESH"), ui::colors::TEXT_PRIMARY(), [this]() { refresh(); });
    root->addWidget(top);

    auto* stats = new QWidget(this);
    stats->setFixedHeight(56);
    stats->setStyleSheet(QString("background:%1;border-bottom:1px solid %2;")
                             .arg(ui::colors::BG_BASE(), ui::colors::BORDER_DIM()));
    auto* stats_l = new QHBoxLayout(stats);
    stats_l->setContentsMargins(12, 8, 12, 8);
    stats_l->setSpacing(10);
    auto add_stat = [&](const QString& label, QLabel*& out) {
        auto* box = new QWidget(stats);
        box->setStyleSheet(QString("background:%1;border:1px solid %2;")
                               .arg(ui::colors::BG_SURFACE(), ui::colors::BORDER_DIM()));
        auto* bl = new QVBoxLayout(box);
        bl->setContentsMargins(10, 4, 10, 4);
        bl->setSpacing(1);
        out = new QLabel(QStringLiteral("-"), box);
        out->setAlignment(Qt::AlignCenter);
        out->setStyleSheet(QString("color:%1;font-size:16px;font-weight:800;background:transparent;%2")
                               .arg(ui::colors::TEXT_PRIMARY(), MF));
        auto* cap = new QLabel(label, box);
        cap->setAlignment(Qt::AlignCenter);
        cap->setStyleSheet(QString("color:%1;font-size:9px;font-weight:700;background:transparent;%2")
                               .arg(ui::colors::TEXT_TERTIARY(), MF));
        bl->addWidget(out);
        bl->addWidget(cap);
        stats_l->addWidget(box, 1);
    };
    add_stat(tr("ACTIVE BOOKS"), active_count_);
    add_stat(tr("RESOLVED"), resolved_count_);
    add_stat(tr("NET PAPER PNL"), net_pnl_);
    add_stat(tr("LIVE ELIGIBLE"), eligible_count_);
    root->addWidget(stats);

    books_table_ = new QTableWidget(this);
    books_table_->setColumnCount(8);
    books_table_->setHorizontalHeaderLabels(
        {tr("Book"), tr("Symbols"), tr("Venue"), tr("Horizon"), tr("Source"), tr("Mode"), tr("Notional"), tr("Status")});
    books_table_->verticalHeader()->setVisible(false);
    books_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    books_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    books_table_->setAlternatingRowColors(true);
    books_table_->setStyleSheet(table_style());
    books_table_->horizontalHeader()->setStretchLastSection(true);
    books_table_->setMinimumHeight(190);
    root->addWidget(books_table_, 1);

    leaderboard_table_ = new QTableWidget(this);
    leaderboard_table_->setColumnCount(10);
    leaderboard_table_->setHorizontalHeaderLabels(
        {tr("Book"), tr("Resolved"), tr("Open"), tr("Net"), tr("Hit"), tr("Drawdown"),
         tr("Data"), tr("Ranked"), tr("Eligible"), tr("Blockers")});
    leaderboard_table_->verticalHeader()->setVisible(false);
    leaderboard_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    leaderboard_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    leaderboard_table_->setAlternatingRowColors(true);
    leaderboard_table_->setStyleSheet(table_style());
    leaderboard_table_->horizontalHeader()->setStretchLastSection(true);
    leaderboard_table_->setMinimumHeight(220);
    root->addWidget(leaderboard_table_, 1);
}

void SandboxBooksPanel::set_status(const QString& text, const QString& color) {
    if (!status_label_)
        return;
    status_label_->setText(text);
    status_label_->setStyleSheet(QString("color:%1;font-size:11px;background:transparent;%2")
                                     .arg(color.isEmpty() ? ui::colors::TEXT_SECONDARY() : color, MF));
}

void SandboxBooksPanel::seed_books() {
    set_status(tr("Seeding default proof books..."));
    auto seeded = services::sandbox::seed_default_strategies();
    if (seeded.is_err()) {
        set_status(QString::fromStdString(seeded.error()), ui::colors::NEGATIVE());
        return;
    }
    set_status(tr("Seeded %1 proof books.").arg(seeded.value().size()), ui::colors::POSITIVE());
    refresh();
}

void SandboxBooksPanel::install_jobs() {
    set_status(tr("Installing sandbox daemon jobs..."));
    run_cli_command({QStringLiteral("sandbox"), QStringLiteral("install-jobs")},
                    [this](const QJsonObject&, const QString&) {
                        set_status(tr("Sandbox proof jobs installed for this profile."), ui::colors::POSITIVE());
                        refresh();
                    },
                    [this](const QString& msg) { set_status(msg, ui::colors::NEGATIVE()); });
}

void SandboxBooksPanel::run_tick() {
    set_status(tr("Running one sandbox paper tick..."));
    const QString profile = ProfileManager::instance().active();
    const QString daemon_dir = ProfilePaths::profile_root() + QStringLiteral("/daemon");
    auto r = services::sandbox::run_cycle(profile, daemon_dir, QDateTime::currentMSecsSinceEpoch());
    if (r.is_err()) {
        set_status(QString::fromStdString(r.error()), ui::colors::NEGATIVE());
        return;
    }
    const auto rep = r.value();
    set_status(tr("Tick complete: opened %1, filled %2, closed %3, resolved %4, skipped %5.")
                   .arg(rep.opened).arg(rep.filled).arg(rep.closed).arg(rep.resolved).arg(rep.skipped),
               ui::colors::POSITIVE());
    populate_leaderboard();
}

void SandboxBooksPanel::run_score() {
    set_status(tr("Scoring sandbox books..."));
    const QString profile = ProfileManager::instance().active();
    auto r = services::sandbox::score_all(profile, QDateTime::currentMSecsSinceEpoch());
    if (r.is_err()) {
        set_status(QString::fromStdString(r.error()), ui::colors::NEGATIVE());
        return;
    }
    set_status(tr("Sandbox score refreshed."), ui::colors::POSITIVE());
    populate_leaderboard();
}

QString SandboxBooksPanel::cli_path() const {
    const QString app_dir = QCoreApplication::applicationDirPath();
    const QString exe = QStringLiteral("openterminalcli");
    const QStringList candidates = {
        app_dir + QDir::separator() + exe,
        QDir::cleanPath(app_dir + QStringLiteral("/../bin/") + exe),
        QDir::cleanPath(app_dir + QStringLiteral("/../../../") + exe),
        QDir::cleanPath(app_dir + QStringLiteral("/../../../bin/") + exe),
        QStandardPaths::findExecutable(exe),
        QStandardPaths::findExecutable(QStringLiteral("ot"))
    };
    for (const QString& path : candidates) {
        if (!path.isEmpty() && QFileInfo::exists(path) && QFileInfo(path).isExecutable())
            return path;
    }
    return {};
}

void SandboxBooksPanel::run_cli_command(const QStringList& command_args,
                                        std::function<void(const QJsonObject&, const QString&)> on_success,
                                        std::function<void(const QString&)> on_error) {
    const QString cli = cli_path();
    if (cli.isEmpty()) {
        if (on_error)
            on_error(tr("openterminalcli not found next to the app or in PATH"));
        return;
    }
    auto* proc = new QProcess(this);
    QStringList args{QStringLiteral("--json"), QStringLiteral("--profile"), ProfileManager::instance().active()};
    args << command_args;
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [proc, on_success, on_error](int code, QProcess::ExitStatus status) {
                const QString out = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
                const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
                proc->deleteLater();
                QJsonParseError parse;
                const QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8(), &parse);
                if (status == QProcess::NormalExit && code == 0 && parse.error == QJsonParseError::NoError && doc.isObject()) {
                    if (on_success)
                        on_success(doc.object(), out);
                    return;
                }
                if (on_error)
                    on_error(err.isEmpty() ? out : err);
            });
    connect(proc, &QProcess::errorOccurred, this, [proc, on_error](QProcess::ProcessError) {
        proc->deleteLater();
        if (on_error)
            on_error(QObject::tr("could not start openterminalcli"));
    });
    proc->start(cli, args);
}

void SandboxBooksPanel::refresh() {
    populate_books();
    populate_leaderboard();
}

void SandboxBooksPanel::populate_books() {
    auto rows = services::sandbox::list_strategies();
    if (rows.is_err()) {
        set_status(QString::fromStdString(rows.error()), ui::colors::NEGATIVE());
        return;
    }
    QList<services::sandbox::StrategyRow> visible;
    for (const auto& row : rows.value()) {
        if (row.status != QStringLiteral("retired"))
            visible.append(row);
    }
    books_table_->setRowCount(visible.size());
    int active = 0;
    auto add = [&](int row, int col, const QString& text, const QString& color = {}) {
        auto* item = new QTableWidgetItem(text);
        if (!color.isEmpty())
            item->setData(Qt::ForegroundRole, QColor(color));
        books_table_->setItem(row, col, item);
    };
    for (int i = 0; i < visible.size(); ++i) {
        const auto& s = visible.at(i);
        const QJsonObject params = parse_params(s.params_json);
        if (s.status == QStringLiteral("active"))
            ++active;
        QString mode = tr("paper");
        if (params.value(QStringLiteral("prediction")).toBool())
            mode = tr("prediction");
        else if (params.value(QStringLiteral("price_forecast")).toBool())
            mode = tr("Chronos forecast");
        else if (params.value(QStringLiteral("hypothetical")).toBool())
            mode = tr("long/short paper");
        const QString horizon = params.value(QStringLiteral("horizon")).toString(
            params.contains(QStringLiteral("horizon_sec"))
                ? tr("%1s").arg(params.value(QStringLiteral("horizon_sec")).toInt())
                : QStringLiteral("-"));
        const QString source = params.value(QStringLiteral("journal_source")).toString(
            params.value(QStringLiteral("source")).toString(QStringLiteral("-")));
        add(i, 0, s.kind, s.status == QStringLiteral("active") ? ui::colors::POSITIVE() : ui::colors::TEXT_SECONDARY());
        add(i, 1, s.symbols);
        add(i, 2, params.value(QStringLiteral("venue")).toString(QStringLiteral("-")));
        add(i, 3, horizon);
        add(i, 4, source);
        add(i, 5, mode);
        add(i, 6, fmt_money(params.value(QStringLiteral("notional_usd")).toDouble()));
        add(i, 7, s.status, s.status == QStringLiteral("active") ? ui::colors::POSITIVE() : ui::colors::AMBER());
    }
    books_table_->resizeColumnsToContents();
    books_table_->horizontalHeader()->setStretchLastSection(true);
    if (active_count_)
        active_count_->setText(QString::number(active));
    if (visible.isEmpty())
        set_status(tr("No proof books found for this profile. Press SEED BOOKS to create the Chronos/Coinbase sandbox books."),
                   ui::colors::AMBER());
}

void SandboxBooksPanel::populate_leaderboard() {
    const QString profile = ProfileManager::instance().active();
    auto board = services::sandbox::leaderboard(profile);
    if (board.is_err()) {
        set_status(QString::fromStdString(board.error()), ui::colors::NEGATIVE());
        return;
    }
    auto strategies = services::sandbox::list_strategies();
    if (strategies.is_err()) {
        set_status(QString::fromStdString(strategies.error()), ui::colors::NEGATIVE());
        return;
    }
    QHash<QString, services::sandbox::StrategyRow> strat_by_id;
    for (const auto& s : strategies.value())
        strat_by_id.insert(s.strategy_id, s);

    leaderboard_table_->setRowCount(board.value().size());
    int resolved_total = 0;
    int eligible_total = 0;
    double net_total = 0.0;
    auto add = [&](int row, int col, const QString& text, const QString& color = {}) {
        auto* item = new QTableWidgetItem(text);
        if (!color.isEmpty())
            item->setData(Qt::ForegroundRole, QColor(color));
        leaderboard_table_->setItem(row, col, item);
    };
    for (int i = 0; i < board.value().size(); ++i) {
        const auto& row = board.value().at(i);
        const bool ranked = !row.hypothetical && row.resolved >= services::sandbox::kMinResolvedSample;
        const int total_positions = scalar_query_int(
            QStringLiteral("SELECT COUNT(*) FROM sandbox_position WHERE strategy_id = ?"), {row.strategy_id});
        const qint64 first_created = scalar_query_i64(
            QStringLiteral("SELECT MIN(created_at) FROM sandbox_position WHERE strategy_id = ?"), {row.strategy_id});
        const int active_days = first_created > 0
            ? qMax(0, static_cast<int>((QDateTime::currentMSecsSinceEpoch() - first_created) / (24LL * 3600 * 1000)))
            : 0;
        services::sandbox::EligibilityInput in;
        in.active_days = active_days;
        in.resolved = row.resolved;
        in.degraded = row.degraded;
        in.total_positions = total_positions;
        in.net_pnl = row.net_pnl;
        in.max_drawdown = row.max_drawdown;
        in.gross_notional = row.gross_notional;
        in.hypothetical = row.hypothetical;
        const auto verdict = services::sandbox::evaluate_eligibility(in);
        if (verdict.eligible)
            ++eligible_total;
        resolved_total += row.resolved;
        net_total += row.net_pnl;
        const int open_count = scalar_query_int(
            QStringLiteral("SELECT COUNT(*) FROM sandbox_position WHERE strategy_id = ? AND state IN ('open','pending_fill')"),
            {row.strategy_id});
        const QString data = tr("deg %1 / unk %2 / gap %3")
                                 .arg(row.degraded).arg(row.unknown_count).arg(row.data_gap_count);
        add(i, 0, row.kind);
        add(i, 1, QString::number(row.resolved));
        add(i, 2, QString::number(open_count));
        add(i, 3, fmt_money(row.net_pnl),
            row.net_pnl > 0.0 ? ui::colors::POSITIVE() : (row.net_pnl < 0.0 ? ui::colors::NEGATIVE() : ui::colors::TEXT_SECONDARY()));
        add(i, 4, fmt_pct(row.hit_rate));
        add(i, 5, fmt_money(row.max_drawdown), ui::colors::TEXT_SECONDARY());
        add(i, 6, data, row.degraded > 0 || row.data_gap_count > 0 ? ui::colors::AMBER() : ui::colors::TEXT_SECONDARY());
        add(i, 7, ranked ? tr("yes") : tr("no"), ranked ? ui::colors::POSITIVE() : ui::colors::TEXT_SECONDARY());
        add(i, 8, verdict.eligible ? tr("yes") : tr("no"), verdict.eligible ? ui::colors::POSITIVE() : ui::colors::AMBER());
        add(i, 9, verdict.blockers.isEmpty() ? tr("-") : verdict.blockers.join(QStringLiteral("; ")));
        Q_UNUSED(strat_by_id);
    }
    leaderboard_table_->resizeColumnsToContents();
    leaderboard_table_->horizontalHeader()->setStretchLastSection(true);
    if (resolved_count_)
        resolved_count_->setText(QString::number(resolved_total));
    if (net_pnl_) {
        net_pnl_->setText(fmt_money(net_total));
        net_pnl_->setStyleSheet(QString("color:%1;font-size:16px;font-weight:800;background:transparent;%2")
                                    .arg(net_total > 0.0 ? ui::colors::POSITIVE()
                                                         : (net_total < 0.0 ? ui::colors::NEGATIVE()
                                                                            : ui::colors::TEXT_PRIMARY()), MF));
    }
    if (eligible_count_)
        eligible_count_->setText(QString::number(eligible_total));
    if (!board.value().isEmpty())
        set_status(tr("Proof books reflect sandbox/Chronos/Coinbase evidence. Empty samples mean the daemon still needs to collect decisions and ticks."));
}

} // namespace openmarketterminal::screens
