#include "screens/setup/EnableTierBanner.h"
#include "python/PythonSetupManager.h"

#include <QHBoxLayout>

namespace openmarketterminal::screens {
using python::FeatureTier;
using python::Tier;
using python::TierState;

EnableTierBanner::EnableTierBanner(QWidget* parent) : QWidget(parent) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(10, 6, 10, 6);
    msg_ = new QLabel(tr("Enable AI & automation (agents, daemon) — one-time ~300 MB download."), this);
    enable_btn_ = new QPushButton(tr("Enable"), this);
    dismiss_btn_ = new QPushButton(tr("Not now"), this);
    bar_ = new QProgressBar(this);
    bar_->setRange(0, 100);
    bar_->setVisible(false);
    bar_->setFixedWidth(160);
    row->addWidget(msg_, 1);
    row->addWidget(bar_);
    row->addWidget(enable_btn_);
    row->addWidget(dismiss_btn_);

    connect(enable_btn_, &QPushButton::clicked, this, &EnableTierBanner::on_enable_clicked);
    connect(dismiss_btn_, &QPushButton::clicked, this, [this] { hide(); });
    connect(&python::PythonSetupManager::instance(), &python::PythonSetupManager::progress_changed,
            this, &EnableTierBanner::on_progress);
    connect(&python::PythonSetupManager::instance(), &python::PythonSetupManager::setup_complete,
            this, &EnableTierBanner::on_setup_done);
    refresh_visibility();
}

void EnableTierBanner::refresh_visibility() {
    setVisible(FeatureTier::instance().state(Tier::Ai) != TierState::Ready);
}

void EnableTierBanner::on_enable_clicked() {
    if (installing_) return;
    installing_ = true;
    FeatureTier::instance().set_installing(Tier::Ai, true);
    enable_btn_->setEnabled(false);
    dismiss_btn_->setEnabled(false);
    bar_->setVisible(true);
    bar_->setValue(0);
    msg_->setText(tr("Setting up AI & automation…"));
    python::PythonSetupManager::instance().run_setup();
}

void EnableTierBanner::on_progress(const python::SetupProgress& p) {
    if (!installing_) return;
    bar_->setValue(p.progress);
    if (!p.message.isEmpty())
        msg_->setText(p.message);
}

void EnableTierBanner::on_setup_done(bool success, const QString& error) {
    if (!installing_) return;
    installing_ = false;
    FeatureTier::instance().set_installing(Tier::Ai, false);
    bar_->setVisible(false);
    dismiss_btn_->setEnabled(true);
    if (success) {
        FeatureTier::instance().refresh();
        hide();
    } else {
        enable_btn_->setEnabled(true);
        enable_btn_->setText(tr("Retry"));
        msg_->setText(tr("Couldn't set up AI features: %1").arg(error));
    }
}

} // namespace openmarketterminal::screens
