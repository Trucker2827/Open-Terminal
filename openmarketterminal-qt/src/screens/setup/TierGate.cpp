#include "screens/setup/TierGate.h"
#include "python/PythonSetupManager.h"

#include <QVBoxLayout>

namespace openmarketterminal::screens {
using python::FeatureTier;
using python::Tier;
using python::TierState;

TierGate::TierGate(const QString& what, QWidget* parent) : QWidget(parent), what_(what) {
    auto* col = new QVBoxLayout(this);
    col->setAlignment(Qt::AlignCenter);
    col->setSpacing(12);

    msg_ = new QLabel(tr("%1 aren't enabled yet.").arg(what_), this);
    msg_->setAlignment(Qt::AlignCenter);

    enable_btn_ = new QPushButton(tr("Enable AI & automation (~300 MB)"), this);
    enable_btn_->setFixedWidth(280);

    bar_ = new QProgressBar(this);
    bar_->setRange(0, 100);
    bar_->setFixedWidth(280);
    bar_->setVisible(false);

    col->addStretch();
    col->addWidget(msg_, 0, Qt::AlignCenter);
    col->addWidget(enable_btn_, 0, Qt::AlignCenter);
    col->addWidget(bar_, 0, Qt::AlignCenter);
    col->addStretch();

    connect(enable_btn_, &QPushButton::clicked, this, &TierGate::on_enable_clicked);
    connect(&python::PythonSetupManager::instance(), &python::PythonSetupManager::progress_changed,
            this, &TierGate::on_progress);
    connect(&python::PythonSetupManager::instance(), &python::PythonSetupManager::setup_complete,
            this, &TierGate::on_setup_done);

    // If the tier is already ready (e.g. enabled elsewhere), signal immediately.
    if (FeatureTier::instance().state(Tier::Ai) == TierState::Ready)
        QMetaObject::invokeMethod(this, "becameReady", Qt::QueuedConnection);
}

void TierGate::on_enable_clicked() {
    if (installing_) return;
    installing_ = true;
    FeatureTier::instance().set_installing(Tier::Ai, true);
    enable_btn_->setEnabled(false);
    bar_->setVisible(true);
    bar_->setValue(0);
    msg_->setText(tr("Setting up AI & automation…"));
    python::PythonSetupManager::instance().run_setup();
}

void TierGate::on_progress(const python::SetupProgress& p) {
    if (!installing_) return;
    bar_->setValue(p.progress);
    if (!p.message.isEmpty())
        msg_->setText(p.message);
}

void TierGate::on_setup_done(bool success, const QString& error) {
    if (!installing_) return;
    installing_ = false;
    FeatureTier::instance().set_installing(Tier::Ai, false);
    bar_->setVisible(false);
    if (success) {
        FeatureTier::instance().refresh();
        emit becameReady();
    } else {
        enable_btn_->setEnabled(true);
        enable_btn_->setText(tr("Retry"));
        msg_->setText(tr("Couldn't set up %1: %2").arg(what_, error));
    }
}

} // namespace openmarketterminal::screens
