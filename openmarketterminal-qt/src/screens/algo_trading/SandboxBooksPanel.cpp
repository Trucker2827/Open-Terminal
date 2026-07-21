#include "screens/algo_trading/SandboxBooksPanel.h"
#include "screens/algo_trading/StrategyCockpitNavigation.h"
#include "screens/algo_trading/StrategyEvidencePresentation.h"

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
#include <QJsonArray>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonValue>
#include <QProcess>
#include <QStandardPaths>
#include <QTableWidgetItem>
#include <QVariantList>
#include <QVBoxLayout>

#include <algorithm>

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

class NumericTableItem final : public QTableWidgetItem {
  public:
    using QTableWidgetItem::QTableWidgetItem;
    bool operator<(const QTableWidgetItem& other) const override {
        return data(Qt::UserRole).toDouble() < other.data(Qt::UserRole).toDouble();
    }
};

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

    back_button_ = new QPushButton(tr("← COCKPIT"), top);
    back_button_->setCursor(Qt::PointingHandCursor);
    back_button_->setStyleSheet(button_style(ui::colors::AMBER()));
    back_button_->setVisible(false);
    connect(back_button_, &QPushButton::clicked, this, &SandboxBooksPanel::returnToCockpit);
    top_l->addWidget(back_button_);

    drilldown_badge_ = new QLabel(top);
    drilldown_badge_->setStyleSheet(QString("color:%1;border:1px solid %1;padding:4px 8px;font-size:10px;font-weight:800;%2")
                                        .arg(ui::colors::CYAN(), MF));
    drilldown_badge_->setVisible(false);
    top_l->addWidget(drilldown_badge_);

    show_all_button_ = new QPushButton(tr("SHOW ALL"), top);
    show_all_button_->setCursor(Qt::PointingHandCursor);
    show_all_button_->setStyleSheet(button_style(ui::colors::CYAN()));
    show_all_button_->setVisible(false);
    connect(show_all_button_, &QPushButton::clicked, this, [this]() {
        apply_cockpit_drilldown(static_cast<int>(StrategyCockpitView::EvidenceAll));
    });
    top_l->addWidget(show_all_button_);

    status_label_ = new QLabel(tr("Every row below is an immutable paper experiment. No result can place a live order."), top);
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
    add_btn(tr("PREPARE BOOKS"), ui::colors::AMBER(), [this]() {
        seed_books();
        install_jobs();
    });
    add_btn(tr("RUN PAPER CYCLE"), ui::colors::CYAN(), [this]() { run_tick(); });
    add_btn(tr("RECALCULATE"), ui::colors::POSITIVE(), [this]() { run_score(); });
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
    add_stat(tr("EXPERIMENTS"), active_count_);
    add_stat(tr("OPEN PAPER"), open_count_);
    add_stat(tr("NO EDGE"), no_edge_count_);
    add_stat(tr("RESOLVED"), resolved_count_);
    add_stat(tr("PNL BY BOOK"), net_pnl_);
    add_stat(tr("PROMOTION READY"), eligible_count_);
    root->addWidget(stats);

    leaderboard_table_ = new QTableWidget(this);
    leaderboard_table_->setColumnCount(12);
    leaderboard_table_->setHorizontalHeaderLabels(
        {tr("Strategy"), tr("Symbols"), tr("Venue"), tr("Horizon"), tr("Source"),
         tr("Samples"), tr("Open"), tr("Net P&L"), tr("Hit Rate"), tr("Drawdown"),
         tr("Proof Status"), tr("Next Requirement")});
    leaderboard_table_->verticalHeader()->setVisible(false);
    leaderboard_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    leaderboard_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    leaderboard_table_->setAlternatingRowColors(true);
    leaderboard_table_->setStyleSheet(table_style());
    leaderboard_table_->horizontalHeader()->setStretchLastSection(true);
    leaderboard_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    leaderboard_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    leaderboard_table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    leaderboard_table_->horizontalHeader()->setSectionResizeMode(11, QHeaderView::Stretch);
    leaderboard_table_->setMinimumHeight(260);
    connect(leaderboard_table_, &QTableWidget::itemSelectionChanged,
            this, &SandboxBooksPanel::update_selected_detail);
    root->addWidget(leaderboard_table_, 1);

    detail_label_ = new QLabel(tr("Select a strategy to inspect its immutable configuration, data quality, and all blockers."), this);
    detail_label_->setWordWrap(true);
    detail_label_->setMinimumHeight(72);
    detail_label_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    detail_label_->setStyleSheet(QString("background:%1;color:%2;border-top:1px solid %3;padding:9px;font-size:10px;%4")
                                     .arg(ui::colors::BG_SURFACE(), ui::colors::TEXT_SECONDARY(),
                                          ui::colors::BORDER_DIM(), MF));
    root->addWidget(detail_label_);
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
    populate_leaderboard();
}

void SandboxBooksPanel::apply_cockpit_drilldown(int view, const QString& book_kind) {
    drilldown_view_ = view;
    drilldown_book_kind_ = book_kind;
    populate_leaderboard();
}

void SandboxBooksPanel::apply_current_drilldown() {
    const auto view = static_cast<StrategyCockpitView>(drilldown_view_);
    leaderboard_table_->setSortingEnabled(false);
    if (view == StrategyCockpitView::None || view == StrategyCockpitView::EvidenceAll) {
        for (int row = 0; row < leaderboard_table_->rowCount(); ++row)
            leaderboard_table_->setRowHidden(row, false);
        back_button_->setVisible(view != StrategyCockpitView::None);
        show_all_button_->setVisible(false);
        drilldown_badge_->setVisible(view != StrategyCockpitView::None);
        if (view != StrategyCockpitView::None)
            drilldown_badge_->setText(tr("ALL PROOF BOOKS"));
        return;
    }

    QString title;
    int first_visible = -1;
    for (int row = 0; row < leaderboard_table_->rowCount(); ++row) {
        const QString kind = leaderboard_table_->item(row, 0)->data(Qt::UserRole + 2).toString();
        const QString source = leaderboard_table_->item(row, 4)->text();
        const int samples = leaderboard_table_->item(row, 5)->text().toInt();
        const int open = leaderboard_table_->item(row, 6)->text().toInt();
        const QString proof = leaderboard_table_->item(row, 0)->data(Qt::UserRole + 3).toString();
        const bool show = strategy_evidence_matches(view, kind, source, samples, open, proof,
                                                    drilldown_book_kind_);
        switch (view) {
        case StrategyCockpitView::EvidenceChronos: title = tr("CHRONOS FORECAST COHORTS"); break;
        case StrategyCockpitView::EvidenceOpen: title = tr("OPEN PAPER POSITIONS BY BOOK"); break;
        case StrategyCockpitView::EvidenceResolved: title = tr("RESOLVED EVIDENCE COHORTS"); break;
        case StrategyCockpitView::EvidenceEligible: title = tr("PROMOTION-READY BOOKS"); break;
        case StrategyCockpitView::EvidenceNoEdge: title = tr("NO-EDGE BOOKS · REVISE OR RETIRE"); break;
        case StrategyCockpitView::EvidencePnl: title = tr("COST-NET P&L BY IMMUTABLE BOOK"); break;
        case StrategyCockpitView::EvidenceCoinbase: title = tr("COINBASE-DERIVED EVIDENCE"); break;
        case StrategyCockpitView::EvidenceKalshi: title = tr("KALSHI / ODDS EVIDENCE"); break;
        case StrategyCockpitView::EvidenceBook: title = tr("PROOF BOOK · %1").arg(drilldown_book_kind_.toUpper()); break;
        default: break;
        }
        leaderboard_table_->setRowHidden(row, !show);
        if (show && first_visible < 0)
            first_visible = row;
    }
    back_button_->setVisible(true);
    show_all_button_->setVisible(true);
    drilldown_badge_->setVisible(true);
    int visible_count = 0;
    for (int row = 0; row < leaderboard_table_->rowCount(); ++row)
        if (!leaderboard_table_->isRowHidden(row)) ++visible_count;
    drilldown_badge_->setText(tr("%1 · %2 BOOK%3")
                                  .arg(title).arg(visible_count).arg(visible_count == 1 ? QString() : QStringLiteral("S")));
    set_status(first_visible >= 0
        ? tr("Contextual cockpit drill-down. Metrics remain cost-net, immutable, and paper-only.")
        : tr("No rows currently match this cockpit cohort."),
        first_visible >= 0 ? ui::colors::CYAN() : ui::colors::AMBER());
    if (first_visible >= 0) {
        if (view == StrategyCockpitView::EvidencePnl) {
            leaderboard_table_->setSortingEnabled(true);
            leaderboard_table_->sortItems(7, Qt::AscendingOrder);
            first_visible = 0;
        }
        leaderboard_table_->selectRow(first_visible);
        leaderboard_table_->scrollToItem(leaderboard_table_->item(first_visible, 0));
    } else {
        QString requirement;
        switch (view) {
        case StrategyCockpitView::EvidenceEligible:
            requirement = tr("No book has cleared every promotion requirement yet. Continue collecting resolved, cost-net evidence."); break;
        case StrategyCockpitView::EvidenceOpen:
            requirement = tr("No paper position is open. This is healthy when current proposals fail deterministic entry gates."); break;
        case StrategyCockpitView::EvidenceCoinbase:
            requirement = tr("No proof book currently declares Coinbase provenance. Inspect all books or verify the journal_source metadata."); break;
        case StrategyCockpitView::EvidenceKalshi:
            requirement = tr("No Kalshi proof book currently matches. Prepare the default books or inspect all immutable experiments."); break;
        default:
            requirement = tr("This cohort is empty under the current evidence snapshot. Use SHOW ALL or return to the cockpit."); break;
        }
        detail_label_->setText(tr("EMPTY COHORT — NOT A DEAD END\n%1\n\nAvailable actions: SHOW ALL preserves context; ← COCKPIT returns to the map.")
                                   .arg(requirement));
    }
}

void SandboxBooksPanel::populate_leaderboard() {
    leaderboard_table_->setSortingEnabled(false);
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

    QList<services::sandbox::LeaderboardRow> visible;
    for (const auto& r : board.value()) {
        const auto it = strat_by_id.constFind(r.strategy_id);
        if (it != strat_by_id.constEnd() && it->status == QStringLiteral("retired"))
            continue; // hide retired books, matching the books table above
        visible.append(r);
    }
    leaderboard_table_->setRowCount(visible.size());
    int active_total = 0;
    int resolved_total = 0;
    int eligible_total = 0;
    int no_edge_total = 0;
    auto add = [&](int row, int col, const QString& text, const QString& color = {},
                   const QVariant& numeric_sort = {}) {
        QTableWidgetItem* item = numeric_sort.isValid()
            ? static_cast<QTableWidgetItem*>(new NumericTableItem(text))
            : new QTableWidgetItem(text);
        if (numeric_sort.isValid())
            item->setData(Qt::UserRole, numeric_sort);
        if (!color.isEmpty())
            item->setData(Qt::ForegroundRole, QColor(color));
        leaderboard_table_->setItem(row, col, item);
    };
    for (int i = 0; i < visible.size(); ++i) {
        const auto& row = visible.at(i);
        const auto strategy_it = strat_by_id.constFind(row.strategy_id);
        const services::sandbox::StrategyRow strategy = strategy_it == strat_by_id.constEnd()
            ? services::sandbox::StrategyRow{} : strategy_it.value();
        const QJsonObject params = parse_params(strategy.params_json);
        if (strategy.status == QStringLiteral("active"))
            ++active_total;
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
        const StrategyProofState proof_state = strategy_proof_state(
            row.hypothetical, verdict.eligible, row.resolved, row.net_pnl,
            services::sandbox::kMinResolvedSample);
        const bool no_edge = proof_state == StrategyProofState::NoEdge;
        if (no_edge)
            ++no_edge_total;
        resolved_total += row.resolved;
        const int open_count = scalar_query_int(
            QStringLiteral("SELECT COUNT(*) FROM sandbox_position WHERE strategy_id = ? AND state IN ('open','pending_fill')"),
            {row.strategy_id});
        const QString venue = params.value(QStringLiteral("venue")).toString(QStringLiteral("-"));
        QString horizon = params.value(QStringLiteral("horizon")).toString();
        if (horizon.isEmpty() && params.contains(QStringLiteral("horizon_sec")))
            horizon = tr("%1s").arg(params.value(QStringLiteral("horizon_sec")).toInt());
        if (horizon.isEmpty()) horizon = QStringLiteral("-");
        const QString source = params.value(QStringLiteral("journal_source")).toString(
            params.value(QStringLiteral("source")).toString(QStringLiteral("-")));
        QString proof_status;
        QString proof_code;
        QString proof_color;
        if (proof_state == StrategyProofState::Hypothetical) {
            proof_status = tr("HYPOTHETICAL");
            proof_code = QStringLiteral("HYPOTHETICAL");
            proof_color = ui::colors::TEXT_SECONDARY();
        } else if (proof_state == StrategyProofState::PromotionReady) {
            proof_status = tr("PROMOTION READY");
            proof_code = QStringLiteral("PROMOTION READY");
            proof_color = ui::colors::POSITIVE();
        } else if (proof_state == StrategyProofState::NoEdge) {
            proof_status = tr("NO EDGE");
            proof_code = QStringLiteral("NO EDGE");
            proof_color = ui::colors::NEGATIVE();
        } else if (proof_state == StrategyProofState::Blocked) {
            proof_status = tr("BLOCKED");
            proof_code = QStringLiteral("BLOCKED");
            proof_color = ui::colors::AMBER();
        } else {
            proof_status = tr("COLLECTING");
            proof_code = QStringLiteral("COLLECTING");
            proof_color = ui::colors::CYAN();
        }
        QString next_requirement;
        if (proof_state == StrategyProofState::NoEdge)
            next_requirement = tr("Negative net after cost; revise or retire");
        else if (proof_state == StrategyProofState::Hypothetical)
            next_requirement = tr("Hypothetical instrument; evidence only");
        else if (proof_state == StrategyProofState::PromotionReady)
            next_requirement = tr("Manual review; never auto-promoted");
        else
            next_requirement = verdict.blockers.isEmpty() ? tr("Continue paper observation") : verdict.blockers.first();

        QString strategy_name = row.kind;
        if (row.kind == QStringLiteral("kalshi") &&
            params.value(QStringLiteral("experiment_protocol")).toString() == QStringLiteral("kalshi-v2")) {
            strategy_name = tr("Kalshi %1 %2 %3")
                                .arg(params.value(QStringLiteral("horizon")).toString(),
                                     params.value(QStringLiteral("entry_cohort")).toString(),
                                     params.value(QStringLiteral("exit_policy")).toString());
        } else if (row.kind == QStringLiteral("scalp")) {
            strategy_name = tr("Scalp %1").arg(venue);
        }
        add(i, 0, strategy_name);
        add(i, 1, strategy.symbols.isEmpty() ? QStringLiteral("-") : strategy.symbols);
        add(i, 2, venue);
        add(i, 3, horizon);
        add(i, 4, source);
        add(i, 5, QString::number(row.resolved));
        add(i, 6, QString::number(open_count));
        add(i, 7, fmt_money(row.net_pnl),
            row.net_pnl > 0.0 ? ui::colors::POSITIVE() : (row.net_pnl < 0.0 ? ui::colors::NEGATIVE() : ui::colors::TEXT_SECONDARY()),
            row.net_pnl);
        add(i, 8, fmt_pct(row.hit_rate));
        add(i, 9, fmt_money(row.max_drawdown), row.max_drawdown > 0.0 ? ui::colors::NEGATIVE() : ui::colors::TEXT_SECONDARY());
        add(i, 10, proof_status, proof_color);
        add(i, 11, next_requirement, verdict.blockers.isEmpty() ? ui::colors::TEXT_SECONDARY() : ui::colors::AMBER());

        QJsonObject detail{{QStringLiteral("strategy_id"), row.strategy_id},
                           {QStringLiteral("params"), params},
                           {QStringLiteral("notes"), strategy.notes},
                           {QStringLiteral("active_days"), active_days},
                           {QStringLiteral("total_positions"), total_positions},
                           {QStringLiteral("degraded"), row.degraded},
                           {QStringLiteral("unknown"), row.unknown_count},
                           {QStringLiteral("data_gaps"), row.data_gap_count},
                           {QStringLiteral("blockers"), QJsonArray::fromStringList(verdict.blockers)}};
        leaderboard_table_->item(i, 0)->setData(Qt::UserRole, row.strategy_id);
        leaderboard_table_->item(i, 0)->setData(Qt::UserRole + 1, detail);
        leaderboard_table_->item(i, 0)->setData(Qt::UserRole + 2, row.kind);
        leaderboard_table_->item(i, 0)->setData(Qt::UserRole + 3, proof_code);
    }
    if (active_count_)
        active_count_->setText(QString::number(active_total));
    if (resolved_count_)
        resolved_count_->setText(QString::number(resolved_total));
    if (net_pnl_) {
        net_pnl_->setText(tr("SEPARATE"));
        net_pnl_->setStyleSheet(QString("color:%1;font-size:16px;font-weight:800;background:transparent;%2")
                                    .arg(ui::colors::CYAN(), MF));
    }
    if (eligible_count_)
        eligible_count_->setText(QString::number(eligible_total));
    if (no_edge_count_) {
        no_edge_count_->setText(QString::number(no_edge_total));
        no_edge_count_->setStyleSheet(QString("color:%1;font-size:16px;font-weight:800;background:transparent;%2")
                                          .arg(no_edge_total > 0 ? ui::colors::NEGATIVE() : ui::colors::TEXT_PRIMARY(), MF));
    }
    populate_position_counts();
    update_selected_detail();
    if (visible.isEmpty())
        set_status(tr("No proof books exist yet. PREPARE BOOKS creates the default paper experiments."), ui::colors::AMBER());
    else if (no_edge_total > 0)
        set_status(tr("%1 experiment(s) have enough samples and currently show no positive net edge after cost.").arg(no_edge_total),
                   ui::colors::NEGATIVE());
    else
        set_status(tr("Paper evidence is accumulating. Promotion remains report-only and requires manual review."));
    apply_current_drilldown();
}

void SandboxBooksPanel::populate_position_counts() {
    if (!open_count_)
        return;
    const int open_positions = scalar_query_int(
        QStringLiteral("SELECT COUNT(*) FROM sandbox_position WHERE state IN ('open','pending_fill')"), {});
    open_count_->setText(QString::number(open_positions));
    open_count_->setStyleSheet(QString("color:%1;font-size:16px;font-weight:800;background:transparent;%2")
                                   .arg(open_positions > 0 ? ui::colors::AMBER() : ui::colors::TEXT_PRIMARY(), MF));
}

void SandboxBooksPanel::update_selected_detail() {
    if (!detail_label_)
        return;
    const int row = leaderboard_table_->currentRow();
    if (row < 0 || !leaderboard_table_->item(row, 0)) {
        detail_label_->setText(tr("Select a strategy to inspect its immutable configuration, data quality, and all blockers."));
        return;
    }
    const QJsonObject detail = leaderboard_table_->item(row, 0)->data(Qt::UserRole + 1).toJsonObject();
    QStringList blockers;
    for (const auto& value : detail.value(QStringLiteral("blockers")).toArray())
        blockers << value.toString();
    const QString params = QString::fromUtf8(QJsonDocument(detail.value(QStringLiteral("params")).toObject())
                                                  .toJson(QJsonDocument::Compact));
    QString evidence_breakdown;
    const QString strategy_id = detail.value(QStringLiteral("strategy_id")).toString();
    auto evidence = Database::instance().execute(
        "SELECT position.side, COUNT(*),"
        " SUM(CASE WHEN position.realized_pnl IS NOT NULL THEN 1 ELSE 0 END),"
        " COALESCE(SUM(position.realized_pnl),0), AVG(position.limit_price), AVG(journal.seconds_left)"
        " FROM sandbox_position position"
        " JOIN edge_decision_journal journal ON journal.id=position.decision_id"
        " WHERE position.strategy_id=? GROUP BY position.side ORDER BY position.side",
        {strategy_id});
    if (evidence.is_ok()) {
        QStringList sides;
        while (evidence.value().next()) {
            sides << tr("%1: entries %2, resolved/exited %3, net %4, avg entry %5c, avg time left %6s")
                         .arg(evidence.value().value(0).toString().toUpper())
                         .arg(evidence.value().value(1).toInt())
                         .arg(evidence.value().value(2).toInt())
                         .arg(fmt_money(evidence.value().value(3).toDouble()))
                         .arg(evidence.value().value(4).toDouble() * 100.0, 0, 'f', 1)
                         .arg(evidence.value().value(5).toDouble(), 0, 'f', 0);
        }
        if (!sides.isEmpty())
            evidence_breakdown = QStringLiteral("\nSide/time evidence: ") + sides.join(QStringLiteral("  |  "));
    }
    detail_label_->setText(
        tr("Immutable ID: %1  |  Active days: %2  |  Positions: %3  |  Data quality: degraded %4, unknown %5, gaps %6\n"
           "Configuration: %7%8\nBlockers: %9")
            .arg(strategy_id)
            .arg(detail.value(QStringLiteral("active_days")).toInt())
            .arg(detail.value(QStringLiteral("total_positions")).toInt())
            .arg(detail.value(QStringLiteral("degraded")).toInt())
            .arg(detail.value(QStringLiteral("unknown")).toInt())
            .arg(detail.value(QStringLiteral("data_gaps")).toInt())
            .arg(params)
            .arg(evidence_breakdown)
            .arg(blockers.isEmpty() ? tr("none; manual review still required") : blockers.join(QStringLiteral("; "))));
}

} // namespace openmarketterminal::screens
