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
inline QColor kAccent() { return QColor(ui::colors::AMBER); }
inline QColor kCyan() { return QColor(ui::colors::CYAN); }
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
inline QColor kVapBar() {
    auto c = kCyan();
    c.setAlpha(55);
    return c;
}
inline QColor kAvgEntryBg() {
    auto c = kAccent();
    c.setAlpha(45);
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

    // Size the row window to the visible panel height and CENTER it on mid, so
    // the mid + bid/ask depth are always on screen. A fixed row count would let
    // a short panel show only the topmost rows (all far above mid → all asks,
    // no depth). rows_each_side each way + the mid row must fit in `avail_h`.
    const int avail_h = h - COL_HEADER_H;
    const int visible_rows = std::max(1, avail_h / ROW_H);
    const int each_side = std::max(1, (visible_rows - 1) / 2);
    const auto v = model_.build(bids_, asks_, grouping_, each_side, my_orders_, avg_entry_);

    // Column widths: ORDERS | BID | PRICE | ASK | VAP
    const int orders_w = static_cast<int>(w * 0.16);
    const int bid_w = static_cast<int>(w * 0.24);
    const int price_w = static_cast<int>(w * 0.20);
    const int ask_w = static_cast<int>(w * 0.24);
    const int vap_w = w - orders_w - bid_w - price_w - ask_w;
    const int x_orders = 0;
    const int x_bid = x_orders + orders_w;
    const int x_price = x_bid + bid_w;
    const int x_ask = x_price + price_w;
    const int x_vap = x_ask + ask_w;

    // Column header row
    p.setPen(kTextDim());
    p.drawText(QRect(x_orders + 4, y_top, orders_w - 4, COL_HEADER_H), Qt::AlignLeft | Qt::AlignVCenter,
               tr("ORDERS"));
    p.drawText(QRect(x_bid, y_top, bid_w - 4, COL_HEADER_H), Qt::AlignRight | Qt::AlignVCenter, tr("BID"));
    p.drawText(QRect(x_price, y_top, price_w, COL_HEADER_H), Qt::AlignCenter, tr("PRICE"));
    p.drawText(QRect(x_ask + 4, y_top, ask_w - 4, COL_HEADER_H), Qt::AlignLeft | Qt::AlignVCenter, tr("ASK"));
    p.drawText(QRect(x_vap, y_top, vap_w - 4, COL_HEADER_H), Qt::AlignRight | Qt::AlignVCenter, tr("VAP"));
    p.setPen(kBorderDim());
    p.drawLine(0, y_top + COL_HEADER_H, w, y_top + COL_HEADER_H);

    const int rows_y = y_top + COL_HEADER_H;

    if (v.rows.isEmpty()) {
        p.setPen(kTextTertiary());
        p.drawText(QRect(0, rows_y, w, h - COL_HEADER_H), Qt::AlignCenter, tr("Waiting for book data..."));
        return;
    }

    const double max_depth = v.max_depth > 0 ? v.max_depth : 1.0;
    const double max_vap = v.max_vap > 0 ? v.max_vap : 1.0;

    for (int i = 0; i < v.rows.size(); ++i) {
        const int y = rows_y + i * ROW_H;
        if (y + ROW_H > height())
            break;
        const auto& row = v.rows[i];
        const bool is_ask_side = row.price > v.mid;

        const QColor row_bg = row.is_avg_entry ? kAvgEntryBg() : ((i % 2 == 0) ? kRowEven() : kRowOdd());
        p.fillRect(0, y, w, ROW_H, row_bg);

        // Depth bars, scaled to max_depth: bid grows leftward from the
        // BID/PRICE boundary, ask grows rightward from the PRICE/ASK boundary.
        if (row.bid_size > 0) {
            const int bar_w = static_cast<int>(bid_w * std::min(1.0, row.bid_size / max_depth));
            p.fillRect(x_bid + bid_w - bar_w, y, bar_w, ROW_H, kBidBar());
        }
        if (row.ask_size > 0) {
            const int bar_w = static_cast<int>(ask_w * std::min(1.0, row.ask_size / max_depth));
            p.fillRect(x_ask, y, bar_w, ROW_H, kAskBar());
        }

        // VAP bar, right-aligned, scaled to max_vap.
        if (row.vap > 0) {
            const int bar_w = static_cast<int>(vap_w * std::min(1.0, row.vap / max_vap));
            p.fillRect(x_vap + vap_w - bar_w, y, bar_w, ROW_H, kVapBar());
        }

        // ORDERS column — resting my_bid_qty / my_ask_qty.
        if (row.my_bid_qty > 0 || row.my_ask_qty > 0) {
            QString txt;
            if (row.my_bid_qty > 0)
                txt += QString("B %1").arg(row.my_bid_qty, 0, 'f', 3);
            if (row.my_ask_qty > 0) {
                if (!txt.isEmpty())
                    txt += " ";
                txt += QString("S %1").arg(row.my_ask_qty, 0, 'f', 3);
            }
            p.setPen(row.my_bid_qty > 0 ? kBid() : kAsk());
            p.drawText(QRect(x_orders + 4, y, orders_w - 4, ROW_H), Qt::AlignLeft | Qt::AlignVCenter, txt);
        }

        // BID size text.
        if (row.bid_size > 0) {
            p.setPen(kBid());
            p.drawText(QRect(x_bid, y, bid_w - 6, ROW_H), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(row.bid_size, 'f', 4));
        }

        // PRICE — bid-side green, ask-side red.
        p.setPen(row.is_avg_entry ? kAccent() : (is_ask_side ? kAsk() : kBid()));
        p.drawText(QRect(x_price, y, price_w, ROW_H), Qt::AlignCenter, QString::number(row.price, 'f', 2));

        // ASK size text.
        if (row.ask_size > 0) {
            p.setPen(kAsk());
            p.drawText(QRect(x_ask + 6, y, ask_w - 6, ROW_H), Qt::AlignLeft | Qt::AlignVCenter,
                       QString::number(row.ask_size, 'f', 4));
        }

        // VAP value text (drawn over the bar, right-aligned).
        if (row.vap > 0) {
            p.setPen(kTextSecondary());
            p.drawText(QRect(x_vap, y, vap_w - 4, ROW_H), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(row.vap, 'f', 2));
        }

        if (row.is_avg_entry) {
            p.setPen(kAccent());
            p.drawLine(0, y, w, y);
            p.drawLine(0, y + ROW_H, w, y + ROW_H);
        }
    }

    // Column separators.
    p.setPen(kBorderDim());
    p.drawLine(x_bid, y_top, x_bid, height());
    p.drawLine(x_price, y_top, x_price, height());
    p.drawLine(x_ask, y_top, x_ask, height());
    p.drawLine(x_vap, y_top, x_vap, height());

    // Recenter affordance — shown only when the ladder has scrolled away
    // from the live mid price. Phase 1 keeps attached_ always true (no
    // scroll/detach interaction is wired up), so this is dormant for now
    // but the paint path exists for the follow-up task that adds it.
    if (!attached_) {
        const QString label = tr("▼ RECENTER");
        QFontMetrics fm(p.font());
        const int pad = 8;
        const int badge_w = fm.horizontalAdvance(label) + pad * 2;
        const int badge_h = 20;
        const QRect badge(w - badge_w - 8, y_top + 4, badge_w, badge_h);
        p.setBrush(kAccent());
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(badge, 4, 4);
        p.setPen(Qt::black);
        p.drawText(badge, Qt::AlignCenter, label);
    }
}

} // namespace openmarketterminal::screens::crypto
