#include "screens/dashboard/widgets/CryptoMicrostructureWidget.h"

#include "ui/theme/Theme.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace openmarketterminal::screens::widgets {

namespace {
const QStringList kDefaultSources = {"coinbase", "kraken", "bitcointicker"};

QStringList clean_sources(QStringList sources) {
    QStringList out;
    for (QString s : sources) {
        s = s.trimmed().toLower();
        if (!s.isEmpty() && !out.contains(s))
            out.append(s);
    }
    return out.isEmpty() ? kDefaultSources : out;
}

QString signed_pct(double value) {
    const QString sign = value > 0.0 ? "+" : "";
    return QString("%1%2%").arg(sign).arg(value, 0, 'f', 3);
}
} // namespace

CryptoMicrostructureWidget::CryptoMicrostructureWidget(const QJsonObject& cfg, QWidget* parent)
    : BaseWidget(tr("BTC MICROSTRUCTURE"), parent), feed_(this) {
    auto* vl = content_layout();
    vl->setContentsMargins(10, 8, 10, 8);
    vl->setSpacing(6);

    call_label_ = new QLabel(tr("CONNECTING"));
    call_label_->setAlignment(Qt::AlignCenter);
    vl->addWidget(call_label_);

    detail_label_ = new QLabel(tr("Waiting for live ticks"));
    detail_label_->setAlignment(Qt::AlignCenter);
    detail_label_->setWordWrap(true);
    vl->addWidget(detail_label_);

    price_label_ = new QLabel("PRICE -");
    price_label_->setAlignment(Qt::AlignCenter);
    vl->addWidget(price_label_);

    book_label_ = new QLabel("BID -  ASK -  SPREAD -");
    book_label_->setAlignment(Qt::AlignCenter);
    book_label_->setWordWrap(true);
    vl->addWidget(book_label_);

    pressure_label_ = new QLabel("TAPE -  BOOK -");
    pressure_label_->setAlignment(Qt::AlignCenter);
    vl->addWidget(pressure_label_);

    windows_table_ = new QTableWidget(3, 4, this);
    windows_table_->setHorizontalHeaderLabels({tr("Window"), tr("Move"), tr("Pressure"), tr("Ticks")});
    windows_table_->verticalHeader()->setVisible(false);
    windows_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    windows_table_->setSelectionMode(QAbstractItemView::NoSelection);
    windows_table_->setShowGrid(false);
    windows_table_->horizontalHeader()->setStretchLastSection(true);
    windows_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    windows_table_->setFixedHeight(88);
    vl->addWidget(windows_table_);

    sources_table_ = new QTableWidget(0, 4, this);
    sources_table_->setHorizontalHeaderLabels({tr("Source"), tr("Bid"), tr("Ask"), tr("Age")});
    sources_table_->verticalHeader()->setVisible(false);
    sources_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    sources_table_->setSelectionMode(QAbstractItemView::NoSelection);
    sources_table_->setShowGrid(false);
    sources_table_->horizontalHeader()->setStretchLastSection(true);
    sources_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    vl->addWidget(sources_table_, 1);

    rationale_label_ = new QLabel;
    rationale_label_->setWordWrap(true);
    vl->addWidget(rationale_label_);

    render_timer_ = new QTimer(this);
    render_timer_->setInterval(1000);
    connect(render_timer_, &QTimer::timeout, this, &CryptoMicrostructureWidget::render);
    connect(&feed_, &services::crypto_latency::CryptoLatencyService::tick_received,
            this, &CryptoMicrostructureWidget::on_tick);
    connect(&feed_, &services::crypto_latency::CryptoLatencyService::snapshot_changed,
            this, &CryptoMicrostructureWidget::render);
    connect(this, &BaseWidget::refresh_requested, this, [this]() {
        stop_feed();
        start_feed();
    });

    set_configurable(true);
    apply_styles();
    apply_config(cfg);
}

CryptoMicrostructureWidget::~CryptoMicrostructureWidget() {
    stop_feed();
}

QJsonObject CryptoMicrostructureWidget::config() const {
    QJsonObject o;
    o.insert("symbol", symbol_);
    QJsonArray arr;
    for (const auto& source : sources_)
        arr.append(source);
    o.insert("sources", arr);
    return o;
}

void CryptoMicrostructureWidget::apply_config(const QJsonObject& cfg) {
    symbol_ = services::crypto_latency::CryptoLatencyService::normalize_symbol(
        cfg.value("symbol").toString(symbol_));
    if (symbol_.isEmpty())
        symbol_ = QStringLiteral("BTC-USD");
    sources_ = normalize_sources(cfg.value("sources").toArray());
    set_title(symbol_.startsWith("BTC", Qt::CaseInsensitive) ? tr("BTC MICROSTRUCTURE")
                                                             : symbol_ + tr(" MICROSTRUCTURE"));
    radar_.clear();
    render();
    if (isVisible()) {
        stop_feed();
        start_feed();
    }
}

void CryptoMicrostructureWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    start_feed();
}

void CryptoMicrostructureWidget::hideEvent(QHideEvent* e) {
    BaseWidget::hideEvent(e);
    stop_feed();
}

void CryptoMicrostructureWidget::start_feed() {
    if (feed_active_)
        return;
    radar_.clear();
    set_loading(true);
    feed_.start(symbol_, sources_);
    render_timer_->start();
    feed_active_ = true;
}

void CryptoMicrostructureWidget::stop_feed() {
    if (render_timer_)
        render_timer_->stop();
    feed_.stop();
    feed_active_ = false;
}

void CryptoMicrostructureWidget::on_tick(const services::crypto_latency::CryptoLatencyTick& tick) {
    radar_.add_tick(tick);
    render();
}

void CryptoMicrostructureWidget::render() {
    const auto snap = radar_.snapshot(feed_.snapshot());
    call_label_->setText(QString("%1  %2  %3%")
                             .arg(snap.call, snap.direction.toUpper())
                             .arg(snap.confidence, 0, 'f', 1));
    QColor call_color = ui::colors::TEXT_TERTIARY();
    if (snap.call == QStringLiteral("TRADE CANDIDATE"))
        call_color = ui::colors::POSITIVE();
    else if (snap.call == QStringLiteral("NO TRADE"))
        call_color = ui::colors::NEGATIVE();
    else if (snap.call == QStringLiteral("WATCH"))
        call_color = ui::colors::ORANGE();
    call_label_->setStyleSheet(QString("color:%1;font-size:18px;font-weight:900;background:transparent;")
                                   .arg(call_color.name()));

    detail_label_->setText(QString("%1 live  freshest %2  divergence %3bps")
                               .arg(snap.live_sources)
                               .arg(snap.freshest_source.isEmpty() ? "-" : snap.freshest_source)
                               .arg(snap.cross_source_spread_bps, 0, 'f', 2));
    price_label_->setText(QString("PRICE %1").arg(fmt_price(snap.reference_price)));

    double best_bid = 0.0;
    double best_ask = 0.0;
    for (const auto& source : snap.sources) {
        if (source.best_bid > 0.0)
            best_bid = qMax(best_bid, source.best_bid);
        if (source.best_ask > 0.0)
            best_ask = best_ask <= 0.0 ? source.best_ask : qMin(best_ask, source.best_ask);
    }
    const double spread = (best_bid > 0.0 && best_ask > 0.0 && best_ask >= best_bid)
                              ? ((best_ask - best_bid) / ((best_ask + best_bid) / 2.0)) * 10000.0
                              : 0.0;
    book_label_->setText(QString("BID %1  ASK %2  SPREAD %3bps")
                             .arg(fmt_price(best_bid), fmt_price(best_ask))
                             .arg(spread, 0, 'f', 2));
    pressure_label_->setText(QString("TAPE %1  BOOK %2  TICKS %3")
                                 .arg(snap.tape_pressure, 0, 'f', 2)
                                 .arg(snap.book_pressure, 0, 'f', 2)
                                 .arg(snap.tick_count));
    rationale_label_->setText(snap.rationale);

    render_windows(snap);
    render_sources(snap);
    set_loading(snap.tick_count <= 0 && feed_active_);
}

void CryptoMicrostructureWidget::render_windows(
    const services::edge_radar::CryptoMicrostructureSnapshot& snap) {
    windows_table_->setRowCount(snap.windows.size());
    for (int i = 0; i < snap.windows.size(); ++i) {
        const auto& w = snap.windows[i];
        const int ticks = w.upticks + w.downticks + w.flat_ticks;
        QList<QTableWidgetItem*> items = {
            new QTableWidgetItem(QString("%1s").arg(w.seconds)),
            new QTableWidgetItem(w.available ? signed_pct(w.move_pct) : "-"),
            new QTableWidgetItem(w.available ? QString::number(w.tape_pressure, 'f', 2) : "-"),
            new QTableWidgetItem(QString::number(ticks)),
        };
        const QColor col = w.tape_pressure >= 0.0 ? ui::colors::POSITIVE() : ui::colors::NEGATIVE();
        for (int c = 0; c < items.size(); ++c) {
            items[c]->setTextAlignment(c == 0 ? Qt::AlignLeft | Qt::AlignVCenter
                                              : Qt::AlignRight | Qt::AlignVCenter);
            if (c == 1 || c == 2)
                items[c]->setForeground(col);
            windows_table_->setItem(i, c, items[c]);
        }
    }
}

void CryptoMicrostructureWidget::render_sources(
    const services::edge_radar::CryptoMicrostructureSnapshot& snap) {
    sources_table_->setRowCount(snap.sources.size());
    for (int i = 0; i < snap.sources.size(); ++i) {
        const auto& s = snap.sources[i];
        QList<QTableWidgetItem*> items = {
            new QTableWidgetItem(s.source),
            new QTableWidgetItem(fmt_price(s.best_bid)),
            new QTableWidgetItem(fmt_price(s.best_ask)),
            new QTableWidgetItem(fmt_age(s.age_ms)),
        };
        const QColor col = s.status == QStringLiteral("live") ? ui::colors::POSITIVE()
                                                              : ui::colors::TEXT_TERTIARY();
        for (int c = 0; c < items.size(); ++c) {
            items[c]->setTextAlignment(c == 0 ? Qt::AlignLeft | Qt::AlignVCenter
                                              : Qt::AlignRight | Qt::AlignVCenter);
            if (c == 0 || c == 3)
                items[c]->setForeground(col);
            sources_table_->setItem(i, c, items[c]);
        }
    }
}

QDialog* CryptoMicrostructureWidget::make_config_dialog(QWidget* parent) {
    auto* dlg = new QDialog(parent);
    dlg->setWindowTitle(tr("Configure - Microstructure"));
    auto* form = new QFormLayout(dlg);

    auto* symbol = new QLineEdit(dlg);
    symbol->setText(symbol_);
    symbol->setPlaceholderText(tr("BTC-USD"));
    form->addRow(tr("Symbol"), symbol);

    auto* sources = new QLineEdit(dlg);
    sources->setText(sources_.join(", "));
    sources->setPlaceholderText(tr("coinbase, kraken, bitcointicker"));
    form->addRow(tr("Sources"), sources);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
    form->addRow(buttons);

    connect(buttons, &QDialogButtonBox::accepted, dlg, [this, dlg, symbol, sources]() {
        QJsonObject cfg;
        cfg.insert("symbol", symbol->text().trimmed());
        QJsonArray arr;
        for (const auto& s : sources->text().split(',', Qt::SkipEmptyParts))
            arr.append(s.trimmed().toLower());
        cfg.insert("sources", arr);
        apply_config(cfg);
        emit config_changed(cfg);
        dlg->accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    return dlg;
}

void CryptoMicrostructureWidget::on_theme_changed() {
    apply_styles();
}

void CryptoMicrostructureWidget::apply_styles() {
    const QString text = ui::colors::TEXT_PRIMARY();
    const QString muted = ui::colors::TEXT_TERTIARY();
    const QString border = ui::colors::BORDER_DIM();
    const QString raised = ui::colors::BG_RAISED();
    detail_label_->setStyleSheet(QString("color:%1;font-size:10px;background:transparent;").arg(muted));
    price_label_->setStyleSheet(QString("color:%1;font-size:13px;font-weight:800;background:transparent;").arg(text));
    book_label_->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;").arg(text));
    pressure_label_->setStyleSheet(QString("color:%1;font-size:10px;font-weight:700;background:transparent;").arg(muted));
    rationale_label_->setStyleSheet(QString("color:%1;font-size:9px;background:transparent;").arg(muted));

    const QString table_css =
        QString("QTableWidget{background:transparent;color:%1;gridline-color:%2;font-size:9px;border:none;}"
                "QHeaderView::section{background:%3;color:%4;border:none;border-bottom:1px solid %2;"
                "padding:2px 4px;font-size:8px;font-weight:bold;}"
                "QTableWidget::item{padding:1px 4px;}")
            .arg(text, border, raised, muted);
    windows_table_->setStyleSheet(table_css);
    sources_table_->setStyleSheet(table_css);
}

void CryptoMicrostructureWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(symbol_.startsWith("BTC", Qt::CaseInsensitive) ? tr("BTC MICROSTRUCTURE")
                                                             : symbol_ + tr(" MICROSTRUCTURE"));
    windows_table_->setHorizontalHeaderLabels({tr("Window"), tr("Move"), tr("Pressure"), tr("Ticks")});
    sources_table_->setHorizontalHeaderLabels({tr("Source"), tr("Bid"), tr("Ask"), tr("Age")});
    render();
}

QString CryptoMicrostructureWidget::fmt_price(double value) {
    if (value <= 0.0)
        return QStringLiteral("-");
    return QString::number(value, 'f', value < 10.0 ? 4 : 2);
}

QString CryptoMicrostructureWidget::fmt_age(qint64 age_ms) {
    if (age_ms < 0)
        return QStringLiteral("-");
    if (age_ms < 1000)
        return QString("%1ms").arg(age_ms);
    return QString("%1s").arg(age_ms / 1000);
}

QStringList CryptoMicrostructureWidget::normalize_sources(const QJsonArray& arr) {
    QStringList sources;
    for (const auto& v : arr)
        sources.append(v.toString());
    return clean_sources(sources);
}

} // namespace openmarketterminal::screens::widgets
