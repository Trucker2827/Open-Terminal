#pragma once
// Reusable graceful-degradation placeholder for a Python-dependent surface.
// When Tier::Ai is not ready, a host panel shows a TierGate instead of its live
// view: a centered message + an "Enable AI & automation" button that runs the
// one-time setup inline (progress + Retry). Emits becameReady() when the tier
// flips to Ready so the host can swap in the real view. Never shows a raw error.
#include "python/FeatureTier.h"

#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QWidget>

namespace openmarketterminal::python { struct SetupProgress; }

namespace openmarketterminal::screens {

class TierGate : public QWidget {
    Q_OBJECT
  public:
    // `what` names the blocked feature, e.g. "AI agents", shown in the message.
    explicit TierGate(const QString& what, QWidget* parent = nullptr);

  signals:
    void becameReady();

  private slots:
    void on_enable_clicked();
    void on_progress(const openmarketterminal::python::SetupProgress& p);
    void on_setup_done(bool success, const QString& error);

  private:
    QString what_;
    QLabel* msg_ = nullptr;
    QPushButton* enable_btn_ = nullptr;
    QProgressBar* bar_ = nullptr;
    bool installing_ = false;
};

} // namespace openmarketterminal::screens
