// src/screens/economics/panels/FredPanel.cpp
#include "screens/economics/panels/FredPanel.h"

#include "core/logging/Logger.h"
#include "screens/economics/panels/EconomicsPresets.h"
#include "services/economics/EconomicsService.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QVBoxLayout>

namespace openmarketterminal::screens {
namespace {

static constexpr const char* kFredScript = "fred_data.py";
static constexpr const char* kFredSourceId = "fred";
static constexpr const char* kFredColor = "#EF4444"; // red
} // namespace

FredPanel::FredPanel(QWidget* parent) : EconPanelBase(kFredSourceId, kFredColor, parent) {
    build_base_ui(this);
    connect(&services::EconomicsService::instance(), &services::EconomicsService::result_ready, this,
            &FredPanel::on_result);
}

void FredPanel::activate() {
    show_empty(tr("Add your FRED API key in Settings → Credentials (\"FRED\"), then select a series and click FETCH\n"
                  "Get a free key at: fred.stlouisfed.org/docs/api/api_key.html"));
}

void FredPanel::build_controls(QHBoxLayout* thl) {
    preset_lbl_ = new QLabel(tr("PRESET"));
    preset_lbl_->setStyleSheet(ctrl_label_style());

    preset_combo_ = new QComboBox;
    for (const auto& p : fred_presets())
        preset_combo_->addItem(p.label, p.series_id);
    preset_combo_->setFixedHeight(26);
    preset_combo_->setMinimumWidth(200);
    connect(preset_combo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        const QString code = preset_combo_->itemData(idx).toString();
        if (!code.isEmpty())
            series_input_->setText(code);
    });

    series_lbl_ = new QLabel(tr("SERIES ID"));
    series_lbl_->setStyleSheet(ctrl_label_style());

    series_input_ = new QLineEdit;
    series_input_->setPlaceholderText(tr("e.g. GDPC1"));
    series_input_->setFixedHeight(26);
    series_input_->setFixedWidth(100);

    thl->addWidget(preset_lbl_);
    thl->addWidget(preset_combo_);
    thl->addWidget(series_lbl_);
    thl->addWidget(series_input_);
}

void FredPanel::on_fetch() {
    QString series = series_input_->text().trimmed().toUpper();
    if (series.isEmpty()) {
        const QString code = preset_combo_->currentData().toString();
        if (code.isEmpty()) {
            show_empty(tr("Enter a FRED series ID"));
            return;
        }
        series = code;
    }
    show_loading(tr("Fetching FRED series %1…").arg(series));
    services::EconomicsService::instance().execute(kFredSourceId, kFredScript, "series", {series}, "fred_" + series);
}

void FredPanel::on_result(const QString& request_id, const services::EconomicsResult& result) {
    if (result.source_id != kFredSourceId)
        return;
    if (!result.success) {
        // EconomicsService prefixes errors with "[CODE] " when the script
        // returns a structured error_code. Branch on those for friendly UX.
        if (result.error.startsWith("[MISSING_API_KEY]")) {
            show_error(tr("FRED API key not configured.\n"
                          "Add it in Settings → Credentials (\"FRED\").\n"
                          "Free key: fred.stlouisfed.org/docs/api/api_key.html"));
        } else if (result.error.startsWith("[INVALID_API_KEY]")) {
            show_error(tr("FRED rejected your API key.\n"
                          "Update it in Settings → Credentials — re-issue at:\n"
                          "fred.stlouisfed.org/docs/api/api_key.html"));
        } else if (result.error.startsWith("[RATE_LIMITED]")) {
            show_error(tr("FRED rate-limit hit. Try again in a moment.\n") +
                       result.error.mid(QStringLiteral("[RATE_LIMITED] ").size()));
        } else if (result.error.startsWith("[TIMEOUT]")) {
            show_error(tr("FRED request timed out — check your network and retry."));
        } else if (result.error.contains("API key") || result.error.contains("api_key")) {
            // Legacy un-coded message fallback
            show_error(tr("FRED API key not configured.\n"
                          "Add it in Settings → Credentials (\"FRED\").\n"
                          "Free key: fred.stlouisfed.org/docs/api/api_key.html"));
        } else {
            show_error(result.error);
        }
        return;
    }

    if (request_id.startsWith("fred_")) {
        // FRED returns: {observations: [{date, value}]}
        QJsonArray obs = result.data["observations"].toArray();
        if (obs.isEmpty())
            obs = result.data["data"].toArray();

        // Normalise observation values to numbers. fred_data.py already emits
        // `value` as a JSON number (float); older/raw FRED payloads use strings
        // ("." marks a missing point). Handle both, else every numeric row gets
        // dropped (QJsonValue::toString() is empty for numbers) → "no data".
        QJsonArray clean;
        for (const auto& v : obs) {
            auto obj = v.toObject();
            const QJsonValue raw = obj["value"];
            double d = 0.0;
            bool ok = false;
            if (raw.isDouble()) {
                d = raw.toDouble();
                ok = true;
            } else {
                const QString val_str = raw.toString();
                if (val_str == "." || val_str.isEmpty())
                    continue;
                d = val_str.toDouble(&ok);
            }
            if (!ok)
                continue;
            obj["value"] = d;
            clean.append(obj);
        }

        const QString series = request_id.mid(5); // strip "fred_"
        display(clean, "FRED: " + series);
        LOG_INFO("FredPanel", QString("Displayed %1 observations").arg(clean.size()));
    }
}

QVariantMap FredPanel::save_panel_state() const {
    QVariantMap s;
    if (series_input_) s["series"] = series_input_->text();
    if (preset_combo_) s["preset"] = preset_combo_->currentIndex();
    return s;
}

void FredPanel::restore_panel_state(const QVariantMap& s) {
    if (series_input_ && s.contains("series"))
        series_input_->setText(s["series"].toString());
    if (preset_combo_ && s.contains("preset"))
        preset_combo_->setCurrentIndex(s["preset"].toInt());
}

// ── i18n ──────────────────────────────────────────────────────────────────────

void FredPanel::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    EconPanelBase::changeEvent(event);
}

void FredPanel::retranslateUi() {
    if (preset_lbl_)
        preset_lbl_->setText(tr("PRESET"));
    if (series_lbl_)
        series_lbl_->setText(tr("SERIES ID"));
    if (series_input_)
        series_input_->setPlaceholderText(tr("e.g. GDPC1"));
    EconPanelBase::retranslateUi();
}

} // namespace openmarketterminal::screens
