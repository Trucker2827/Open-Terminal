#include "screens/weather/WeatherPredictionsScreen.h"

#include "storage/sqlite/Database.h"
#include "ui/theme/Theme.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace openmarketterminal::screens {

namespace {

static const char* MF = "font-family:'Consolas',monospace;";

int q_int(const QString& sql) {
    auto r = Database::instance().execute(sql, {});
    if (!r.is_ok() || !r.value().next())
        return 0;
    return r.value().value(0).toInt();
}

double q_double(const QString& sql) {
    auto r = Database::instance().execute(sql, {});
    if (!r.is_ok() || !r.value().next() || r.value().value(0).isNull())
        return 0.0;
    return r.value().value(0).toDouble();
}

QString table_style() {
    return QString("QTableWidget{background:%1;color:%2;border:none;gridline-color:%3;font-size:11px;%4}"
                   "QHeaderView::section{background:%5;color:%6;border:1px solid %3;padding:4px;"
                   "font-size:10px;font-weight:700;%4}"
                   "QTableWidget::item{padding:3px 6px;border-bottom:1px solid %3;}")
        .arg(ui::colors::BG_BASE(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(), MF,
             ui::colors::BG_RAISED(), ui::colors::TEXT_SECONDARY());
}

QLabel* make_stat(QLabel** value_out, const QString& caption, QVBoxLayout* into) {
    auto* box = new QVBoxLayout;
    auto* cap = new QLabel(caption);
    cap->setStyleSheet(QString("color:%1;font-size:9px;font-weight:700;background:transparent;%2")
                           .arg(ui::colors::TEXT_TERTIARY(), MF));
    auto* val = new QLabel(QStringLiteral("-"));
    val->setStyleSheet(QString("color:%1;font-size:16px;font-weight:800;background:transparent;%2")
                           .arg(ui::colors::TEXT_PRIMARY(), MF));
    box->addWidget(cap);
    box->addWidget(val);
    box->setSpacing(0);
    into->addLayout(box);
    *value_out = val;
    return val;
}

QTableWidgetItem* cell(const QString& text, const QString& color = {}) {
    auto* it = new QTableWidgetItem(text);
    if (!color.isEmpty())
        it->setForeground(QColor(color));
    return it;
}

} // namespace

WeatherPredictionsScreen::WeatherPredictionsScreen(QWidget* parent) : QWidget(parent) {
    build_ui();
    timer_ = new QTimer(this);
    timer_->setInterval(30'000);   // periodic refresh while visible
    connect(timer_, &QTimer::timeout, this, &WeatherPredictionsScreen::refresh);
}

void WeatherPredictionsScreen::build_ui() {
    setStyleSheet(QString("background:%1;").arg(ui::colors::BG_BASE()));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(10);

    auto* title = new QLabel(tr("KALSHI WEATHER — daily city high-temp forecast edge (paper)"));
    title->setStyleSheet(QString("color:%1;font-size:15px;font-weight:800;background:transparent;%2")
                             .arg(ui::colors::AMBER(), MF));
    root->addWidget(title);

    auto* subtitle = new QLabel(tr("Forecast-vs-market edge on genuinely-uncertain brackets. "
                                   "Separate from Bitcoin predictions; both run in parallel."));
    subtitle->setStyleSheet(QString("color:%1;font-size:10px;background:transparent;%2")
                                .arg(ui::colors::TEXT_TERTIARY(), MF));
    root->addWidget(subtitle);

    // Summary stat row
    auto* stats = new QHBoxLayout;
    stats->setSpacing(28);
    {
        auto add = [&](QLabel** out, const QString& cap) {
            auto* col = new QVBoxLayout;
            make_stat(out, cap, col);
            stats->addLayout(col);
        };
        add(&open_count_, tr("OPEN POSITIONS"));
        add(&resolved_count_, tr("RESOLVED"));
        add(&win_count_, tr("WINS"));
        add(&net_pnl_, tr("NET P&L (paper)"));
    }
    stats->addStretch();
    root->addLayout(stats);

    status_label_ = new QLabel(QStringLiteral("-"));
    status_label_->setStyleSheet(QString("color:%1;font-size:10px;background:transparent;%2")
                                     .arg(ui::colors::TEXT_SECONDARY(), MF));
    root->addWidget(status_label_);

    // Recent forecast decisions
    auto* dec_hdr = new QLabel(tr("RECENT FORECAST DECISIONS"));
    dec_hdr->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;%2")
                               .arg(ui::colors::CYAN(), MF));
    root->addWidget(dec_hdr);
    decisions_table_ = new QTableWidget(0, 7, this);
    decisions_table_->setHorizontalHeaderLabels(
        {tr("Time"), tr("Market"), tr("Side"), tr("Forecast P"), tr("Mkt Price"), tr("Edge"), tr("Gate")});
    decisions_table_->setStyleSheet(table_style());
    decisions_table_->verticalHeader()->setVisible(false);
    decisions_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    decisions_table_->setSelectionMode(QAbstractItemView::NoSelection);
    decisions_table_->horizontalHeader()->setStretchLastSection(true);
    decisions_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    root->addWidget(decisions_table_, 1);

    // Positions
    auto* pos_hdr = new QLabel(tr("WEATHER PAPER POSITIONS"));
    pos_hdr->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;%2")
                               .arg(ui::colors::CYAN(), MF));
    root->addWidget(pos_hdr);
    positions_table_ = new QTableWidget(0, 8, this);
    positions_table_->setHorizontalHeaderLabels(
        {tr("Time"), tr("Market"), tr("Side"), tr("Entry"), tr("Qty"), tr("State"), tr("P&L"), tr("Reason")});
    positions_table_->setStyleSheet(table_style());
    positions_table_->verticalHeader()->setVisible(false);
    positions_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    positions_table_->setSelectionMode(QAbstractItemView::NoSelection);
    positions_table_->horizontalHeader()->setStretchLastSection(true);
    positions_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    root->addWidget(positions_table_, 1);
}

void WeatherPredictionsScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh();
    if (timer_)
        timer_->start();
}

void WeatherPredictionsScreen::refresh() {
    populate_summary();
    populate_decisions();
    populate_positions();
}

void WeatherPredictionsScreen::populate_summary() {
    const QString base =
        "FROM sandbox_position p JOIN sandbox_strategy s ON s.strategy_id=p.strategy_id "
        "WHERE s.kind='kalshi_weather'";
    const int open = q_int("SELECT COUNT(*) " + base + " AND p.state IN ('open','pending_fill')");
    const int resolved = q_int("SELECT COUNT(*) " + base + " AND p.state='closed'");
    const int wins = q_int("SELECT COUNT(*) " + base + " AND p.state='closed' AND p.realized_pnl>0");
    const double pnl = q_double("SELECT COALESCE(SUM(p.realized_pnl),0) " + base);

    if (open_count_)
        open_count_->setText(QString::number(open));
    if (resolved_count_)
        resolved_count_->setText(QString::number(resolved));
    if (win_count_)
        win_count_->setText(resolved > 0 ? QStringLiteral("%1 (%2%)").arg(wins).arg(100 * wins / resolved)
                                         : QString::number(wins));
    if (net_pnl_) {
        net_pnl_->setText(QString::asprintf("%+.2f", pnl));
        net_pnl_->setStyleSheet(QString("color:%1;font-size:16px;font-weight:800;background:transparent;%2")
                                    .arg(pnl >= 0 ? ui::colors::POSITIVE() : ui::colors::NEGATIVE(), MF));
    }

    const bool strategy_active =
        q_int("SELECT COUNT(*) FROM sandbox_strategy WHERE kind='kalshi_weather' AND status='active'") > 0;
    const int recent_decisions =
        q_int("SELECT COUNT(*) FROM edge_decision_journal WHERE source='kalshi weather-plan'");
    if (status_label_) {
        if (!strategy_active)
            status_label_->setText(tr("Weather lane not seeded yet (run: sandbox seed)."));
        else if (recent_decisions == 0)
            status_label_->setText(tr("Weather lane active — awaiting the morning forecast window "
                                      "(producer runs during US active hours)."));
        else
            status_label_->setText(tr("Weather lane active — %1 forecast decisions journaled.")
                                       .arg(recent_decisions));
    }
}

void WeatherPredictionsScreen::populate_decisions() {
    if (!decisions_table_)
        return;
    decisions_table_->setRowCount(0);
    auto r = Database::instance().execute(
        "SELECT created_at, market_id, side, model_probability, market_probability, edge_after_cost, gate "
        "FROM edge_decision_journal WHERE source='kalshi weather-plan' "
        "ORDER BY created_at DESC LIMIT 60",
        {});
    if (!r.is_ok())
        return;
    auto& q = r.value();
    int row = 0;
    while (q.next()) {
        decisions_table_->insertRow(row);
        const auto ts = QDateTime::fromMSecsSinceEpoch(q.value(0).toLongLong());
        const double edge = q.value(5).toDouble();
        const QString gate = q.value(6).toString();
        decisions_table_->setItem(row, 0, cell(ts.toString(QStringLiteral("MM-dd HH:mm"))));
        decisions_table_->setItem(row, 1, cell(q.value(1).toString()));
        decisions_table_->setItem(row, 2, cell(q.value(2).toString().toUpper(),
                                               q.value(2).toString() == "yes" ? ui::colors::POSITIVE()
                                                                              : ui::colors::CYAN()));
        decisions_table_->setItem(row, 3, cell(QString::asprintf("%.2f", q.value(3).toDouble())));
        decisions_table_->setItem(row, 4, cell(QString::asprintf("%.2f", q.value(4).toDouble())));
        decisions_table_->setItem(row, 5, cell(QString::asprintf("%+.3f", edge),
                                               edge >= 0 ? ui::colors::POSITIVE() : ui::colors::NEGATIVE()));
        decisions_table_->setItem(row, 6, cell(gate.toUpper(),
                                               gate == "pass" ? ui::colors::AMBER() : ui::colors::TEXT_TERTIARY()));
        ++row;
    }
}

void WeatherPredictionsScreen::populate_positions() {
    if (!positions_table_)
        return;
    positions_table_->setRowCount(0);
    auto r = Database::instance().execute(
        "SELECT p.created_at, j.market_id, p.side, p.limit_price, p.qty, p.state, p.realized_pnl, p.close_reason "
        "FROM sandbox_position p JOIN sandbox_strategy s ON s.strategy_id=p.strategy_id "
        "JOIN edge_decision_journal j ON j.id=p.decision_id "
        "WHERE s.kind='kalshi_weather' ORDER BY p.created_at DESC LIMIT 100",
        {});
    if (!r.is_ok())
        return;
    auto& q = r.value();
    int row = 0;
    while (q.next()) {
        positions_table_->insertRow(row);
        const auto ts = QDateTime::fromMSecsSinceEpoch(q.value(0).toLongLong());
        const QString state = q.value(5).toString();
        const bool has_pnl = !q.value(6).isNull();
        const double pnl = q.value(6).toDouble();
        positions_table_->setItem(row, 0, cell(ts.toString(QStringLiteral("MM-dd HH:mm"))));
        positions_table_->setItem(row, 1, cell(q.value(1).toString()));
        positions_table_->setItem(row, 2, cell(q.value(2).toString().toUpper()));
        positions_table_->setItem(row, 3, cell(QString::asprintf("%.2f", q.value(3).toDouble())));
        positions_table_->setItem(row, 4, cell(QString::asprintf("%.0f", q.value(4).toDouble())));
        positions_table_->setItem(row, 5, cell(state.toUpper(),
                                               state == "open" ? ui::colors::AMBER() : ui::colors::TEXT_SECONDARY()));
        positions_table_->setItem(row, 6, cell(has_pnl ? QString::asprintf("%+.2f", pnl) : QStringLiteral("-"),
                                               !has_pnl ? ui::colors::TEXT_TERTIARY()
                                                        : (pnl >= 0 ? ui::colors::POSITIVE() : ui::colors::NEGATIVE())));
        positions_table_->setItem(row, 7, cell(q.value(7).toString()));
        ++row;
    }
}

} // namespace openmarketterminal::screens
