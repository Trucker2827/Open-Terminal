// src/screens/economics/panels/OpenMarketTerminalMacroPanel.h
// OpenMarketTerminal — Open Macro panel.
//
// Was a OpenMarketTerminal "Coming Soon" stub (required a OpenMarketTerminal subscription). Rewired to
// pull directly from public macro sources via the connector framework:
//   · World Bank (keyless) indicator series
//   · FRED (St. Louis Fed, key-required) series
// A ProvenanceBadge above the table shows source · key requirement · timestamp ·
// live/stale/cache for every fetch.
#pragma once

#include "screens/economics/panels/EconPanelBase.h"

class QComboBox;

namespace openmarketterminal::ui {
class ProvenanceBadge;
}

namespace openmarketterminal::screens {

class OpenMarketTerminalMacroPanel : public EconPanelBase {
    Q_OBJECT
  public:
    explicit OpenMarketTerminalMacroPanel(QWidget* parent = nullptr);
    void activate() override;

  protected:
    void build_controls(QHBoxLayout* thl) override;
    void on_fetch() override;
    void on_result(const QString& request_id, const services::EconomicsResult& result) override;
    void changeEvent(QEvent* event) override;

  private:
    void retranslateUi() override;
    void fetch_selected();

    QLabel*    source_lbl_ = nullptr;
    QComboBox* preset_combo_ = nullptr;
    openmarketterminal::ui::ProvenanceBadge* badge_ = nullptr;
    bool       connected_ = false;
};

} // namespace openmarketterminal::screens
