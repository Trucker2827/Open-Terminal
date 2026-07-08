// CryptoLadder.cpp — custom-painted DOM ladder (read-only, no order entry)
#include "screens/crypto_trading/CryptoLadder.h"

#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QHBoxLayout>
#include <QPainter>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace openmarketterminal::screens::crypto {

namespace {
// Live color accessors — reflect active theme at call time.
inline QColor kBgBase() { return QColor(ui::colors::BG_BASE); }
inline QColor kBorderDim() { return QColor(ui::colors::BORDER_DIM); }
inline QColor kTextSecondary() { return QColor(ui::colors::TEXT_SECONDARY); }
inline QColor kTextTertiary() { return QColor(ui::colors::TEXT_TERTIARY); }
inline QColor kTextDim() { return QColor(ui::colors::TEXT_DIM); }
inline QColor kBid() { return QColor(ui::colors::POSITIVE); }
inline QColor kAsk() { return QColor(ui::colors::NEGATIVE); }
inline QColor kRowEven() { return QColor(ui::colors::BG_BASE); }
inline QColor kRowOdd() { return QColor(ui::colors::ROW_ALT); }
inline QColor kBidBar() {
    auto c = kBid();
    c.setAlpha(40);
    return c;
}
inline QColor kAskBar() {
    auto c = kAsk();
    c.setAlpha(40);
    return c;
}

// Grouping increments offered in the dropdown — all multiples of the
// model's 0.1 fine-VAP base.
constexpr double kGroupingSteps[] = {0.1, 1.0, 5.0, 10.0, 25.0, 100.0};

QString grouping_label(double g) {
    if (std::abs(g - std::round(g)) < 1e-9)
        return QString::number(static_cast<qint64>(std::round(g)));
    return QString::number(g, 'f', 1);
}
} // namespace

CryptoLadder::CryptoLadder(QWidget* parent) : QWidget(parent) {
    setObjectName("cryptoLadder");
    connect(&ui::ThemeManager::instance(), &ui::ThemeManager::theme_changed, this,
            [this](const ui::ThemeTokens&) { update(); });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* header = new QWidget(this);
    header->setObjectName("cryptoLadderHeader");
    header->setFixedHeight(HEADER_H);
    auto* h_layout = new QHBoxLayout(header);
    h_layout->setContentsMargins(8, 0, 8, 0);
    h_layout->setSpacing(6);

    title_label_ = new QLabel(tr("DOM LADDER"));
    title_label_->setObjectName("cryptoLadderTitle");
    h_layout->addWidget(title_label_);
    h_layout->addStretch();

    auto* grouping_lbl = new QLabel(tr("Group"));
    grouping_lbl->setObjectName("cryptoLadderGroupLabel");
    h_layout->addWidget(grouping_lbl);

    grouping_cb_ = new QComboBox(header);
    grouping_cb_->setObjectName("cryptoLadderGroupCombo");
    grouping_cb_->setFixedHeight(22);
    populate_grouping_combo();
    connect(grouping_cb_, &QComboBox::currentIndexChanged, this, [this](int idx) { on_grouping_changed(idx); });
    h_layout->addWidget(grouping_cb_);

    layout->addWidget(header);
    layout->addStretch(1); // rest is custom-painted

    setMinimumHeight(200);
    setMinimumWidth(260);

    repaint_timer_ = new QTimer(this);
    repaint_timer_->setSingleShot(true);
    repaint_timer_->setInterval(50);
    connect(repaint_timer_, &QTimer::timeout, this, [this]() { update(); });
}

void CryptoLadder::populate_grouping_combo() {
    grouping_cb_->blockSignals(true);
    grouping_cb_->clear();
    for (double g : kGroupingSteps)
        grouping_cb_->addItem(grouping_label(g), g);
    const int idx = grouping_cb_->findData(grouping_);
    grouping_cb_->setCurrentIndex(idx >= 0 ? idx : 0);
    grouping_cb_->blockSignals(false);
}

void CryptoLadder::apply_default_grouping(double mid) {
    double g = 0.1;
    if (mid >= 10000.0)
        g = 10.0;
    else if (mid >= 1000.0)
        g = 1.0;
    else if (mid >= 100.0)
        g = 0.1;
    else
        g = 0.1;
    grouping_ = g;
    if (grouping_cb_) {
        grouping_cb_->blockSignals(true);
        const int idx = grouping_cb_->findData(grouping_);
        grouping_cb_->setCurrentIndex(idx >= 0 ? idx : 0);
        grouping_cb_->blockSignals(false);
    }
}

void CryptoLadder::on_grouping_changed(int idx) {
    if (idx < 0 || !grouping_cb_)
        return;
    grouping_ = grouping_cb_->itemData(idx).toDouble();
    grouping_auto_ = false; // user took control — stop auto-tiering on new book data
    schedule_repaint();
}

void CryptoLadder::schedule_repaint() {
    if (repaint_timer_ && !repaint_timer_->isActive())
        repaint_timer_->start();
    else if (!repaint_timer_)
        update();
}

void CryptoLadder::set_book(const QVector<QPair<double, double>>& bids, const QVector<QPair<double, double>>& asks) {
    bids_ = bids;
    asks_ = asks;

    if (grouping_auto_) {
        double best_bid = 0, best_ask = 0;
        for (const auto& b : bids_) best_bid = std::max(best_bid, b.first);
        for (const auto& a : asks_) best_ask = (best_ask == 0) ? a.first : std::min(best_ask, a.first);
        if (best_bid > 0 && best_ask > 0)
            apply_default_grouping((best_bid + best_ask) / 2.0);
    }
    schedule_repaint();
}

void CryptoLadder::add_trade(double price, double amount) {
    model_.accumulate_vap(price, amount);
    schedule_repaint();
}

void CryptoLadder::set_symbol(const QString& symbol, const QString& exchange) {
    if (symbol == symbol_ && exchange == exchange_)
        return;
    symbol_ = symbol;
    exchange_ = exchange;
    model_.reset_vap();
    bids_.clear();
    asks_.clear();
    grouping_auto_ = true; // let the new symbol re-derive a sensible default tier
    schedule_repaint();
}

void CryptoLadder::set_my_orders(const QVector<MyOrder>& orders) {
    my_orders_ = orders;
    schedule_repaint();
}

void CryptoLadder::set_avg_entry(double price) {
    avg_entry_ = price;
    schedule_repaint();
}

void CryptoLadder::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    update();
}

void CryptoLadder::hideEvent(QHideEvent* e) {
    QWidget::hideEvent(e);
    if (repaint_timer_)
        repaint_timer_->stop();
}

void CryptoLadder::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
        update();
    }
    QWidget::changeEvent(event);
}

void CryptoLadder::retranslateUi() {
    if (title_label_)
        title_label_->setText(tr("DOM LADDER"));
}

void CryptoLadder::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.fillRect(rect(), kBgBase());
    p.setFont(QFont("Consolas", 9));

    const int y_top = HEADER_H;
    const int w = width();
    const int h = height() - y_top;
    if (w <= 0 || h <= 0)
        return;

    // Side-by-side book: best bids (green) on the left, best asks (red) on the
    // right, each ranked best-first from the top so the tightest quotes sit at
    // the top of the panel. Raw levels are aggregated into the selected grouping.
    const int avail_h = h - COL_HEADER_H;
    const int rows = std::max(1, avail_h / ROW_H);
    const auto bid_levels = model_.book_side(bids_, grouping_, rows, /*is_bid=*/true);
    const auto ask_levels = model_.book_side(asks_, grouping_, rows, /*is_bid=*/false);

    // Columns: [bid size][bid price] | [ask price][ask size]. Prices meet at the
    // center divider; sizes sit on the outer edges; depth bars grow outward.
    const int half = w / 2;
    const int bid_size_w = half / 2;
    const int bid_price_w = half - bid_size_w;
    const int ask_price_w = (w - half) / 2;
    const int ask_size_w = (w - half) - ask_price_w;
    const int x_bid_size = 0;
    const int x_bid_price = x_bid_size + bid_size_w;
    const int x_ask_price = half;
    const int x_ask_size = x_ask_price + ask_price_w;

    // Column header row: SIZE | BID  ||  ASK | SIZE
    p.setPen(kTextDim());
    p.drawText(QRect(x_bid_size + 4, y_top, bid_size_w - 4, COL_HEADER_H), Qt::AlignLeft | Qt::AlignVCenter,
               tr("SIZE"));
    p.drawText(QRect(x_bid_price, y_top, bid_price_w - 6, COL_HEADER_H), Qt::AlignRight | Qt::AlignVCenter,
               tr("BID"));
    p.drawText(QRect(x_ask_price + 6, y_top, ask_price_w - 6, COL_HEADER_H), Qt::AlignLeft | Qt::AlignVCenter,
               tr("ASK"));
    p.drawText(QRect(x_ask_size, y_top, ask_size_w - 4, COL_HEADER_H), Qt::AlignRight | Qt::AlignVCenter,
               tr("SIZE"));
    p.setPen(kBorderDim());
    p.drawLine(0, y_top + COL_HEADER_H, w, y_top + COL_HEADER_H);

    const int rows_y = y_top + COL_HEADER_H;

    if (bid_levels.isEmpty() && ask_levels.isEmpty()) {
        p.setPen(kTextTertiary());
        p.drawText(QRect(0, rows_y, w, h - COL_HEADER_H), Qt::AlignCenter, tr("Waiting for book data..."));
        return;
    }

    // Depth bars scale to the largest single-level size across both sides.
    double max_size = 1.0;
    for (const auto& l : bid_levels) max_size = std::max(max_size, l.size);
    for (const auto& l : ask_levels) max_size = std::max(max_size, l.size);

    const int n_rows = std::max(bid_levels.size(), ask_levels.size());
    for (int i = 0; i < n_rows; ++i) {
        const int y = rows_y + i * ROW_H;
        if (y + ROW_H > height())
            break;
        p.fillRect(0, y, w, ROW_H, (i % 2 == 0) ? kRowEven() : kRowOdd());

        // Bid side (left half): depth bar grows leftward from the center divider.
        if (i < bid_levels.size()) {
            const auto& b = bid_levels[i];
            const int bar_w = static_cast<int>(half * std::min(1.0, b.size / max_size));
            p.fillRect(half - bar_w, y, bar_w, ROW_H, kBidBar());
            p.setPen(kTextSecondary());
            p.drawText(QRect(x_bid_size + 4, y, bid_size_w - 6, ROW_H), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(b.size, 'f', 4));
            p.setPen(kBid());
            p.drawText(QRect(x_bid_price, y, bid_price_w - 6, ROW_H), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(b.price, 'f', 2));
        }

        // Ask side (right half): depth bar grows rightward from the center divider.
        if (i < ask_levels.size()) {
            const auto& a = ask_levels[i];
            const int bar_w = static_cast<int>((w - half) * std::min(1.0, a.size / max_size));
            p.fillRect(half, y, bar_w, ROW_H, kAskBar());
            p.setPen(kAsk());
            p.drawText(QRect(x_ask_price + 6, y, ask_price_w - 6, ROW_H), Qt::AlignLeft | Qt::AlignVCenter,
                       QString::number(a.price, 'f', 2));
            p.setPen(kTextSecondary());
            p.drawText(QRect(x_ask_size, y, ask_size_w - 4, ROW_H), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(a.size, 'f', 4));
        }
    }

    // Center divider between bid and ask sides.
    p.setPen(kBorderDim());
    p.drawLine(half, y_top, half, height());
}

} // namespace openmarketterminal::screens::crypto
