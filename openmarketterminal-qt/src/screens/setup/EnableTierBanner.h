#pragma once
// Dismissible top-of-window opt-in strip for enabling the AI & automation tier.
// Self-hides when Tier::Ai is already Ready. Never modal — the window stays
// interactive while the one-time Python setup runs.
#include "python/FeatureTier.h"

#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QWidget>

namespace openmarketterminal::python { struct SetupProgress; }

namespace openmarketterminal::screens {

class EnableTierBanner : public QWidget {
    Q_OBJECT
  public:
    explicit EnableTierBanner(QWidget* parent = nullptr);

  private slots:
    void on_enable_clicked();
    void on_progress(const openmarketterminal::python::SetupProgress& p);
    void on_setup_done(bool success, const QString& error);

  private:
    void refresh_visibility();
    QLabel* msg_ = nullptr;
    QPushButton* enable_btn_ = nullptr;
    QPushButton* dismiss_btn_ = nullptr;
    QProgressBar* bar_ = nullptr;
    bool installing_ = false;
};

} // namespace openmarketterminal::screens
