#include "screens/edge_radar/EdgeRadarScreen.h"

#include "services/edge_radar/EdgeRadarService.h"
#include "storage/repositories/EdgeRadarRepository.h"
#include "ui/theme/Theme.h"

#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QAbstractItemView>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QVBoxLayout>

namespace openmarketterminal::screens {

using namespace openmarketterminal::ui::colors;
namespace edge = openmarketterminal::services::edge_radar;

namespace {

QString input_style() {
    return QString("QLineEdit, QTextEdit, QComboBox, QDoubleSpinBox { background:%1; color:%2; border:1px solid %3;"
                   " padding:4px 6px; font-family:'Consolas','Courier New',monospace; font-size:12px; }"
                   "QLineEdit:focus, QTextEdit:focus, QComboBox:focus, QDoubleSpinBox:focus { border-color:%4; }"
                   "QComboBox QAbstractItemView { background:%1; color:%2; selection-background-color:%3; }")
        .arg(BG_BASE(), TEXT_PRIMARY(), BORDER_DIM(), BORDER_BRIGHT());
}

QString button_style(bool primary = false) {
    if (primary) {
        return QString("QPushButton { background:rgba(217,119,6,0.12); color:%1; border:1px solid %2;"
                       " padding:0 12px; min-height:28px; font-weight:700; font-family:'Consolas','Courier New',monospace; }"
                       "QPushButton:hover { background:%1; color:%3; }")
            .arg(AMBER(), AMBER_DIM(), BG_BASE());
    }
    return QString("QPushButton { background:%1; color:%2; border:1px solid %3; padding:0 10px;"
                   " min-height:28px; font-weight:700; font-family:'Consolas','Courier New',monospace; }"
                   "QPushButton:hover { background:%4; color:%5; }")
        .arg(BG_SURFACE(), TEXT_SECONDARY(), BORDER_DIM(), BG_HOVER(), TEXT_PRIMARY());
}

QString table_style() {
    return QString("QTableWidget { background:%1; color:%2; border:1px solid %3; gridline-color:%3;"
                   " font-family:'Consolas','Courier New',monospace; font-size:12px; }"
                   "QTableWidget::item { padding:3px 6px; border-bottom:1px solid %3; }"
                   "QTableWidget::item:selected { background:rgba(217,119,6,0.16); color:%2; }"
                   "QHeaderView::section { background:%4; color:%5; border:1px solid %3; padding:4px 6px;"
                   " font-weight:700; }")
        .arg(BG_BASE(), TEXT_PRIMARY(), BORDER_DIM(), BG_SURFACE(), TEXT_SECONDARY());
}

QLabel* section_label(const QString& text) {
    auto* l = new QLabel(text);
    l->setStyleSheet(QString("color:%1; font-weight:800; font-size:13px; letter-spacing:0;"
                             " font-family:'Consolas','Courier New',monospace;")
                         .arg(AMBER()));
    return l;
}

QDoubleSpinBox* probability_spin(double value) {
    auto* s = new QDoubleSpinBox;
    s->setRange(0.0, 100.0);
    s->setDecimals(2);
    s->setSingleStep(1.0);
    s->setSuffix(QStringLiteral("%"));
    s->setValue(value);
    s->setStyleSheet(input_style());
    return s;
}

QTableWidgetItem* item(const QString& value) {
    auto* i = new QTableWidgetItem(value);
    i->setFlags(i->flags() & ~Qt::ItemIsEditable);
    return i;
}

QString pct(double v) {
    return QString::number(v * 100.0, 'f', 2) + QStringLiteral("%");
}

} // namespace

EdgeRadarScreen::EdgeRadarScreen(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("edgeRadarScreen"));
    setStyleSheet(QString("QWidget#edgeRadarScreen { background:%1; color:%2; }").arg(BG_BASE(), TEXT_PRIMARY()));
    build_ui();
    refresh_table();
    evaluate_current();
}

void EdgeRadarScreen::restore_state(const QVariantMap& state) {
    const QString selected = state.value(QStringLiteral("selected_id")).toString();
    if (selected.isEmpty())
        return;
    auto r = EdgeRadarRepository::instance().get(selected);
    if (r.is_ok()) {
        selected_id_ = selected;
        load_idea(r.value());
    }
}

QVariantMap EdgeRadarScreen::save_state() const {
    return {{QStringLiteral("selected_id"), selected_id_}};
}

void EdgeRadarScreen::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* title = section_label(tr("EDGE RADAR"));
    root->addWidget(title);

    auto* form_panel = new QWidget;
    form_panel->setStyleSheet(QString("background:%1; border:1px solid %2;").arg(BG_SURFACE(), BORDER_DIM()));
    auto* grid = new QGridLayout(form_panel);
    grid->setContentsMargins(12, 12, 12, 12);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);

    asset_class_ = new QComboBox;
    asset_class_->addItems({tr("prediction"), tr("stock")});
    venue_ = new QComboBox;
    venue_->addItems({tr("kalshi"), tr("polymarket"), tr("stocks")});
    symbol_ = new QLineEdit;
    symbol_->setPlaceholderText(tr("Ticker or symbol"));
    market_id_ = new QLineEdit;
    market_id_->setPlaceholderText(tr("Kalshi ticker / Polymarket market id"));
    question_ = new QLineEdit;
    question_->setPlaceholderText(tr("Market question or trade idea"));
    for (auto* w : {asset_class_, venue_})
        w->setStyleSheet(input_style());
    for (auto* w : {symbol_, market_id_, question_})
        w->setStyleSheet(input_style());

    market_prob_ = probability_spin(50.0);
    model_prob_ = probability_spin(55.0);
    spread_cost_ = probability_spin(1.0);
    fee_cost_ = probability_spin(0.5);
    liquidity_ = probability_spin(60.0);
    confidence_ = probability_spin(65.0);

    auto add_label = [grid](const QString& text, int row, int col) {
        auto* l = new QLabel(text);
        l->setStyleSheet(QString("color:%1; font-size:11px; font-weight:700; font-family:'Consolas','Courier New',monospace;")
                             .arg(TEXT_SECONDARY()));
        grid->addWidget(l, row, col);
    };

    add_label(tr("ASSET"), 0, 0);       grid->addWidget(asset_class_, 1, 0);
    add_label(tr("VENUE"), 0, 1);       grid->addWidget(venue_, 1, 1);
    add_label(tr("SYMBOL"), 0, 2);      grid->addWidget(symbol_, 1, 2);
    add_label(tr("MARKET ID"), 0, 3);   grid->addWidget(market_id_, 1, 3);
    add_label(tr("QUESTION"), 2, 0);    grid->addWidget(question_, 3, 0, 1, 4);

    add_label(tr("MARKET PROB"), 4, 0); grid->addWidget(market_prob_, 5, 0);
    add_label(tr("MODEL PROB"), 4, 1);  grid->addWidget(model_prob_, 5, 1);
    add_label(tr("SPREAD COST"), 4, 2); grid->addWidget(spread_cost_, 5, 2);
    add_label(tr("FEE COST"), 4, 3);    grid->addWidget(fee_cost_, 5, 3);
    add_label(tr("LIQUIDITY"), 6, 0);   grid->addWidget(liquidity_, 7, 0);
    add_label(tr("CONFIDENCE"), 6, 1);  grid->addWidget(confidence_, 7, 1);

    auto* metrics = new QWidget;
    auto* metrics_layout = new QHBoxLayout(metrics);
    metrics_layout->setContentsMargins(0, 0, 0, 0);
    metrics_layout->setSpacing(8);
    auto make_metric = [metrics_layout](const QString& name) {
        auto* l = new QLabel(name);
        l->setMinimumHeight(34);
        l->setAlignment(Qt::AlignCenter);
        l->setStyleSheet(QString("background:%1; color:%2; border:1px solid %3; font-size:13px;"
                                 " font-weight:800; font-family:'Consolas','Courier New',monospace;")
                             .arg(BG_BASE(), TEXT_PRIMARY(), BORDER_DIM()));
        metrics_layout->addWidget(l);
        return l;
    };
    side_label_ = make_metric(tr("SIDE -"));
    raw_edge_label_ = make_metric(tr("RAW 0.00%"));
    after_cost_label_ = make_metric(tr("NET 0.00%"));
    recommendation_label_ = make_metric(tr("WATCH"));
    grid->addWidget(metrics, 7, 2, 1, 2);

    root->addWidget(form_panel);

    auto* text_row = new QHBoxLayout;
    thesis_ = new QTextEdit;
    thesis_->setPlaceholderText(tr("Thesis"));
    risks_ = new QTextEdit;
    risks_->setPlaceholderText(tr("Risk notes"));
    for (auto* t : {thesis_, risks_}) {
        t->setMinimumHeight(74);
        t->setMaximumHeight(110);
        t->setStyleSheet(input_style());
        text_row->addWidget(t);
    }
    root->addLayout(text_row);

    auto* actions = new QHBoxLayout;
    auto* evaluate = new QPushButton(tr("EVALUATE"));
    auto* save = new QPushButton(tr("SAVE IDEA"));
    auto* update = new QPushButton(tr("UPDATE SELECTED"));
    auto* close = new QPushButton(tr("CLOSE SELECTED"));
    auto* refresh = new QPushButton(tr("REFRESH"));
    evaluate->setStyleSheet(button_style(true));
    save->setStyleSheet(button_style(true));
    for (auto* b : {update, close, refresh})
        b->setStyleSheet(button_style(false));
    actions->addWidget(evaluate);
    actions->addWidget(save);
    actions->addWidget(update);
    actions->addWidget(close);
    actions->addStretch(1);
    actions->addWidget(refresh);
    root->addLayout(actions);

    table_ = new QTableWidget;
    table_->setColumnCount(10);
    table_->setHorizontalHeaderLabels({tr("Symbol"), tr("Venue"), tr("Asset"), tr("Market"), tr("Model"),
                                       tr("Raw"), tr("Net"), tr("Side"), tr("Call"), tr("Status")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->verticalHeader()->setVisible(false);
    table_->setAlternatingRowColors(true);
    table_->setStyleSheet(table_style());
    root->addWidget(table_, 1);

    status_label_ = new QLabel;
    status_label_->setStyleSheet(QString("color:%1; font-size:12px; font-family:'Consolas','Courier New',monospace;")
                                     .arg(TEXT_SECONDARY()));
    root->addWidget(status_label_);

    connect(evaluate, &QPushButton::clicked, this, &EdgeRadarScreen::evaluate_current);
    connect(save, &QPushButton::clicked, this, &EdgeRadarScreen::save_idea);
    connect(update, &QPushButton::clicked, this, &EdgeRadarScreen::update_selected);
    connect(close, &QPushButton::clicked, this, &EdgeRadarScreen::close_selected);
    connect(refresh, &QPushButton::clicked, this, &EdgeRadarScreen::refresh_table);
    connect(table_, &QTableWidget::cellClicked, this, &EdgeRadarScreen::on_row_selected);
}

EdgeRadarIdea EdgeRadarScreen::collect_idea() const {
    EdgeRadarIdea idea;
    idea.id = selected_id_;
    idea.asset_class = asset_class_->currentText();
    idea.venue = venue_->currentText();
    idea.symbol = symbol_->text().trimmed().toUpper();
    idea.market_id = market_id_->text().trimmed();
    idea.question = question_->text().trimmed();
    idea.market_probability = market_prob_->value() / 100.0;
    idea.model_probability = model_prob_->value() / 100.0;
    idea.spread_cost = spread_cost_->value() / 100.0;
    idea.fee_cost = fee_cost_->value() / 100.0;
    idea.liquidity_score = liquidity_->value() / 100.0;
    idea.confidence = confidence_->value() / 100.0;
    idea.thesis = thesis_->toPlainText().trimmed();
    idea.risk_notes = risks_->toPlainText().trimmed();
    idea.status = QStringLiteral("watching");

    const auto score = edge::EdgeRadarService::evaluate({idea.market_probability, idea.model_probability,
                                                         idea.spread_cost, idea.fee_cost,
                                                         idea.liquidity_score, idea.confidence});
    idea.side = score.side;
    idea.raw_edge = score.raw_edge;
    idea.edge_after_cost = score.edge_after_cost;
    idea.recommendation = score.recommendation;
    if (idea.risk_notes.isEmpty())
        idea.risk_notes = score.risk_notes;
    idea.last_evaluated_at = QDateTime::currentMSecsSinceEpoch();
    return idea;
}

void EdgeRadarScreen::evaluate_current() {
    const EdgeRadarIdea idea = collect_idea();
    set_score_labels(idea);
    risks_->setPlainText(idea.risk_notes);
    set_status(tr("Evaluation updated locally"));
}

void EdgeRadarScreen::save_idea() {
    EdgeRadarIdea idea = collect_idea();
    idea.id.clear();
    auto r = EdgeRadarRepository::instance().create(idea);
    if (r.is_err()) {
        set_status(QString::fromStdString(r.error()), true);
        return;
    }
    selected_id_ = r.value().id;
    refresh_table();
    set_status(tr("Edge idea saved"));
}

void EdgeRadarScreen::update_selected() {
    if (selected_id_.isEmpty()) {
        set_status(tr("Select an idea first"), true);
        return;
    }
    EdgeRadarIdea idea = collect_idea();
    idea.id = selected_id_;
    auto r = EdgeRadarRepository::instance().update(idea);
    if (r.is_err()) {
        set_status(QString::fromStdString(r.error()), true);
        return;
    }
    refresh_table();
    set_status(tr("Selected idea updated"));
}

void EdgeRadarScreen::close_selected() {
    if (selected_id_.isEmpty()) {
        set_status(tr("Select an idea first"), true);
        return;
    }
    auto r = EdgeRadarRepository::instance().get(selected_id_);
    if (r.is_err()) {
        set_status(QString::fromStdString(r.error()), true);
        return;
    }
    EdgeRadarIdea idea = r.value();
    idea.status = QStringLiteral("closed");
    auto u = EdgeRadarRepository::instance().update(idea);
    if (u.is_err()) {
        set_status(QString::fromStdString(u.error()), true);
        return;
    }
    refresh_table();
    set_status(tr("Selected idea closed"));
}

void EdgeRadarScreen::refresh_table() {
    auto r = EdgeRadarRepository::instance().list_all();
    if (r.is_err()) {
        set_status(QString::fromStdString(r.error()), true);
        return;
    }
    table_->setRowCount(0);
    int row = 0;
    for (const auto& idea : r.value()) {
        table_->insertRow(row);
        table_->setItem(row, 0, item(idea.symbol.isEmpty() ? idea.market_id : idea.symbol));
        table_->setItem(row, 1, item(idea.venue));
        table_->setItem(row, 2, item(idea.asset_class));
        table_->setItem(row, 3, item(pct(idea.market_probability)));
        table_->setItem(row, 4, item(pct(idea.model_probability)));
        table_->setItem(row, 5, item(pct(idea.raw_edge)));
        table_->setItem(row, 6, item(pct(idea.edge_after_cost)));
        table_->setItem(row, 7, item(idea.side));
        table_->setItem(row, 8, item(idea.recommendation));
        table_->setItem(row, 9, item(idea.status));
        table_->item(row, 0)->setData(Qt::UserRole, idea.id);
        ++row;
    }
    set_status(tr("%1 edge ideas loaded").arg(row));
}

void EdgeRadarScreen::on_row_selected(int row, int) {
    if (row < 0 || !table_->item(row, 0))
        return;
    const QString id = table_->item(row, 0)->data(Qt::UserRole).toString();
    auto r = EdgeRadarRepository::instance().get(id);
    if (r.is_err()) {
        set_status(QString::fromStdString(r.error()), true);
        return;
    }
    selected_id_ = id;
    load_idea(r.value());
}

void EdgeRadarScreen::load_idea(const EdgeRadarIdea& idea) {
    asset_class_->setCurrentText(idea.asset_class);
    venue_->setCurrentText(idea.venue);
    symbol_->setText(idea.symbol);
    market_id_->setText(idea.market_id);
    question_->setText(idea.question);
    market_prob_->setValue(idea.market_probability * 100.0);
    model_prob_->setValue(idea.model_probability * 100.0);
    spread_cost_->setValue(idea.spread_cost * 100.0);
    fee_cost_->setValue(idea.fee_cost * 100.0);
    liquidity_->setValue(idea.liquidity_score * 100.0);
    confidence_->setValue(idea.confidence * 100.0);
    thesis_->setPlainText(idea.thesis);
    risks_->setPlainText(idea.risk_notes);
    set_score_labels(idea);
    set_status(tr("Loaded %1").arg(idea.symbol.isEmpty() ? idea.market_id : idea.symbol));
}

void EdgeRadarScreen::set_score_labels(const EdgeRadarIdea& idea) {
    side_label_->setText(tr("SIDE %1").arg(idea.side.toUpper()));
    raw_edge_label_->setText(tr("RAW %1").arg(pct(idea.raw_edge)));
    after_cost_label_->setText(tr("NET %1").arg(pct(idea.edge_after_cost)));
    recommendation_label_->setText(idea.recommendation.toUpper());
    const QString rec_color = idea.recommendation == QStringLiteral("candidate") ? POSITIVE()
                              : idea.recommendation == QStringLiteral("watch") ? WARNING()
                                                                               : NEGATIVE();
    recommendation_label_->setStyleSheet(QString("background:%1; color:%2; border:1px solid %3; font-size:13px;"
                                                 " font-weight:800; font-family:'Consolas','Courier New',monospace;")
                                             .arg(BG_BASE(), rec_color, BORDER_DIM()));
}

void EdgeRadarScreen::set_status(const QString& text, bool error) {
    if (!status_label_)
        return;
    status_label_->setText(text);
    status_label_->setStyleSheet(QString("color:%1; font-size:12px; font-family:'Consolas','Courier New',monospace;")
                                     .arg(error ? NEGATIVE() : TEXT_SECONDARY()));
}

} // namespace openmarketterminal::screens
