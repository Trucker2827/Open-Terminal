// src/screens/geopolitics/RelationshipPanel.h
#pragma once
#include "services/geopolitics/GeopoliticsTypes.h"

#include <QEvent>
#include <QJsonObject>
#include <QLabel>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

class QScrollArea;

namespace openmarketterminal::screens {

/// Network relationship visualization of conflicts, crises, and organizations.
class RelationshipPanel : public QWidget {
    Q_OBJECT
  public:
    explicit RelationshipPanel(QWidget* parent = nullptr);

  public slots:
    /// Populate the network from a GDELT event-network payload (see
    /// GeopoliticsService::event_network_loaded): {edges:[...], actors, ...}.
    void set_event_network(const QJsonObject& data);

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void build_ui();
    void render_network();  // rebuild scroll_ contents from nodes_
    QWidget* build_node_card(const openmarketterminal::services::geo::RelationshipNode& node, QWidget* parent);
    void retranslateUi();

    QScrollArea* scroll_ = nullptr;
    QVector<openmarketterminal::services::geo::RelationshipNode> nodes_;  // live actor→actor edges

    // Static text widgets (cached for retranslateUi)
    QLabel* title_lbl_ = nullptr;
    QLabel* stats_lbl_ = nullptr;
    QLabel* provenance_lbl_ = nullptr;
    int node_count_ = 0;      // distinct actors
    int conflict_count_ = 0;  // conflict edges
    int org_count_ = 0;       // total relationships
};

} // namespace openmarketterminal::screens
