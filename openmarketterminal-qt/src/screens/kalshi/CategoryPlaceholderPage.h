#pragma once

// A placeholder page for Kalshi categories that have not (yet) grown a
// dedicated view (KalshiScreen's category_stack_ page 2 — see Task 6 of
// docs/superpowers/plans/2026-08-03-per-category-kalshi-views.md). Crypto
// gets the full trading workspace (page 0) and Weather gets the rich
// WeatherScreen (page 1); everything else lands here until it earns its
// own screen. Display only — no data fetch, no controls.

#include <QString>
#include <QWidget>

class QLabel;

namespace openmarketterminal::screens::kalshi {

class CategoryPlaceholderPage final : public QWidget {
    Q_OBJECT

  public:
    explicit CategoryPlaceholderPage(const QString& category = QString(), QWidget* parent = nullptr);

    /// Updates the shown category name (e.g. when the operator switches
    /// between two categories that both land on this placeholder page).
    void set_category(const QString& category);

  private:
    void build_ui();
    void update_text();

    QString category_;
    QLabel* message_ = nullptr;
};

} // namespace openmarketterminal::screens::kalshi
