#pragma once

#include <QElapsedTimer>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

namespace NoraQuickChat {

class NoraAvatarWidget final : public QWidget {
    Q_OBJECT

  public:
    enum class State {
        Idle,
        Listening,
        Thinking,
        Speaking,
        Error
    };
    Q_ENUM(State)

    explicit NoraAvatarWidget(QWidget* parent = nullptr);

    /// Cropped transparent Nora artwork for compact launchers and badges.
    static QPixmap launcherPixmap();

    State state() const noexcept { return state_; }
    void setState(State state);
    void wave();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    enum class Pose {
        Neutral,
        Wave,
        Conversation
    };

    QPixmap neutral_;
    QPixmap wave_;
    QPixmap conversation_;
    QTimer frame_timer_;
    QTimer wave_timer_;
    QElapsedTimer clock_;
    State state_ = State::Idle;
    State state_before_wave_ = State::Idle;
    bool waving_ = false;
    int blink_frame_ = -1;
    qint64 next_blink_ms_ = 2600;

    Pose pose() const noexcept;
    void updateAnimation();
    void paintFaceAnimation(QPainter& painter, const QRectF& character_rect, Pose pose);
};

} // namespace NoraQuickChat
