#pragma once
// Crypto DOM Ladder — custom-painted price ladder (read-only, no order entry).
//
// Renders CryptoLadderModel::LadderView as a single price-ordered column with
// ORDERS | BID | PRICE | ASK | VAP sub-columns. Mirrors CryptoOrderBook's
// structure: custom paintEvent, timer-throttled update(), theme-change repaint.

#include "screens/crypto_trading/CryptoLadderModel.h"

#include <QComboBox>
#include <QEvent>
#include <QLabel>
#include <QPair>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

namespace openmarketterminal::screens::crypto {

// Re-exported from the model's own namespace (openmarketterminal::crypto) so
// callers of this widget (and the CryptoTradingScreen wiring that owns it)
// can write crypto::MyOrder without reaching into CryptoLadderModel.h.
using MyOrder = ::openmarketterminal::crypto::MyOrder;

class CryptoLadder : public QWidget {
    Q_OBJECT
  public:
    explicit CryptoLadder(QWidget* parent = nullptr);

    // Depth update — replaces the last known book snapshot.
    void set_book(const QVector<QPair<double, double>>& bids, const QVector<QPair<double, double>>& asks);

    // One executed trade -> folded into the session volume-at-price accumulator.
    void add_trade(double price, double amount);

    // Resets VAP + clears the book when symbol or exchange actually changes.
    void set_symbol(const QString& symbol, const QString& exchange);

    void set_my_orders(const QVector<MyOrder>& orders);
    void set_avg_entry(double price);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;
    // Intentionally NO mousePressEvent: this widget is read-only display —
    // no click-to-place, click-to-cancel, or any order-mutation interaction.

  private:
    void retranslateUi();
    void populate_grouping_combo();
    void apply_default_grouping(double mid);
    void on_grouping_changed(int idx);
    void schedule_repaint();

    ::openmarketterminal::crypto::CryptoLadderModel model_;

    QVector<QPair<double, double>> bids_;
    QVector<QPair<double, double>> asks_;
    double grouping_ = 0.1;
    bool grouping_auto_ = true; // true until the user manually picks a grouping
    int rows_each_side_ = 12;
    QVector<MyOrder> my_orders_;
    double avg_entry_ = 0;

    QLabel* title_label_ = nullptr;
    QComboBox* grouping_cb_ = nullptr;
    bool attached_ = true; // recenter affordance state (Phase 1: always centered)

    QString symbol_;
    QString exchange_;

    QTimer* repaint_timer_ = nullptr; // 50ms coalesce — max 20fps

    static constexpr int ROW_H = 20;
    static constexpr int HEADER_H = 28;
    static constexpr int COL_HEADER_H = 18;
};

} // namespace openmarketterminal::screens::crypto
