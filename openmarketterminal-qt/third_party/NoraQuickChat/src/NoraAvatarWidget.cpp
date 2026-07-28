#include <NoraQuickChat/NoraAvatarWidget.h>

#include <QPainter>
#include <QImage>
#include <QPainterPath>
#include <QPaintEvent>
#include <QRadialGradient>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>

// Static Qt libraries do not automatically retain compiled resource objects.
// This explicit global-scope initializer creates a linker reference to
// qInitResources_nora(), ensuring Nora's three textures are embedded.
static void initialize_nora_quick_chat_resources() {
    Q_INIT_RESOURCE(nora);
}

namespace NoraQuickChat {

namespace {
constexpr double kSourceWidth = 500.0;
constexpr double kSourceHeight = 870.0;
constexpr double kPi = 3.14159265358979323846;
}

NoraAvatarWidget::NoraAvatarWidget(QWidget* parent)
    : QWidget(parent),
      neutral_(),
      wave_(),
      conversation_() {
    initialize_nora_quick_chat_resources();
    neutral_.load(":/nora/neutral.png");
    wave_.load(":/nora/wave.png");
    conversation_.load(":/nora/conversation.png");
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setAccessibleName(tr("Nora Quick Chat assistant"));
    setToolTip(tr("Nora reacts to Quick Chat listening, thinking, and speaking"));

    clock_.start();
    frame_timer_.setInterval(33);
    connect(&frame_timer_, &QTimer::timeout, this, &NoraAvatarWidget::updateAnimation);
    frame_timer_.start();

    wave_timer_.setSingleShot(true);
    connect(&wave_timer_, &QTimer::timeout, this, [this]() {
        waving_ = false;
        state_ = state_before_wave_;
        update();
    });
}

QPixmap NoraAvatarWidget::launcherPixmap() {
    initialize_nora_quick_chat_resources();
    const QImage source(QStringLiteral(":/nora/neutral.png"));
    if (source.isNull())
        return {};

    int left = source.width();
    int top = source.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            if (qAlpha(source.pixel(x, y)) > 12) {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
        }
    }
    if (right < left || bottom < top)
        return QPixmap::fromImage(source);

    // The launcher is only ~42 px. A full-body reduction becomes an
    // indistinct vertical sliver, so use a square head-and-shoulders/upper-body
    // portrait while leaving the in-chat avatar artwork untouched.
    const int figure_width = right - left + 1;
    const int portrait_side = std::min(figure_width, bottom - top + 1);
    const int center_x = (left + right) / 2;
    const int portrait_left = std::clamp(center_x - portrait_side / 2, 0,
                                         source.width() - portrait_side);
    const QRect portrait_rect(portrait_left, top, portrait_side, portrait_side);
    return QPixmap::fromImage(source.copy(portrait_rect));
}

QSize NoraAvatarWidget::sizeHint() const {
    return {178, 292};
}

QSize NoraAvatarWidget::minimumSizeHint() const {
    return {72, 122};
}

void NoraAvatarWidget::setState(State state) {
    if (waving_)
        state_before_wave_ = state;
    else
        state_ = state;
    update();
}

void NoraAvatarWidget::wave() {
    if (!waving_)
        state_before_wave_ = state_;
    waving_ = true;
    wave_timer_.start(1900);
    update();
}

NoraAvatarWidget::Pose NoraAvatarWidget::pose() const noexcept {
    if (waving_)
        return Pose::Wave;
    if (state_ == State::Speaking)
        return Pose::Conversation;
    return Pose::Neutral;
}

void NoraAvatarWidget::updateAnimation() {
    const qint64 now = clock_.elapsed();
    if (blink_frame_ < 0 && now >= next_blink_ms_)
        blink_frame_ = 0;

    if (blink_frame_ >= 0) {
        ++blink_frame_;
        if (blink_frame_ > 6) {
            blink_frame_ = -1;
            next_blink_ms_ = now + QRandomGenerator::global()->bounded(2300, 5000);
        }
    }
    update();
}

void NoraAvatarWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const qint64 elapsed = clock_.elapsed();
    const double seconds = elapsed / 1000.0;
    const Pose active_pose = pose();

    QColor glow(41, 190, 155, 38);
    if (state_ == State::Listening)
        glow = QColor(64, 220, 125, 55);
    else if (state_ == State::Thinking)
        glow = QColor(242, 145, 20, 52);
    else if (state_ == State::Speaking)
        glow = QColor(36, 190, 220, 58);
    else if (state_ == State::Error)
        glow = QColor(230, 70, 80, 45);

    QRadialGradient radial(rect().center(), width() * 0.54);
    radial.setColorAt(0.0, glow);
    radial.setColorAt(1.0, QColor(glow.red(), glow.green(), glow.blue(), 0));
    painter.fillRect(rect(), radial);

    double sway = 0.0;
    if (state_ == State::Listening)
        sway = std::sin(seconds * 2.2) * 0.010;
    else if (state_ == State::Thinking)
        sway = std::sin(seconds * 1.5) * 0.017;
    else if (state_ == State::Speaking)
        sway = std::sin(seconds * 4.0) * 0.008;
    else if (waving_)
        sway = std::sin(seconds * 8.5) * 0.018;

    const double breath = std::sin(seconds * 2.0) * 0.006;
    const double bob = std::sin(seconds * 2.0) * 1.8;
    const double available_w = width() * 0.94;
    const double available_h = height() * 0.96;
    const double scale = std::min(available_w / kSourceWidth, available_h / kSourceHeight);
    const QSizeF base_size(kSourceWidth * scale, kSourceHeight * scale * (1.0 + breath));
    QRectF character_rect(
        (width() - base_size.width()) / 2.0,
        height() - base_size.height() - 2.0 + bob,
        base_size.width(),
        base_size.height());

    const QPixmap* pixmap = &neutral_;
    if (active_pose == Pose::Wave)
        pixmap = &wave_;
    else if (active_pose == Pose::Conversation)
        pixmap = &conversation_;

    painter.save();
    painter.translate(character_rect.center());
    painter.rotate(sway * 180.0 / kPi);
    painter.translate(-character_rect.center());
    painter.drawPixmap(character_rect, *pixmap, pixmap->rect());
    paintFaceAnimation(painter, character_rect, active_pose);
    painter.restore();
}

void NoraAvatarWidget::paintFaceAnimation(
    QPainter& painter,
    const QRectF& character_rect,
    Pose active_pose) {
    QPointF mouth_px(250.0, 158.0);
    QPointF right_eye_px(233.0, 98.0); // Nora's right; viewer's left
    QPointF left_eye_px(271.0, 98.0);

    if (active_pose == Pose::Neutral) {
        mouth_px = QPointF(257.5, 130.0);
        right_eye_px = QPointF(239.0, 91.0);
        left_eye_px = QPointF(270.0, 91.0);
    } else if (active_pose == Pose::Wave) {
        mouth_px = QPointF(201.0, 132.0);
        right_eye_px = QPointF(184.0, 92.0);
        left_eye_px = QPointF(217.0, 92.0);
    }

    const auto map_point = [&character_rect](const QPointF& source) {
        return QPointF(
            character_rect.left() + source.x() / kSourceWidth * character_rect.width(),
            character_rect.top() + source.y() / kSourceHeight * character_rect.height());
    };

    if (blink_frame_ >= 0) {
        const double blink = std::sin(std::clamp(blink_frame_ / 6.0, 0.0, 1.0) * kPi);
        const double sx = character_rect.width() / kSourceWidth;
        const double sy = character_rect.height() / kSourceHeight;
        const QColor skin(184, 128, 92, static_cast<int>(235 * blink));
        painter.setPen(Qt::NoPen);
        painter.setBrush(skin);

        const QPointF right = map_point(right_eye_px);
        const QPointF left = map_point(left_eye_px);
        painter.drawEllipse(
            QRectF(right.x() - 3.0 * sx, right.y() - 1.0 * sy,
                   6.0 * sx, 2.0 * sy * blink));
        painter.drawEllipse(
            QRectF(left.x() - 2.2 * sx, left.y() - 0.8 * sy,
                   4.4 * sx, 1.6 * sy * blink));
    }

    if (state_ != State::Speaking || active_pose != Pose::Conversation)
        return;

    const QPointF mouth_center = map_point(mouth_px);
    const double sx = character_rect.width() / kSourceWidth;
    const double sy = character_rect.height() / kSourceHeight;
    const int viseme = static_cast<int>((clock_.elapsed() / 105) % 5);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(178, 117, 87));
    painter.drawEllipse(
        QRectF(mouth_center.x() - 9.0 * sx, mouth_center.y() - 5.0 * sy,
               18.0 * sx, 10.0 * sy));

    painter.setBrush(QColor(67, 22, 24));
    QRectF opening;
    switch (viseme) {
        case 0:
            opening = QRectF(mouth_center.x() - 6.0 * sx, mouth_center.y() - 0.6 * sy,
                             12.0 * sx, 1.2 * sy);
            break;
        case 1:
            opening = QRectF(mouth_center.x() - 7.0 * sx, mouth_center.y() - 1.8 * sy,
                             14.0 * sx, 3.6 * sy);
            break;
        case 2:
            opening = QRectF(mouth_center.x() - 4.0 * sx, mouth_center.y() - 4.0 * sy,
                             8.0 * sx, 8.0 * sy);
            break;
        case 3:
            opening = QRectF(mouth_center.x() - 2.8 * sx, mouth_center.y() - 4.5 * sy,
                             5.6 * sx, 9.0 * sy);
            break;
        default:
            opening = QRectF(mouth_center.x() - 5.0 * sx, mouth_center.y() - 2.4 * sy,
                             10.0 * sx, 4.8 * sy);
            break;
    }
    painter.drawEllipse(opening);
}

} // namespace NoraQuickChat
