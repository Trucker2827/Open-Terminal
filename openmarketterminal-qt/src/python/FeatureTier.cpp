#include "python/FeatureTier.h"
#include "python/PythonRunner.h"

#include <QFileInfo>

namespace openmarketterminal::python {

FeatureTier& FeatureTier::instance() {
    static FeatureTier inst;
    return inst;
}

FeatureTier::FeatureTier(QObject* parent) : QObject(parent) {}

bool FeatureTier::chronos_ready() const {
    const auto status = PythonSetupManager::instance().check_status();
    return QFileInfo::exists(status.install_dir + "/.chronos_ready");
}

TierState FeatureTier::state(Tier tier) const {
    const auto status = PythonSetupManager::instance().check_status();
    const bool py = PythonRunner::instance().is_available();
    const bool installing = (tier == Tier::Ai) ? installing_ai_ : installing_forecasting_;
    return tier_state_from(tier, status, py, chronos_ready(), installing);
}

void FeatureTier::set_installing(Tier tier, bool installing) {
    if (tier == Tier::Ai)
        installing_ai_ = installing;
    else if (tier == Tier::Forecasting)
        installing_forecasting_ = installing;
    emit tier_changed(tier, state(tier));
}

void FeatureTier::refresh() {
    emit tier_changed(Tier::Ai, state(Tier::Ai));
    emit tier_changed(Tier::Forecasting, state(Tier::Forecasting));
}

} // namespace openmarketterminal::python
