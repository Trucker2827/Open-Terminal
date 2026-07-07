#pragma once
// Crypto Order Entry — BUY/SELL tabs with toggle type buttons

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QWidget>

namespace openmarketterminal::screens::crypto {

class CryptoOrderEntry : public QWidget {
    Q_OBJECT
  public:
    explicit CryptoOrderEntry(QWidget* parent = nullptr);

    // currency: the actual account balance currency (e.g. "USD"); when empty,
    // money figures fall back to the display pair's quote symbol.
    void set_balance(double balance, const QString& currency = {});
    void set_current_price(double price);
    void set_mode(bool is_paper);
    void set_symbol(const QString& symbol);
    void set_exchange_id(const QString& exchange_id);
    void set_orderbook_quote(double bid, double ask);
    void set_futures_mode(bool is_futures); // show/hide leverage + margin controls
    bool reduce_only() const;               // reduce-only flag (perp/futures only)

    // In-flight guard for live order submission. While an order POST is in
    // flight the submit button is disabled and relabelled "SENDING…" so a
    // double-click / held Ctrl+Enter can't fire a second real exchange order.
    // Re-enabling on the main thread when the async call completes restores the
    // normal BUY/SELL label.
    void set_submit_busy(bool busy);
    bool submit_busy() const { return submit_busy_; }
    void show_order_result(bool ok, const QString& message);

  signals:
    void order_submitted(const QString& side, const QString& order_type, double quantity, double price,
                         double stop_price, double sl_price, double tp_price, bool post_only);
    void leverage_changed(int leverage);
    void margin_mode_changed(const QString& mode); // "cross" or "isolated"

  protected:
    void changeEvent(QEvent* event) override;

  private slots:
    void on_submit();
    void on_maker_submit();
    void on_pct_clicked(int pct);

  private:
    void update_cost_preview();
    void update_maker_preview();
    QString quote_label() const;
    void set_buy_side(bool is_buy);
    void set_order_type(int idx);
    void retranslateUi();

    // Title
    QLabel* title_label_ = nullptr;

    // Side tabs
    QPushButton* buy_tab_ = nullptr;
    QPushButton* sell_tab_ = nullptr;

    // Type buttons
    QPushButton* type_btns_[4] = {};
    int active_type_ = 0; // 0=Market 1=Limit 2=Stop 3=StopLimit

    // Form
    QLineEdit* qty_edit_ = nullptr;
    QLineEdit* price_edit_ = nullptr;
    QLineEdit* stop_price_edit_ = nullptr;
    QLineEdit* sl_edit_ = nullptr;
    QLineEdit* tp_edit_ = nullptr;
    QCheckBox* post_only_check_ = nullptr;
    QPushButton* submit_btn_ = nullptr;
    QWidget* advanced_section_ = nullptr;
    QPushButton* advanced_toggle_ = nullptr;

    // Container rows so price/stop can be shown/hidden as a unit when the
    // selected order type doesn't need them.
    QWidget* price_row_ = nullptr;
    QWidget* stop_row_ = nullptr;

    // Header / account card labels
    QLabel* balance_label_ = nullptr;
    QLabel* market_price_label_ = nullptr;
    QLabel* avail_label_ = nullptr;
    QLabel* mode_label_ = nullptr;

    // Idiot-proof maker-only quick ticket.
    QLabel* maker_title_ = nullptr;
    QLabel* maker_help_label_ = nullptr;
    QLabel* maker_usd_title_ = nullptr;
    QLabel* maker_limit_title_ = nullptr;
    QLineEdit* maker_usd_edit_ = nullptr;
    QLineEdit* maker_limit_edit_ = nullptr;
    QPushButton* maker_use_bid_btn_ = nullptr;
    QPushButton* maker_submit_btn_ = nullptr;
    QLabel* maker_preview_label_ = nullptr;
    QLabel* maker_status_label_ = nullptr;

    // Static field/section titles (cached for retranslateUi)
    QLabel* balance_title_ = nullptr;
    QLabel* mark_title_ = nullptr;
    QLabel* avail_title_ = nullptr;
    QLabel* qty_title_ = nullptr;
    QLabel* price_title_ = nullptr;
    QLabel* stop_title_ = nullptr;
    QLabel* sl_title_ = nullptr;
    QLabel* tp_title_ = nullptr;
    QLabel* leverage_title_ = nullptr;
    QLabel* margin_title_ = nullptr;

    // Order breakdown
    QLabel* cost_label_ = nullptr;
    QLabel* fee_label_ = nullptr;
    QLabel* total_label_ = nullptr;
    QLabel* recv_label_ = nullptr;
    QLabel* pct_used_label_ = nullptr;

    // Order breakdown titles (cached for retranslateUi)
    QLabel* cost_title_ = nullptr;
    QLabel* fee_title_ = nullptr;
    QLabel* total_title_ = nullptr;
    QLabel* recv_title_ = nullptr;
    QLabel* pct_used_title_ = nullptr;

    // Submit subtitle (rendered as a second line under the main button label)
    QLabel* submit_subtitle_ = nullptr;
    QLabel* status_label_ = nullptr;

    // Futures controls
    QWidget* futures_section_ = nullptr;
    QSpinBox* leverage_spin_ = nullptr;
    QComboBox* margin_mode_combo_ = nullptr;
    QCheckBox* reduce_only_check_ = nullptr;

    // State
    double balance_ = 0;
    QString balance_currency_; // actual account balance currency (e.g. "USD"); empty → use pair quote
    double current_price_ = 0;
    double best_bid_ = 0;
    double best_ask_ = 0;
    bool is_paper_ = true;
    bool is_buy_side_ = true;
    bool is_futures_ = false;
    bool submit_busy_ = false; // true while a live order POST is in flight
    QString current_symbol_ = "BTC/USDT";
    QString exchange_id_ = "coinbase";
};

} // namespace openmarketterminal::screens::crypto
