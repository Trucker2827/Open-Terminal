#pragma once

#include "screens/common/IStatefulScreen.h"

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTextEdit;

namespace openmarketterminal {
struct EdgeRadarIdea;
}

namespace openmarketterminal::screens {

class EdgeRadarScreen : public QWidget, public IStatefulScreen {
    Q_OBJECT
  public:
    explicit EdgeRadarScreen(QWidget* parent = nullptr);

    void restore_state(const QVariantMap& state) override;
    QVariantMap save_state() const override;
    QString state_key() const override { return QStringLiteral("edge_radar"); }
    int state_version() const override { return 1; }

  private slots:
    void evaluate_current();
    void save_idea();
    void update_selected();
    void close_selected();
    void refresh_table();
    void on_row_selected(int row, int column);

  private:
    void build_ui();
    EdgeRadarIdea collect_idea() const;
    void load_idea(const EdgeRadarIdea& idea);
    void set_score_labels(const EdgeRadarIdea& idea);
    void set_status(const QString& text, bool error = false);

    QComboBox* asset_class_ = nullptr;
    QComboBox* venue_ = nullptr;
    QLineEdit* symbol_ = nullptr;
    QLineEdit* market_id_ = nullptr;
    QLineEdit* question_ = nullptr;
    QDoubleSpinBox* market_prob_ = nullptr;
    QDoubleSpinBox* model_prob_ = nullptr;
    QDoubleSpinBox* spread_cost_ = nullptr;
    QDoubleSpinBox* fee_cost_ = nullptr;
    QDoubleSpinBox* liquidity_ = nullptr;
    QDoubleSpinBox* confidence_ = nullptr;
    QTextEdit* thesis_ = nullptr;
    QTextEdit* risks_ = nullptr;
    QLabel* side_label_ = nullptr;
    QLabel* raw_edge_label_ = nullptr;
    QLabel* after_cost_label_ = nullptr;
    QLabel* recommendation_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QTableWidget* table_ = nullptr;
    QString selected_id_;
};

} // namespace openmarketterminal::screens
