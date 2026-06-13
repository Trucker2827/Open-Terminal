#pragma once
// OpenMarketTerminal — provenance badge.
//
// A small reusable label that makes a connector's data transparent:
//   ● Yahoo Finance · no key required · 15:42:01 · LIVE
// Colour encodes freshness (green = live, amber = stale, grey = cached).
// Screens embed one of these next to any connector-sourced data.

#include "services/connectors/Provenance.h"

#include <QWidget>

class QLabel;

namespace openmarketterminal::ui {

class ProvenanceBadge : public QWidget {
    Q_OBJECT
  public:
    explicit ProvenanceBadge(QWidget* parent = nullptr);

    /// Render the given provenance. Safe to call repeatedly.
    void set_provenance(const openmarketterminal::connectors::Provenance& prov);
    /// Reset to an empty/neutral state.
    void clear_provenance();

  private:
    QLabel* dot_ = nullptr;
    QLabel* text_ = nullptr;
};

} // namespace openmarketterminal::ui
