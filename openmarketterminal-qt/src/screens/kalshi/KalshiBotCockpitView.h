#pragma once

// The BOT COCKPIT scene widget (issue #146): the decision-rain view the
// Predictions screen opens over the BOT tab while `kalshi bot` is trading.
//
// This class paints and nothing else. Every value it draws — the mood, the
// columns, whether a column is frozen, the ignitions, the KPI strip, the orbit
// nodes — is read off a `BotCockpitScene` produced by
// `present_bot_cockpit()`, which is pure and regression-tested. If a number is
// not in the scene model, it cannot appear on this widget; there is no second
// place for one to be computed.
//
// Two behaviours are this class's own, and both are consequences of the model:
//
//   * **Timers run only while the widget is visible.** Both are started in
//     `showEvent` and stopped in `hideEvent`, so a closed cockpit costs
//     nothing.
//   * **The animation timer runs only while `scene.motion` is true.** A frozen
//     (stale-report) or dormant cockpit has nothing moving to draw, so it is
//     repainted on data arrival alone rather than at frame rate. That is what
//     keeps a stale cockpit from busy-looping — and it is the same boolean the
//     tests assert on, not a separate widget-side rule.
//
// The widget has NO controls: it cannot arm, size, price, place, or stop
// anything. The kill switch stays on the BOT tab where it already lives.

#include "screens/kalshi/BotCockpitPresentation.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QWidget>

#include <functional>

class QTimer;

namespace openmarketterminal::screens::kalshi {

class KalshiBotCockpitView : public QWidget {
    Q_OBJECT

  public:
    explicit KalshiBotCockpitView(QWidget* parent = nullptr);

    /// How the widget obtains the `kalshi auto live status` object (the screen
    /// already polls it). Absent or returning an empty object is reported as
    /// UNKNOWN / FAIL CLOSED by the model, never as disarmed.
    void set_live_status_provider(std::function<QJsonObject()> provider);

    /// Re-reads the four evidence files and repaints.
    void reload();

    const BotCockpitScene& scene() const { return scene_; }
    /// True exactly when the frame timer is running. Mirrors `scene.motion`
    /// while visible, and is always false while hidden.
    bool animating() const;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    void apply_scene(const BotCockpitScene& scene);
    void sync_animation_timer();
    QColor role_color(const QString& role) const;
    QColor mood_color() const;

    BotCockpitScene scene_;
    std::function<QJsonObject()> live_status_provider_;
    QTimer* data_timer_ = nullptr;
    QTimer* frame_timer_ = nullptr;
    /// Advances one step per frame; frozen columns ignore it, which is what
    /// makes "frozen rain does not fall" literal rather than decorative.
    double phase_ = 0.0;
    /// Event keys already seen. A pulse flashes the first time its key appears
    /// and never again — one flash per real journal row.
    QHash<QString, int> pulse_age_frames_;
    int frame_ = 0;
};

} // namespace openmarketterminal::screens::kalshi
