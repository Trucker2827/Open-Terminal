// src/screens/economics/panels/OpenMarketTerminalMacroPanel.cpp
// OpenMarketTerminal — Open Macro panel.
// Pulls directly from World Bank (keyless) and FRED (key-required) via the
// connector framework, with a ProvenanceBadge showing source/key/timestamp/cache.
#include "screens/economics/panels/OpenMarketTerminalMacroPanel.h"

#include "core/logging/Logger.h"
#include "services/connectors/FredConnector.h"
#include "services/connectors/WorldBankConnector.h"
#include "ui/components/ProvenanceBadge.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>

namespace openmarketterminal::screens {
namespace {

constexpr const char* kSourceId = "openmarketterminal"; // registered source slot — now "Open Macro".
constexpr const char* kColor = "#d97706";    // amber

// Preset indicators. itemData = [source, code, country, label].
struct Preset {
    const char* source;  // "wb" | "fred"
    const char* code;    // indicator / series id
    const char* country; // ISO3 (World Bank only)
    const char* label;
};
const Preset kPresets[] = {
    {"wb", "NY.GDP.MKTP.CD", "USA", "World Bank · US GDP (current US$)"},
    {"wb", "FP.CPI.TOTL.ZG", "USA", "World Bank · US Inflation (CPI %)"},
    {"wb", "SL.UEM.TOTL.ZS", "USA", "World Bank · US Unemployment (%)"},
    {"wb", "SP.POP.TOTL", "WLD", "World Bank · World Population"},
    {"wb", "NY.GDP.MKTP.KD.ZG", "WLD", "World Bank · World GDP Growth (%)"},
    {"fred", "CPIAUCSL", "", "FRED · US CPI (CPIAUCSL) — key required"},
    {"fred", "FEDFUNDS", "", "FRED · Fed Funds Rate (FEDFUNDS) — key required"},
    {"fred", "UNRATE", "", "FRED · US Unemployment (UNRATE) — key required"},
    {"fred", "DGS10", "", "FRED · 10Y Treasury (DGS10) — key required"},
};

QJsonArray points_to_rows_wb(const QVector<connectors::WorldBankPoint>& pts) {
    QJsonArray rows;
    for (const auto& p : pts) {
        QJsonObject o;
        o["date"] = p.date;
        o["value"] = p.has_value ? QJsonValue(p.value) : QJsonValue();
        rows.append(o);
    }
    return rows;
}
QJsonArray points_to_rows_fred(const QVector<connectors::FredPoint>& pts) {
    QJsonArray rows;
    for (const auto& p : pts) {
        QJsonObject o;
        o["date"] = p.date;
        o["value"] = p.has_value ? QJsonValue(p.value) : QJsonValue();
        rows.append(o);
    }
    return rows;
}

} // namespace

OpenMarketTerminalMacroPanel::OpenMarketTerminalMacroPanel(QWidget* parent) : EconPanelBase(kSourceId, kColor, parent) {
    build_base_ui(this);

    // Wire connector results into the table + badge (once).
    if (!connected_) {
        connect(&connectors::WorldBankConnector::instance(), &connectors::WorldBankConnector::series_ready, this,
                [this](QString, QString, QString name, QVector<connectors::WorldBankPoint> series,
                       connectors::Provenance prov) {
                    if (badge_)
                        badge_->set_provenance(prov);
                    display(points_to_rows_wb(series), name);
                });
        connect(&connectors::WorldBankConnector::instance(), &connectors::WorldBankConnector::series_failed, this,
                [this](QString err, connectors::Provenance prov) {
                    if (badge_)
                        badge_->set_provenance(prov);
                    show_error(err);
                });
        connect(&connectors::FredConnector::instance(), &connectors::FredConnector::series_ready, this,
                [this](QString sid, QVector<connectors::FredPoint> series, connectors::Provenance prov) {
                    if (badge_)
                        badge_->set_provenance(prov);
                    display(points_to_rows_fred(series), sid);
                });
        connect(&connectors::FredConnector::instance(), &connectors::FredConnector::series_failed, this,
                [this](QString err, connectors::Provenance prov) {
                    if (badge_)
                        badge_->set_provenance(prov);
                    show_error(err);
                });
        connected_ = true;
    }
}

void OpenMarketTerminalMacroPanel::activate() {
    fetch_selected();
}

void OpenMarketTerminalMacroPanel::build_controls(QHBoxLayout* thl) {
    source_lbl_ = new QLabel(tr("INDICATOR"));
    source_lbl_->setStyleSheet(ctrl_label_style());
    thl->addWidget(source_lbl_);

    preset_combo_ = new QComboBox;
    preset_combo_->setMinimumWidth(320);
    for (const auto& p : kPresets) {
        preset_combo_->addItem(QString::fromUtf8(p.label),
                               QStringList{QString::fromUtf8(p.source), QString::fromUtf8(p.code),
                                           QString::fromUtf8(p.country)});
    }
    connect(preset_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { fetch_selected(); });
    thl->addWidget(preset_combo_);

    thl->addStretch();

    // Transparency badge — source · key requirement · timestamp · live/stale/cache.
    badge_ = new openmarketterminal::ui::ProvenanceBadge(this);
    thl->addWidget(badge_);
}

void OpenMarketTerminalMacroPanel::fetch_selected() {
    if (!preset_combo_)
        return;
    const QStringList d = preset_combo_->currentData().toStringList();
    if (d.size() < 3)
        return;
    const QString source = d.at(0);
    const QString code = d.at(1);
    const QString country = d.at(2);

    show_loading(tr("Fetching %1 …").arg(preset_combo_->currentText()));
    if (source == QLatin1String("fred"))
        connectors::FredConnector::instance().fetch_series(code, 36);
    else
        connectors::WorldBankConnector::instance().fetch_series(country, code, 40);
}

void OpenMarketTerminalMacroPanel::on_fetch() {
    fetch_selected();
}

void OpenMarketTerminalMacroPanel::on_result(const QString& /*request_id*/, const services::EconomicsResult& /*result*/) {
    // Unused — this panel sources from the C++ connector framework, not the
    // Python EconomicsService. Results arrive via the connector signals above.
}

// ── i18n ──────────────────────────────────────────────────────────────────────

void OpenMarketTerminalMacroPanel::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    EconPanelBase::changeEvent(event);
}

void OpenMarketTerminalMacroPanel::retranslateUi() {
    if (source_lbl_)
        source_lbl_->setText(tr("INDICATOR"));
    EconPanelBase::retranslateUi();
}

} // namespace openmarketterminal::screens
