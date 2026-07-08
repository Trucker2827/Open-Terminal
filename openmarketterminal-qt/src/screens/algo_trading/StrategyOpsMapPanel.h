#pragma once

#include <QDateTime>
#include <QShowEvent>
#include <QTimer>
#include <QWidget>

namespace openmarketterminal::screens {

class StrategyOpsMapPanel : public QWidget {
    Q_OBJECT
  public:
    explicit StrategyOpsMapPanel(QWidget* parent = nullptr);

  public slots:
    void refresh();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    struct BookNode {
        QString kind;
        QString source;
        QString horizon;
        QString status;
        int resolved = 0;
        int open = 0;
        double net_pnl = 0.0;
        double hit_rate = 0.0;
        bool eligible = false;
        bool hypothetical = false;
        bool chronos = false;
        bool price_forecast = false;
    };

    void animate();
    void draw_background(QPainter& p, const QRectF& r);
    void draw_hud(QPainter& p, const QRectF& r);
    void draw_flow(QPainter& p, const QRectF& r);
    void draw_book_orbit(QPainter& p, const QRectF& r);
    void draw_node(QPainter& p, const QPointF& c, qreal radius, const QString& title,
                   const QString& subtitle, const QColor& color, bool active);
    void draw_particle_line(QPainter& p, const QPointF& a, const QPointF& b, const QColor& color,
                            qreal phase_offset);
    QColor color_for_book(const BookNode& b) const;

    QTimer frame_timer_;
    QTimer refresh_timer_;
    qreal phase_ = 0.0;
    bool first_show_ = true;
    qint64 last_refresh_ms_ = 0;

    QVector<BookNode> books_;
    int active_books_ = 0;
    int chronos_books_ = 0;
    int spot_books_ = 0;
    int hypothetical_books_ = 0;
    int open_positions_ = 0;
    int resolved_total_ = 0;
    int eligible_books_ = 0;
    double net_pnl_total_ = 0.0;
    QString status_text_ = QStringLiteral("waiting for sandbox data");
};

} // namespace openmarketterminal::screens
