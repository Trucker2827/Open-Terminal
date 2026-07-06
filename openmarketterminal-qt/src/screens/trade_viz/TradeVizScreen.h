#pragma once
#include "screens/common/IStatefulScreen.h"

#include <QComboBox>
#include <QEvent>
#include <QHideEvent>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QPointer>
#include <QPushButton>
#include <QShowEvent>
#include <QSplitter>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <limits>

namespace openmarketterminal::screens {

struct TradeVizPartner {
    QString name;
    QString iso3;
    double imports_m = 0.0;
    double exports_m = 0.0;
    double total_m = 0.0;
    double balance_m = 0.0;
    double yoy_pct = std::numeric_limits<double>::quiet_NaN();
};

/// Trade Flow visualization.
/// Chord diagram showing bilateral trade flows + partner ranking table.
class TradeVizScreen : public QWidget, public IStatefulScreen {
    Q_OBJECT
  public:
    explicit TradeVizScreen(QWidget* parent = nullptr);

    // IStatefulScreen — persists the filter combo selections
    // (country/order/period/year).
    QVariantMap save_state() const override;
    void restore_state(const QVariantMap& state) override;
    QString state_key() const override { return "trade_viz"; }

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void setup_ui();
    void retranslateUi();

    // ── Sub-builders ─────────────────────────────────────────────────────────
    QWidget* build_tab_bar();
    QWidget* build_filter_bar();
    QWidget* build_partner_table();
    QWidget* build_intelligence_panel();

    void populate_partner_table();
    void load_trade_data();
    void apply_payload(const QJsonObject& payload);
    void apply_fallback_data(const QString& reason);
    void update_intelligence_panel(const QJsonObject& payload = {});
    void update_sources_panel();
    void update_clock();
    void select_partner(const QString& iso3, bool sync_table = true);
    void set_active_tab(int index);
    void update_tab_styles();
    QString current_reporter() const;
    QString current_commodity() const;
    int current_year() const;

    // ── Widgets ──────────────────────────────────────────────────────────────
    QTableWidget* partner_table_ = nullptr;
    QLabel* clock_label_ = nullptr;
    QComboBox* country_combo_ = nullptr;
    QComboBox* order_combo_ = nullptr;
    QComboBox* period_combo_ = nullptr;
    QComboBox* year_combo_ = nullptr;
    QComboBox* commodity_combo_ = nullptr;
    QPushButton* refresh_btn_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* summary_label_ = nullptr;
    QTextEdit* insights_text_ = nullptr;
    QSplitter* main_splitter_ = nullptr;
    QSplitter* right_splitter_ = nullptr;
    QWidget* partner_panel_ = nullptr;
    QWidget* intelligence_panel_ = nullptr;

    // Text-bearing widgets cached for retranslateUi.
    QList<QPushButton*> tab_buttons_;
    QLabel* flow_title_ = nullptr;
    QLabel* provenance_note_ = nullptr;  // honest "static sample" disclaimer
    QLabel* browse_label_ = nullptr;
    QLabel* order_caption_ = nullptr;
    QLabel* period_caption_ = nullptr;
    QPointer<QWidget> chord_widget_; // chord diagram (painter text re-rendered on update())
    QVector<TradeVizPartner> partners_;
    QString selected_partner_iso_;
    QString source_status_ = "STATIC";
    QString source_name_ = "bundled sample";
    int active_tab_ = 0;

    // ── Timers ───────────────────────────────────────────────────────────────
    QTimer* clock_timer_ = nullptr;
    QTimer* animation_timer_ = nullptr;
    double animation_phase_ = 0.0;
};

} // namespace openmarketterminal::screens
