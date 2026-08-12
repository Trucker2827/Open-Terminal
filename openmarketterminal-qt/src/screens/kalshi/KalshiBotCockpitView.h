#pragma once

// The BOT COCKPIT scene widget (issue #146): the L→R decision-flow view the
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
// The widget has NO trading controls: it cannot arm, size, price, place, or
// stop anything. Click is inspect-only (outside-info ablations / parity on the
// scoreboard nodes, or the PM KPI for bid postmortem detail). Wheel / ↑↓
// scrolls the FLOW lane list when more contracts are published than fit. The
// kill switch stays on the BOT tab.

#include "screens/kalshi/BotCockpitPresentation.h"

#include <QHash>
#include <QJsonObject>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QVector>
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

    /// Which inspectable orbit node / PM KPI is expanded (empty when closed).
    /// Postmortem uses `kBotCockpitPostmortemInspectId`. Test hook.
    QString inspect_node_id() const { return inspect_node_id_; }
    /// Open the bid-postmortem inspect overlay (PM KPI). Read-only.
    void open_postmortem_inspect();
    /// First visible FLOW lane index (scroll offset). Test hook.
    int lane_scroll() const { return lane_scroll_; }

  protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

  private:
    void apply_scene(const BotCockpitScene& scene);
    void sync_animation_timer();
    QColor role_color(const QString& role) const;
    QColor mood_color() const;
    /// Orbit height for the current scene (one or two scoreboard rows).
    int orbit_band_height() const;
    int orbit_row_count() const;
    QRect orbit_band_rect() const;
    QRect orbit_row_rect(int row) const;
    QList<int> node_indices_for_row(int row) const;
    QRectF node_hit_rect(int index) const;
    const BotCockpitNode* node_at(const QPoint& pos) const;
    /// Per-scene-index KPI hit boxes matching paint (empty rect = not shown).
    /// PM is always reserved when present so narrow widths cannot bury it.
    QVector<QRect> kpi_entry_rects() const;
    /// Index of the PM KPI entry, or -1.
    int postmortem_kpi_index() const;
    bool postmortem_kpi_at(const QPoint& pos) const;
    struct KpiStripLayout {
        QVector<QRect> entry_rects;
        QRect overflow_rect;
        QString overflow_text;
    };
    KpiStripLayout layout_kpi_strip() const;
    QString inspect_detail_text() const;
    QString inspect_title() const;
    int flow_lane_capacity() const;
    int max_lane_scroll() const;
    void clamp_lane_scroll();
    bool scroll_lanes_by(int delta_lanes);

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
    /// Click-to-inspect: id of the open scoreboard node, or empty.
    QString inspect_node_id_;
    /// First visible contract lane in the FLOW panel (0-based).
    int lane_scroll_ = 0;
    /// Last painted FLOW body rect — wheel scroll hit-tests against this.
    QRect flow_body_rect_;
    /// Trackpad pixel accumulator so fine gestures still advance one lane.
    double wheel_accum_ = 0.0;
};

} // namespace openmarketterminal::screens::kalshi
