#pragma once
// Pure tier-capability mapping over the existing PythonSetupManager state.
// Reports which capability tier is ready so UI can gracefully degrade.
#include "python/PythonSetupManager.h"

#include <QObject>

namespace openmarketterminal::python {

enum class Tier { Core, Ai, Forecasting };
enum class TierState { NotInstalled, Installing, Ready, Failed };

// Pure mapping — no I/O, header-inline so it is unit-testable standalone
// (the FeatureTier class methods below pull in PythonSetupManager/PythonRunner).
// `installing` reflects an in-flight ensure_tier().
inline TierState tier_state_from(Tier tier, const SetupStatus& status, bool python_available,
                                 bool chronos_ready, bool installing) {
    if (tier == Tier::Core)
        return TierState::Ready;
    if (installing)
        return TierState::Installing;
    const bool ai_ready = !status.needs_setup && status.venv_numpy1_ready &&
                          status.venv_numpy2_ready && python_available;
    if (tier == Tier::Ai)
        return ai_ready ? TierState::Ready : TierState::NotInstalled;
    // Tier::Forecasting
    return (ai_ready && chronos_ready) ? TierState::Ready : TierState::NotInstalled;
}

class FeatureTier : public QObject {
    Q_OBJECT
  public:
    static FeatureTier& instance();

    TierState state(Tier tier) const;
    bool is_ready(Tier tier) const { return state(tier) == TierState::Ready; }

    // Mark a tier as installing / cleared, so state() reports Installing while
    // PythonSetupManager runs. Emits tier_changed.
    void set_installing(Tier tier, bool installing);

    // Recompute from live status and emit tier_changed for any transitions.
    void refresh();

  signals:
    void tier_changed(Tier tier, TierState state);

  private:
    explicit FeatureTier(QObject* parent = nullptr);
    Q_DISABLE_COPY_MOVE(FeatureTier)
    bool chronos_ready() const;

    bool installing_ai_ = false;
    bool installing_forecasting_ = false;
};

} // namespace openmarketterminal::python
