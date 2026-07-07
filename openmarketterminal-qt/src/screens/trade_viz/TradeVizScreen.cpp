// Trade Intelligence - live/fallback trade-flow visualization.
#include "screens/trade_viz/TradeVizScreen.h"

#include "core/session/ScreenStateManager.h"
#include "python/PythonRunner.h"
#include "ui/theme/Theme.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSplitter>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace openmarketterminal::screens {

static const char* FONT = "'Consolas','Courier New',monospace";

static QString combo_ss() {
    return QString("QComboBox { background: %1; color: %2; border: 1px solid %3;"
                   "  padding: 3px 8px; font-size: 11px; font-family: %4; min-width: 96px; }"
                   "QComboBox:hover { border-color: %5; }"
                   "QComboBox::drop-down { border: none; width: 16px; }"
                   "QComboBox QAbstractItemView { background: %6; color: %2; border: 1px solid %3;"
                   "  selection-background-color: %7; selection-color: %8; font-family: %4; }")
        .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(), FONT,
             ui::colors::BORDER_BRIGHT(), ui::colors::BG_BASE(), ui::colors::BG_HOVER(), ui::colors::AMBER());
}

static QString button_ss() {
    return QString("QPushButton { background: %1; color: %2; border: 1px solid %3;"
                   "  padding: 5px 12px; font-size: 11px; font-weight: 800; font-family: %4; }"
                   "QPushButton:hover { border-color: %5; color: %5; }"
                   "QPushButton:disabled { color: %6; border-color: %3; }")
        .arg(ui::colors::BG_BASE(), ui::colors::AMBER(), ui::colors::BORDER_DIM(), FONT,
             ui::colors::BORDER_BRIGHT(), ui::colors::TEXT_TERTIARY());
}

static QString table_ss() {
    return QString("QTableWidget { background: %1; color: %2; border: none;"
                   "  gridline-color: %3; font-size: 12px; font-family: %4; }"
                   "QTableWidget::item { padding: 3px 8px; border-bottom: 1px solid %3; }"
                   "QTableWidget::item:selected { background: %5; color: %2; }"
                   "QHeaderView::section { background: %1; color: %6; font-size: 11px; font-weight: bold;"
                   "  border: none; border-bottom: 1px solid %7; padding: 6px 8px; font-family: %4; }"
                   "QScrollBar:vertical { width: 5px; background: transparent; }"
                   "QScrollBar::handle:vertical { background: %8; }"
                   "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }")
        .arg(ui::colors::BG_BASE(), ui::colors::TEXT_PRIMARY(), ui::colors::BG_RAISED(), FONT,
             ui::colors::BG_HOVER(), ui::colors::TEXT_SECONDARY(), ui::colors::BORDER_DIM(),
             ui::colors::BORDER_MED());
}

static QVector<TradeVizPartner> fallback_partners() {
    return {
        {"Mexico", "MEX", 437898.0, 345098.0, 782996.0, -92800.0},
        {"Canada", "CAN", 406282.0, 297329.0, 703611.0, -108953.0},
        {"China", "CHN", 427230.0, 177093.0, 604323.0, -250137.0},
        {"Germany", "DEU", 145632.0, 90342.5, 235974.5, -55289.5},
        {"Japan", "JPN", 138420.0, 94699.0, 233119.0, -43721.0},
        {"South Korea", "KOR", 115210.0, 88830.2, 204040.2, -26379.8},
        {"Vietnam", "VNM", 109450.0, 42081.8, 151531.8, -67368.2},
        {"United Kingdom", "GBR", 67342.0, 73428.6, 140770.6, 6086.6},
        {"Türkiye", "TUR", 87120.0, 45012.1, 132132.1, -42107.9},
        {"Ireland", "IRL", 82340.0, 44515.1, 126855.1, -37824.9},
        {"Netherlands", "NLD", 56120.0, 52126.2, 108246.2, -3993.8},
        {"France", "FRA", 62340.0, 45550.2, 107890.2, -16789.8},
        {"Italy", "ITA", 67230.0, 37358.7, 104588.7, -29871.3},
        {"Singapore", "SGP", 48120.0, 51015.2, 99135.2, 2895.2},
        {"Switzerland", "CHE", 54230.0, 38722.6, 92952.6, -15507.4},
    };
}

static bool is_india_partner(const QString& iso3, const QString& name) {
    return iso3.compare(QLatin1String("IND"), Qt::CaseInsensitive) == 0
        || name.compare(QLatin1String("India"), Qt::CaseInsensitive) == 0;
}

static QString normalize_partner_name(const QString& iso3, const QString& name) {
    if (iso3.compare(QLatin1String("TUR"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("Türkiye");
    const QString lower = name.toLower();
    if (lower == QLatin1String("turkey") || lower == QLatin1String("turkiye") || lower == QLatin1String("türkiye"))
        return QStringLiteral("Türkiye");
    return name;
}

static QString money_m(double value) {
    const double abs_v = std::abs(value);
    if (abs_v >= 1'000'000.0)
        return QString("$%1T").arg(value / 1'000'000.0, 0, 'f', 2);
    if (abs_v >= 1'000.0)
        return QString("$%1B").arg(value / 1'000.0, 0, 'f', 1);
    return QString("$%1M").arg(value, 0, 'f', 0);
}

static bool has_yoy(double value) {
    return !std::isnan(value) && std::isfinite(value);
}

static QString iso3_to_iso2(const QString& iso3) {
    const QString key = iso3.trimmed().toUpper();
    static const std::pair<const char*, const char*> kIsoMap[] = {
        {"ARG", "AR"}, {"AUS", "AU"}, {"AUT", "AT"}, {"BEL", "BE"}, {"BRA", "BR"}, {"CAN", "CA"},
        {"CHE", "CH"}, {"CHL", "CL"}, {"CHN", "CN"}, {"COL", "CO"}, {"DEU", "DE"}, {"DNK", "DK"},
        {"ECU", "EC"}, {"ESP", "ES"}, {"FIN", "FI"}, {"FRA", "FR"}, {"GBR", "GB"}, {"HKG", "HK"},
        {"IDN", "ID"}, {"IND", "IN"}, {"IRL", "IE"}, {"ISR", "IL"}, {"ITA", "IT"}, {"JPN", "JP"},
        {"KOR", "KR"}, {"MEX", "MX"}, {"MYS", "MY"}, {"NGA", "NG"}, {"NLD", "NL"}, {"NOR", "NO"},
        {"NZL", "NZ"}, {"PHL", "PH"}, {"POL", "PL"}, {"PRT", "PT"}, {"SAU", "SA"}, {"SGP", "SG"},
        {"SWE", "SE"}, {"THA", "TH"}, {"TUR", "TR"}, {"USA", "US"}, {"VNM", "VN"}, {"ZAF", "ZA"},
    };
    for (const auto& row : kIsoMap) {
        if (key == QLatin1String(row.first))
            return QString::fromLatin1(row.second);
    }
    return {};
}

static QString flag_emoji_for_iso3(const QString& iso3) {
    const QString iso2 = iso3_to_iso2(iso3);
    if (iso2.size() != 2)
        return {};
    QString flag;
    flag.reserve(4);
    for (const QChar ch : iso2) {
        const ushort letter = ch.toUpper().unicode();
        if (letter < 'A' || letter > 'Z')
            return {};
        const char32_t indicator[] = {static_cast<char32_t>(0x1F1E6u + static_cast<uint>(letter - 'A')), 0};
        flag.append(QString::fromUcs4(indicator));
    }
    return flag;
}

class TradeFlowChordWidget : public QWidget {
  public:
    explicit TradeFlowChordWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumSize(460, 360);
        setMouseTracking(true);
        setStyleSheet(QString("background: %1;").arg(ui::colors::BG_BASE()));
    }

    void set_partners(QVector<TradeVizPartner> partners, QString reporter) {
        partners_ = std::move(partners);
        reporter_ = reporter.isEmpty() ? QStringLiteral("USA") : reporter;
        update();
    }

    void set_phase(double phase) {
        phase_ = phase;
        update();
    }

    void set_selected_iso(QString iso3) {
        selected_iso_ = std::move(iso3);
        update();
    }

    std::function<void(const QString&)> on_partner_selected;
    std::function<void(const QString&)> on_partner_activated;

  protected:
    void mouseMoveEvent(QMouseEvent* event) override {
        const QString iso = node_at(event->pos());
        if (iso != hovered_iso_) {
            hovered_iso_ = iso;
            setCursor(hovered_iso_.isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
            update();
        }
        if (!iso.isEmpty())
            QToolTip::showText(event->globalPosition().toPoint(), tooltip_for(iso), this);
        else
            QToolTip::hideText();
    }

    void leaveEvent(QEvent*) override {
        if (!hovered_iso_.isEmpty()) {
            hovered_iso_.clear();
            setCursor(Qt::ArrowCursor);
            QToolTip::hideText();
            update();
        }
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton)
            return;
        const QString iso = node_at(event->pos());
        if (!iso.isEmpty()) {
            selected_iso_ = iso;
            if (on_partner_selected)
                on_partner_selected(iso);
            update();
        }
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton)
            return;
        const QString iso = node_at(event->pos());
        if (!iso.isEmpty() && on_partner_activated)
            on_partner_activated(iso);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        node_hitboxes_.clear();

        const int w = width();
        const int h = height();
        const int cx = w / 2;
        const int cy = h / 2 + 8;
        const int radius = std::min(cx, cy) - 72;
        if (radius < 70 || partners_.isEmpty())
            return;

        QRadialGradient bg(QPointF(cx, cy), radius * 1.25);
        bg.setColorAt(0.0, QColor(8, 14, 15));
        bg.setColorAt(0.45, QColor(4, 8, 8));
        bg.setColorAt(1.0, QColor(0, 0, 0));
        p.fillRect(rect(), bg);

        p.setPen(QPen(QColor(20, 28, 28), 1));
        for (int r = 1; r <= 5; ++r)
            p.drawEllipse(QPointF(cx, cy), radius * r / 5.0, radius * r / 5.0);

        double max_val = 1.0;
        for (const auto& row : partners_)
            max_val = std::max(max_val, std::max(row.imports_m, row.exports_m));

        const int n = std::min(static_cast<int>(partners_.size()), 18);
        auto point_at = [&](int i, double extra = 0.0) {
            const double angle = 2.0 * M_PI * i / std::max(1, n) - M_PI / 2.0;
            return QPointF(cx + (radius + extra) * std::cos(angle),
                           cy + (radius + extra) * std::sin(angle));
        };

        for (int i = 0; i < n; ++i) {
            p.setPen(QPen(QColor(18, 26, 26), 1));
            p.drawLine(QPointF(cx, cy), point_at(i, 8));
        }

        auto draw_arc = [&](int i, double value, bool import_flow) {
            if (value <= 0.0)
                return;
            const QString iso = partners_[i].iso3;
            const bool focused = !selected_iso_.isEmpty() || !hovered_iso_.isEmpty();
            const bool active = iso == selected_iso_ || iso == hovered_iso_;
            const QPointF outer = point_at(i);
            const QPointF center(cx, cy);
            const QPointF mid = (outer + center) / 2.0;
            const QPointF d = outer - center;
            const double len = std::hypot(d.x(), d.y());
            if (len < 1.0)
                return;
            const QPointF normal(-d.y() / len, d.x() / len);
            const double strength = std::clamp(value / max_val, 0.05, 1.0);
            const int strands = 2 + static_cast<int>(5 * strength);
            const double pulse = 0.35 + 0.65 * std::abs(std::sin(phase_ + i * 0.43));

            for (int s = 0; s < strands; ++s) {
                const double offset = (s - strands / 2.0) * 2.5;
                const double curve = (import_flow ? 1.0 : -1.0) * (28.0 + 26.0 * strength + offset);
                QColor color = import_flow
                                   ? QColor(234, 88 + s * 9, 12, 70 + static_cast<int>(135 * strength * pulse))
                                   : QColor(0, 215, 200 - s * 12, 70 + static_cast<int>(125 * strength * pulse));
                if (focused && !active)
                    color.setAlpha(18);
                else if (active)
                    color.setAlpha(std::min(255, color.alpha() + 80));
                QPen pen(color, 1.2 + 2.0 * strength);
                if (active)
                    pen.setWidthF(pen.widthF() + 1.6);
                pen.setCapStyle(Qt::RoundCap);
                p.setPen(pen);

                QPainterPath path;
                if (import_flow)
                    path.moveTo(outer);
                else
                    path.moveTo(center);
                path.quadTo(mid + normal * curve, import_flow ? center : outer);
                p.drawPath(path);
            }
        };

        for (int i = n - 1; i >= 0; --i)
            draw_arc(i, partners_[i].imports_m, true);
        for (int i = n - 1; i >= 0; --i)
            draw_arc(i, partners_[i].exports_m, false);

        QFont label_font(FONT, 9);
        label_font.setBold(true);
        p.setFont(label_font);
        for (int i = 0; i < n; ++i) {
            const QPointF node = point_at(i);
            const double strength = std::clamp(partners_[i].total_m / std::max(1.0, partners_.first().total_m), 0.2, 1.0);
            const int size = 14 + static_cast<int>(11 * strength);
            const bool active = partners_[i].iso3 == selected_iso_ || partners_[i].iso3 == hovered_iso_;
            QRectF halo(node.x() - size * 1.7, node.y() - size * 1.7, size * 3.4, size * 3.4);
            QColor halo_color = partners_[i].balance_m >= 0 ? QColor(0, 210, 170, 34) : QColor(234, 88, 12, 38);
            if (active)
                halo_color.setAlpha(105);
            p.setBrush(halo_color);
            p.setPen(Qt::NoPen);
            p.drawEllipse(halo);

            QRectF box(node.x() - size * 1.15, node.y() - size * 0.78, size * 2.3, size * 1.56);
            node_hitboxes_.append({partners_[i].iso3, halo.adjusted(-8, -8, 8, 8)});
            p.setBrush(QColor(25, 30, 30));
            p.setPen(QPen(partners_[i].balance_m >= 0 ? QColor(0, 210, 170) : QColor(234, 88, 12), active ? 2.2 : 1.0));
            p.drawRoundedRect(box, 3, 3);

            const QString flag = flag_emoji_for_iso3(partners_[i].iso3);
            if (!flag.isEmpty()) {
                p.save();
                QFont flag_font(QStringLiteral("Apple Color Emoji"), std::max(11, static_cast<int>(size * 0.86)));
                p.setFont(flag_font);
                p.setPen(Qt::white);
                p.drawText(box.adjusted(1, -1, -1, 1), Qt::AlignCenter, flag);
                p.restore();
            } else {
                p.save();
                QFont iso_font(FONT, std::max(7, static_cast<int>(size * 0.42)));
                iso_font.setBold(true);
                p.setFont(iso_font);
                p.setPen(QColor(ui::colors::TEXT_SECONDARY()));
                p.drawText(box, Qt::AlignCenter, partners_[i].iso3.left(3));
                p.restore();
            }

            const QPointF label_pt = point_at(i, 42);
            QRectF label(label_pt.x() - 42, label_pt.y() - 10, 84, 20);
            p.setPen(QColor(ui::colors::TEXT_PRIMARY()));
            p.drawText(label, Qt::AlignCenter, partners_[i].iso3.left(4));
        }

        QRectF center(cx - 36, cy - 26, 72, 52);
        p.setBrush(QColor(10, 16, 16));
        p.setPen(QPen(QColor(ui::colors::AMBER()), 1.5));
        p.drawRoundedRect(center, 5, 5);
        QFont center_font(FONT, 12);
        center_font.setBold(true);
        p.setFont(center_font);
        p.setPen(QColor(ui::colors::AMBER()));
        const QString reporter_flag = flag_emoji_for_iso3(reporter_);
        if (!reporter_flag.isEmpty()) {
            p.save();
            p.setFont(QFont(QStringLiteral("Apple Color Emoji"), 13));
            p.drawText(center.adjusted(0, 2, 0, -22), Qt::AlignCenter, reporter_flag);
            p.restore();
        }
        p.drawText(center.adjusted(0, reporter_flag.isEmpty() ? 5 : 16, 0, -15), Qt::AlignCenter, reporter_);
        p.setFont(QFont(FONT, 7));
        p.setPen(QColor(ui::colors::TEXT_SECONDARY()));
        p.drawText(center.adjusted(0, 26, 0, 0), Qt::AlignCenter, "IMPORT / EXPORT");

        p.setFont(QFont(FONT, 9, QFont::Bold));
        p.setPen(QColor(234, 88, 12));
        p.drawText(14, 22, "ORANGE: IMPORTS");
        p.setPen(QColor(0, 210, 190));
        p.drawText(14, 40, "CYAN: EXPORTS");
        p.setPen(QColor(ui::colors::TEXT_SECONDARY()));
        p.drawText(14, h - 16, QString("Largest route: %1").arg(partners_.isEmpty() ? "-" : money_m(partners_.first().total_m)));
    }

  private:
    QString node_at(const QPoint& pos) const {
        for (auto it = node_hitboxes_.crbegin(); it != node_hitboxes_.crend(); ++it) {
            if (it->second.contains(pos))
                return it->first;
        }
        return {};
    }

    const TradeVizPartner* partner_for(const QString& iso) const {
        for (const auto& row : partners_) {
            if (row.iso3 == iso)
                return &row;
        }
        return nullptr;
    }

    QString tooltip_for(const QString& iso) const {
        const auto* row = partner_for(iso);
        if (!row)
            return {};
        return QString("%1 %2\nImports: %3\nExports: %4\nBalance: %5\nYoY: %6\nClick: inspect route\nDouble-click: pivot reporter")
            .arg(row->iso3, row->name)
            .arg(money_m(row->imports_m))
            .arg(money_m(row->exports_m))
            .arg(QString("%1%2").arg(row->balance_m >= 0 ? "+" : "").arg(money_m(row->balance_m)))
            .arg(has_yoy(row->yoy_pct) ? QString("%1%2%").arg(row->yoy_pct >= 0 ? "+" : "").arg(row->yoy_pct, 0, 'f', 1)
                                       : QString("--"));
    }

    QVector<TradeVizPartner> partners_;
    QVector<std::pair<QString, QRectF>> node_hitboxes_;
    QString reporter_ = "USA";
    QString hovered_iso_;
    QString selected_iso_;
    double phase_ = 0.0;
};

QWidget* TradeVizScreen::build_tab_bar() {
    auto* bar = new QWidget(this);
    bar->setStyleSheet(QString("background: %1; border-bottom: 1px solid %2;")
                           .arg(ui::colors::BG_BASE(), ui::colors::BORDER_BRIGHT()));
    bar->setFixedHeight(30);
    auto* hl = new QHBoxLayout(bar);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(0);

    const QStringList labels = {tr("Flow"), tr("Table"), tr("Impact"), tr("Sources")};
    for (int i = 0; i < labels.size(); ++i) {
        auto* btn = new QPushButton(QString("%1) %2").arg(21 + i).arg(labels[i]), bar);
        btn->setFlat(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setMinimumWidth(104);
        connect(btn, &QPushButton::clicked, this, [this, i]() { set_active_tab(i); });
        hl->addWidget(btn);
        tab_buttons_.append(btn);
    }
    hl->addStretch();

    flow_title_ = new QLabel(tr("Trade Intelligence"));
    flow_title_->setStyleSheet(QString("color: %1; font-size: 16px; font-weight: bold; background: transparent;"
                                       " padding-right: 14px; font-family: %2;")
                                   .arg(ui::colors::AMBER(), FONT));
    hl->addWidget(flow_title_);
    update_tab_styles();
    return bar;
}

QWidget* TradeVizScreen::build_filter_bar() {
    auto* bar = new QWidget(this);
    bar->setStyleSheet(QString("background: %1; border-bottom: 1px solid %2;")
                           .arg(ui::colors::BG_RAISED(), ui::colors::BORDER_DIM()));
    bar->setFixedHeight(38);
    auto* hl = new QHBoxLayout(bar);
    hl->setContentsMargins(10, 0, 10, 0);
    hl->setSpacing(8);

    country_combo_ = new QComboBox;
    country_combo_->setStyleSheet(combo_ss());
    const QList<QPair<QString, QString>> countries = {
        {"United States", "USA"}, {"China", "CHN"}, {"Germany", "DEU"}, {"Japan", "JPN"},
        {"United Kingdom", "GBR"}, {"France", "FRA"}, {"Türkiye", "TUR"}, {"Italy", "ITA"},
        {"Canada", "CAN"}, {"South Korea", "KOR"}, {"Mexico", "MEX"}, {"Brazil", "BRA"},
        {"Australia", "AUS"}, {"Netherlands", "NLD"}, {"Switzerland", "CHE"},
    };
    for (const auto& c : countries)
        country_combo_->addItem(c.first, c.second);
    hl->addWidget(country_combo_);

    commodity_combo_ = new QComboBox;
    commodity_combo_->setStyleSheet(combo_ss());
    commodity_combo_->addItem(tr("All Goods"), "TOTAL");
    commodity_combo_->addItem(tr("Energy"), "27");
    commodity_combo_->addItem(tr("Machinery"), "84");
    commodity_combo_->addItem(tr("Electronics"), "85");
    commodity_combo_->addItem(tr("Vehicles"), "87");
    commodity_combo_->addItem(tr("Aircraft"), "88");
    commodity_combo_->addItem(tr("Pharma"), "30");
    commodity_combo_->addItem(tr("Steel"), "72");
    hl->addWidget(commodity_combo_);

    order_caption_ = new QLabel(tr("Rank"));
    order_caption_->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: bold; background: transparent; font-family: %2;")
                                      .arg(ui::colors::TEXT_SECONDARY(), FONT));
    hl->addWidget(order_caption_);
    order_combo_ = new QComboBox;
    order_combo_->setStyleSheet(combo_ss());
    order_combo_->addItems({tr("Total"), tr("Imports"), tr("Exports"), tr("Balance"), tr("YoY")});
    hl->addWidget(order_combo_);

    period_caption_ = new QLabel(tr("Period"));
    period_caption_->setStyleSheet(order_caption_->styleSheet());
    hl->addWidget(period_caption_);
    period_combo_ = new QComboBox;
    period_combo_->setStyleSheet(combo_ss());
    period_combo_->addItems({tr("Yearly")});
    period_combo_->setToolTip(tr("UN Comtrade annual flow is wired first; monthly view can be added next."));
    hl->addWidget(period_combo_);

    year_combo_ = new QComboBox;
    year_combo_->setStyleSheet(combo_ss());
    year_combo_->setFixedWidth(82);
    const int current_year = QDate::currentDate().year();
    for (int y = std::min(current_year - 1, 2025); y >= 2010; --y)
        year_combo_->addItem(QString::number(y));
    const int y2024 = year_combo_->findText(QStringLiteral("2024"));
    if (y2024 >= 0)
        year_combo_->setCurrentIndex(y2024);
    hl->addWidget(year_combo_);

    refresh_btn_ = new QPushButton(tr("REFRESH"));
    refresh_btn_->setStyleSheet(button_ss());
    connect(refresh_btn_, &QPushButton::clicked, this, &TradeVizScreen::load_trade_data);
    hl->addWidget(refresh_btn_);

    status_label_ = new QLabel(tr("STATIC"));
    status_label_->setStyleSheet(QString("color: %1; border: 1px solid %2; padding: 3px 8px; font-size: 11px;"
                                         "font-weight: 800; background: %3; font-family: %4;")
                                     .arg(ui::colors::TEXT_SECONDARY(), ui::colors::BORDER_DIM(),
                                          ui::colors::BG_BASE(), FONT));
    hl->addWidget(status_label_);

    hl->addStretch();

    clock_label_ = new QLabel(QDateTime::currentDateTime().toString("HH:mm:ss"));
    clock_label_->setStyleSheet(QString("color: %1; font-size: 11px; background: transparent; font-family: %2;")
                                    .arg(ui::colors::TEXT_TERTIARY(), FONT));
    hl->addWidget(clock_label_);

    auto on_filter = [this]() {
        openmarketterminal::ScreenStateManager::instance().notify_changed(this);
        populate_partner_table();
        update_intelligence_panel();
    };
    connect(country_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, on_filter);
    connect(commodity_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, on_filter);
    connect(order_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, on_filter);
    connect(period_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, on_filter);
    connect(year_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, on_filter);

    return bar;
}

QWidget* TradeVizScreen::build_partner_table() {
    auto* panel = new QWidget(this);
    panel->setStyleSheet(QString("background: %1;").arg(ui::colors::BG_BASE()));
    auto* vl = new QVBoxLayout(panel);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    partner_table_ = new QTableWidget;
    partner_table_->setStyleSheet(table_ss());
    partner_table_->setColumnCount(6);
    partner_table_->setHorizontalHeaderLabels({tr("#"), tr("Partner"), tr("Imports"), tr("Exports"), tr("Balance"), tr("YoY")});
    partner_table_->verticalHeader()->setVisible(false);
    partner_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    partner_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    partner_table_->setShowGrid(false);
    partner_table_->setAlternatingRowColors(true);
    partner_table_->setStyleSheet(partner_table_->styleSheet() +
                                  QString(" QTableWidget { alternate-background-color: %1; }").arg(ui::colors::BG_SURFACE()));
    auto* hdr = partner_table_->horizontalHeader();
    hdr->setSectionResizeMode(0, QHeaderView::Fixed);
    hdr->resizeSection(0, 32);
    hdr->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int c = 2; c < 6; ++c)
        hdr->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    partner_table_->verticalHeader()->setDefaultSectionSize(28);
    connect(partner_table_, &QTableWidget::itemSelectionChanged, this, [this]() {
        if (!partner_table_)
            return;
        const auto items = partner_table_->selectedItems();
        if (items.isEmpty())
            return;
        select_partner(items.first()->data(Qt::UserRole).toString(), false);
    });
    vl->addWidget(partner_table_, 1);
    return panel;
}

QWidget* TradeVizScreen::build_intelligence_panel() {
    auto* panel = new QWidget(this);
    panel->setStyleSheet(QString("background: %1; border-left: 1px solid %2;")
                             .arg(ui::colors::BG_BASE(), ui::colors::BORDER_DIM()));
    auto* vl = new QVBoxLayout(panel);
    vl->setContentsMargins(10, 8, 10, 8);
    vl->setSpacing(8);

    summary_label_ = new QLabel(tr("Trade intelligence will load from UN Comtrade or local fallback."));
    summary_label_->setWordWrap(true);
    summary_label_->setStyleSheet(QString("color: %1; font-size: 12px; font-weight: 800; font-family: %2;")
                                      .arg(ui::colors::TEXT_PRIMARY(), FONT));
    vl->addWidget(summary_label_);

    auto* title = new QLabel(tr("MARKET IMPACT"));
    title->setStyleSheet(QString("color: %1; font-size: 13px; font-weight: 900; font-family: %2;")
                             .arg(ui::colors::AMBER(), FONT));
    vl->addWidget(title);

    insights_text_ = new QTextEdit(panel);
    insights_text_->setReadOnly(true);
    insights_text_->setMinimumWidth(330);
    insights_text_->setStyleSheet(QString("QTextEdit { background: %1; color: %2; border: 1px solid %3;"
                                          "font-size: 12px; padding: 8px; font-family: %4; }")
                                      .arg(ui::colors::BG_SURFACE(), ui::colors::TEXT_PRIMARY(),
                                           ui::colors::BORDER_DIM(), FONT));
    vl->addWidget(insights_text_, 1);

    return panel;
}

void TradeVizScreen::populate_partner_table() {
    if (!partner_table_)
        return;
    QVector<TradeVizPartner> rows = partners_;
    const int order = order_combo_ ? order_combo_->currentIndex() : 0;
    std::sort(rows.begin(), rows.end(), [order](const TradeVizPartner& a, const TradeVizPartner& b) {
        switch (order) {
        case 1: return a.imports_m > b.imports_m;
        case 2: return a.exports_m > b.exports_m;
        case 3: return a.balance_m > b.balance_m;
        case 4:
            if (!has_yoy(a.yoy_pct)) return false;
            if (!has_yoy(b.yoy_pct)) return true;
            return a.yoy_pct > b.yoy_pct;
        default: return a.total_m > b.total_m;
        }
    });

    partner_table_->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        const QStringList values = {
            QString::number(i + 1),
            QString("%1  %2").arg(r.iso3, r.name),
            money_m(r.imports_m),
            money_m(r.exports_m),
            QString("%1%2").arg(r.balance_m >= 0 ? "+" : "").arg(money_m(r.balance_m)),
            has_yoy(r.yoy_pct) ? QString("%1%2%").arg(r.yoy_pct >= 0 ? "+" : "").arg(r.yoy_pct, 0, 'f', 1) : QString("--"),
        };
        for (int c = 0; c < values.size(); ++c) {
            auto* item = new QTableWidgetItem(values[c]);
            item->setFont(QFont(FONT, 12, c == 1 ? QFont::Bold : QFont::Normal));
            if (c == 1)
                item->setForeground(QColor(ui::colors::POSITIVE()));
            else if ((c == 4 || c == 5) && values[c].startsWith('+'))
                item->setForeground(QColor(ui::colors::POSITIVE()));
            else if (c == 4 || c == 5)
                item->setForeground(QColor(ui::colors::NEGATIVE()));
            else
                item->setForeground(QColor(ui::colors::TEXT_PRIMARY()));
            if (c != 1)
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            item->setData(Qt::UserRole, r.iso3);
            partner_table_->setItem(i, c, item);
        }
    }

    if (!selected_partner_iso_.isEmpty()) {
        bool found = false;
        for (const auto& row : rows) {
            if (row.iso3 == selected_partner_iso_) {
                found = true;
                break;
            }
        }
        if (!found)
            selected_partner_iso_.clear();
    }

    if (auto* chord = static_cast<TradeFlowChordWidget*>(chord_widget_.data())) {
        chord->set_partners(rows, current_reporter());
        chord->set_selected_iso(selected_partner_iso_);
    }
}

void TradeVizScreen::select_partner(const QString& iso3, bool sync_table) {
    selected_partner_iso_ = iso3;
    if (auto* chord = static_cast<TradeFlowChordWidget*>(chord_widget_.data()))
        chord->set_selected_iso(selected_partner_iso_);

    if (sync_table && partner_table_) {
        const QSignalBlocker blocker(partner_table_);
        partner_table_->clearSelection();
        for (int row = 0; row < partner_table_->rowCount(); ++row) {
            auto* item = partner_table_->item(row, 0);
            if (item && item->data(Qt::UserRole).toString() == iso3) {
                partner_table_->selectRow(row);
                partner_table_->scrollToItem(item, QAbstractItemView::PositionAtCenter);
                break;
            }
        }
    }

    update_intelligence_panel();
    openmarketterminal::ScreenStateManager::instance().notify_changed(this);
}

QString TradeVizScreen::current_reporter() const {
    if (!country_combo_)
        return QStringLiteral("USA");
    const QString value = country_combo_->currentData().toString();
    return value.isEmpty() ? QStringLiteral("USA") : value;
}

QString TradeVizScreen::current_commodity() const {
    if (!commodity_combo_)
        return QStringLiteral("TOTAL");
    const QString value = commodity_combo_->currentData().toString();
    return value.isEmpty() ? QStringLiteral("TOTAL") : value;
}

int TradeVizScreen::current_year() const {
    return year_combo_ ? year_combo_->currentText().toInt() : 2024;
}

void TradeVizScreen::load_trade_data() {
    if (!python::PythonRunner::instance().is_available()) {
        apply_fallback_data(tr("Python environment is not ready."));
        return;
    }

    if (refresh_btn_) {
        refresh_btn_->setEnabled(false);
        refresh_btn_->setText(tr("LOADING"));
    }
    if (status_label_) {
        status_label_->setText(tr("LOADING"));
        status_label_->setStyleSheet(status_label_->styleSheet() + QString("color:%1;").arg(ui::colors::AMBER()));
    }

    const QStringList args = {
        QStringLiteral("flow"),
        QStringLiteral("--reporter"), current_reporter(),
        QStringLiteral("--year"), QString::number(current_year()),
        QStringLiteral("--commodity"), current_commodity(),
        QStringLiteral("--limit"), QStringLiteral("15"),
        QStringLiteral("--json"),
    };

    QPointer<TradeVizScreen> self = this;
    python::PythonRunner::instance().run("trade_intelligence.py", args, [self](const python::PythonResult& result) {
        if (!self)
            return;
        if (self->refresh_btn_) {
            self->refresh_btn_->setEnabled(true);
            self->refresh_btn_->setText(self->tr("REFRESH"));
        }
        if (!result.success) {
            self->apply_fallback_data(result.error.isEmpty() ? self->tr("Trade intelligence script failed.") : result.error);
            return;
        }

        QJsonParseError err;
        const QString json = python::extract_json(result.output);
        const auto doc = QJsonDocument::fromJson(json.toUtf8(), &err);
        if (!err.errorString().isEmpty() && err.error != QJsonParseError::NoError) {
            self->apply_fallback_data(self->tr("Invalid trade intelligence JSON: %1").arg(err.errorString()));
            return;
        }
        if (!doc.isObject()) {
            self->apply_fallback_data(self->tr("Trade intelligence returned no object."));
            return;
        }
        self->apply_payload(doc.object());
    });
}

void TradeVizScreen::apply_payload(const QJsonObject& payload) {
    QVector<TradeVizPartner> rows;
    const QJsonArray arr = payload.value("partners").toArray();
    rows.reserve(arr.size());
    for (const QJsonValue& value : arr) {
        const QJsonObject obj = value.toObject();
        TradeVizPartner row;
        row.name = obj.value("partner").toString();
        row.iso3 = obj.value("iso3").toString();
        row.imports_m = obj.value("imports_m").toDouble();
        row.exports_m = obj.value("exports_m").toDouble();
        row.total_m = obj.value("total_m").toDouble(row.imports_m + row.exports_m);
        row.balance_m = obj.value("balance_m").toDouble(row.exports_m - row.imports_m);
        row.yoy_pct = obj.value("yoy_pct").isNull() ? std::numeric_limits<double>::quiet_NaN()
                                                     : obj.value("yoy_pct").toDouble();
        if (is_india_partner(row.iso3, row.name))
            continue;
        row.name = normalize_partner_name(row.iso3, row.name);
        rows.append(row);
    }
    if (rows.isEmpty()) {
        apply_fallback_data(tr("No trade partner rows in response."));
        return;
    }

    bool has_turkiye = false;
    for (const auto& row : rows) {
        if (row.iso3.compare(QLatin1String("TUR"), Qt::CaseInsensitive) == 0) {
            has_turkiye = true;
            break;
        }
    }
    if (!has_turkiye) {
        for (const auto& sample : fallback_partners()) {
            if (sample.iso3.compare(QLatin1String("TUR"), Qt::CaseInsensitive) == 0) {
                rows.last() = sample;
                break;
            }
        }
    }

    partners_ = rows;
    source_status_ = payload.value("status").toString(QStringLiteral("LIVE"));
    source_name_ = payload.value("source").toString(QStringLiteral("UN Comtrade"));
    if (status_label_) {
        const bool live = source_status_ == QLatin1String("LIVE");
        status_label_->setText(source_status_);
        status_label_->setStyleSheet(QString("color: %1; border: 1px solid %2; padding: 3px 8px; font-size: 11px;"
                                             "font-weight: 800; background: %3; font-family: %4;")
                                         .arg(live ? ui::colors::POSITIVE() : ui::colors::AMBER(),
                                              live ? ui::colors::POSITIVE() : ui::colors::AMBER(),
                                              ui::colors::BG_BASE(), FONT));
    }
    if (provenance_note_) {
        const QString error = payload.value("error").toString();
        provenance_note_->setText(error.isEmpty()
                                      ? tr("%1 data from %2. Filters are active; Refresh reloads this view.")
                                            .arg(source_status_, source_name_)
                                      : tr("%1 via %2. Reason: %3").arg(source_status_, source_name_, error.left(220)));
    }
    populate_partner_table();
    update_intelligence_panel(payload);
    openmarketterminal::ScreenStateManager::instance().notify_changed(this);
}

void TradeVizScreen::apply_fallback_data(const QString& reason) {
    partners_ = fallback_partners();
    source_status_ = "FALLBACK";
    source_name_ = "bundled sample";
    if (status_label_)
        status_label_->setText(tr("FALLBACK"));
    if (provenance_note_)
        provenance_note_->setText(tr("Fallback sample shown. Reason: %1").arg(reason.left(220)));
    populate_partner_table();
    update_intelligence_panel();
}

void TradeVizScreen::update_intelligence_panel(const QJsonObject& payload) {
    if (!summary_label_ || !insights_text_)
        return;
    if (active_tab_ == 3) {
        update_sources_panel();
        return;
    }

    double imports = 0.0;
    double exports = 0.0;
    for (const auto& row : partners_) {
        imports += row.imports_m;
        exports += row.exports_m;
    }
    summary_label_->setText(tr("%1 %2 %3 | imports %4 | exports %5 | balance %6")
                                .arg(current_reporter())
                                .arg(current_year())
                                .arg(commodity_combo_ ? commodity_combo_->currentText() : QStringLiteral("All Goods"))
                                .arg(money_m(imports))
                                .arg(money_m(exports))
                                .arg(QString("%1%2").arg(exports >= imports ? "+" : "").arg(money_m(exports - imports))));

    QStringList bullets;
    const QJsonArray insights = payload.value("insights").toArray();
    for (const QJsonValue& v : insights) {
        const QString line = v.toString();
        if (line.contains(QLatin1String("India"), Qt::CaseInsensitive))
            continue;
        bullets << line;
    }
    if (bullets.isEmpty() && !partners_.isEmpty()) {
        auto rows = partners_;
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.total_m > b.total_m; });
        bullets << tr("Top route: %1 at %2 total trade.").arg(rows.first().name, money_m(rows.first().total_m));
        bullets << tr("Watch tariff, FX, shipping, and sector headlines tied to the largest routes.");
        bullets << tr("Use commodity filters to narrow the signal from broad macro noise to tradable supply-chain themes.");
        if (source_status_ != QLatin1String("LIVE"))
            bullets << tr("Live source did not answer; this is a local fallback sample, clearly marked.");
    }

    QString text;
    if (!selected_partner_iso_.isEmpty()) {
        for (const auto& row : partners_) {
            if (row.iso3 == selected_partner_iso_) {
                text += tr("SELECTED ROUTE\n");
                text += tr("%1 -> %2 %3\n").arg(current_reporter(), row.iso3, row.name);
                text += tr("Imports: %1 | Exports: %2 | Balance: %3 | Total: %4\n")
                            .arg(money_m(row.imports_m))
                            .arg(money_m(row.exports_m))
                            .arg(QString("%1%2").arg(row.balance_m >= 0 ? "+" : "").arg(money_m(row.balance_m)))
                            .arg(money_m(row.total_m));
                if (has_yoy(row.yoy_pct))
                    text += tr("YoY: %1%2%\n").arg(row.yoy_pct >= 0 ? "+" : "").arg(row.yoy_pct, 0, 'f', 1);
                text += tr("Use this route to watch tariff, FX, shipping, sector, and earnings exposure.\n\n");
                break;
            }
        }
    }
    for (const QString& b : bullets)
        text += QStringLiteral("- ") + b + QStringLiteral("\n\n");
    insights_text_->setPlainText(text.trimmed());
}

void TradeVizScreen::update_sources_panel() {
    if (!summary_label_ || !insights_text_)
        return;
    summary_label_->setText(tr("Source status: %1 | provider: %2 | reporter %3 | year %4 | commodity %5")
                                .arg(source_status_, source_name_, current_reporter())
                                .arg(current_year())
                                .arg(current_commodity()));

    QString text;
    text += tr("ACTIVE SOURCE\n");
    text += tr("%1 via %2\n\n").arg(source_status_, source_name_);
    text += tr("WHAT THIS VIEW USES\n");
    text += tr("- UN Comtrade annual country/commodity trade flows when the public endpoint answers.\n");
    text += tr("- Local bundled sample when the live source is unavailable or rate-limited.\n");
    text += tr("- The status badge and note above tell you which path is active.\n\n");
    text += tr("CURRENT FILTERS\n");
    text += tr("- Reporter: %1\n").arg(current_reporter());
    text += tr("- Year: %1\n").arg(current_year());
    text += tr("- Commodity: %1\n").arg(current_commodity());
    text += tr("- Partners loaded: %1\n\n").arg(partners_.size());
    text += tr("INTERACTION\n");
    text += tr("- Flow: hover/click nodes to inspect a route.\n");
    text += tr("- Double-click a supported country node to pivot the whole flower.\n");
    text += tr("- Table: selects the same route as the flower.\n");
    insights_text_->setPlainText(text.trimmed());
}

void TradeVizScreen::set_active_tab(int index) {
    active_tab_ = std::clamp(index, 0, 3);
    update_tab_styles();

    const bool flow = active_tab_ == 0;
    const bool table = active_tab_ == 1;
    const bool impact = active_tab_ == 2;
    const bool sources = active_tab_ == 3;

    if (chord_widget_)
        chord_widget_->setVisible(flow);
    if (partner_panel_)
        partner_panel_->setVisible(flow || table);
    if (intelligence_panel_)
        intelligence_panel_->setVisible(flow || impact || sources);

    if (main_splitter_) {
        if (flow)
            main_splitter_->setSizes({width() * 5 / 8, width() * 3 / 8});
        else
            main_splitter_->setSizes({0, width()});
    }
    if (right_splitter_) {
        if (flow)
            right_splitter_->setSizes({height() * 3 / 5, height() * 2 / 5});
        else if (table)
            right_splitter_->setSizes({height(), 0});
        else
            right_splitter_->setSizes({0, height()});
    }

    if (sources)
        update_sources_panel();
    else
        update_intelligence_panel();
}

void TradeVizScreen::update_tab_styles() {
    for (int i = 0; i < tab_buttons_.size(); ++i) {
        const bool active = i == active_tab_;
        tab_buttons_[i]->setStyleSheet(QString(
            "QPushButton { color: %1; background: %2; border: none; border-bottom: 2px solid %3;"
            "  font-size: 12px; font-weight: 900; padding: 0 14px; font-family: %4; text-align: left; }"
            "QPushButton:hover { color: %5; background: %6; }")
            .arg(active ? ui::colors::AMBER() : ui::colors::TEXT_PRIMARY(),
                 active ? ui::colors::BG_RAISED() : ui::colors::BG_BASE(),
                 active ? ui::colors::AMBER() : ui::colors::BG_BASE(),
                 FONT,
                 ui::colors::AMBER(),
                 ui::colors::BG_HOVER()));
    }
}

void TradeVizScreen::update_clock() {
    if (clock_label_)
        clock_label_->setText(QDateTime::currentDateTime().toString("HH:mm:ss"));
    animation_phase_ += 0.11;
    if (auto* chord = static_cast<TradeFlowChordWidget*>(chord_widget_.data()))
        chord->set_phase(animation_phase_);
}

void TradeVizScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (clock_timer_)
        clock_timer_->start();
    if (animation_timer_)
        animation_timer_->start();
}

void TradeVizScreen::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (clock_timer_)
        clock_timer_->stop();
    if (animation_timer_)
        animation_timer_->stop();
}

TradeVizScreen::TradeVizScreen(QWidget* parent) : QWidget(parent) {
    partners_ = fallback_partners();
    setup_ui();
    populate_partner_table();
    update_intelligence_panel();

    clock_timer_ = new QTimer(this);
    clock_timer_->setInterval(1000);
    connect(clock_timer_, &QTimer::timeout, this, &TradeVizScreen::update_clock);

    animation_timer_ = new QTimer(this);
    animation_timer_->setInterval(90);
    connect(animation_timer_, &QTimer::timeout, this, [this]() {
        animation_phase_ += 0.055;
        if (auto* chord = static_cast<TradeFlowChordWidget*>(chord_widget_.data()))
            chord->set_phase(animation_phase_);
    });

    QTimer::singleShot(250, this, &TradeVizScreen::load_trade_data);
}

void TradeVizScreen::setup_ui() {
    setObjectName("tradeVizScreen");
    setStyleSheet(QString("QWidget#tradeVizScreen { background: %1; }").arg(ui::colors::BG_BASE()));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(build_tab_bar());
    root->addWidget(build_filter_bar());

    provenance_note_ = new QLabel(
        tr("Loading trade intelligence. Live path uses UN Comtrade; fallback sample remains local and clearly marked."));
    provenance_note_->setWordWrap(true);
    provenance_note_->setStyleSheet(QString("color: %1; background: %2; padding: 5px 12px; font-size: 11px; font-family: %3;")
                                        .arg(ui::colors::TEXT_TERTIARY(), ui::colors::BG_RAISED(), FONT));
    root->addWidget(provenance_note_);

    auto* splitter = new QSplitter(Qt::Horizontal);
    main_splitter_ = splitter;
    splitter->setStyleSheet(QString("QSplitter { background: %1; } QSplitter::handle { background: %2; width: 1px; }")
                                .arg(ui::colors::BG_BASE(), ui::colors::BORDER_DIM()));

    auto* chord = new TradeFlowChordWidget;
    chord->on_partner_selected = [this](const QString& iso3) { select_partner(iso3); };
    chord->on_partner_activated = [this](const QString& iso3) {
        if (!country_combo_)
            return;
        const int idx = country_combo_->findData(iso3);
        if (idx < 0) {
            if (provenance_note_)
                provenance_note_->setText(tr("%1 is selected. Add it to the country filter list before pivoting the whole flower.").arg(iso3));
            select_partner(iso3);
            return;
        }
        country_combo_->setCurrentIndex(idx);
        selected_partner_iso_.clear();
        load_trade_data();
    };
    chord_widget_ = chord;
    splitter->addWidget(chord);

    auto* right = new QSplitter(Qt::Vertical);
    right_splitter_ = right;
    right->setStyleSheet(splitter->styleSheet());
    partner_panel_ = build_partner_table();
    intelligence_panel_ = build_intelligence_panel();
    right->addWidget(partner_panel_);
    right->addWidget(intelligence_panel_);
    right->setStretchFactor(0, 3);
    right->setStretchFactor(1, 2);
    splitter->addWidget(right);

    splitter->setStretchFactor(0, 5);
    splitter->setStretchFactor(1, 3);
    root->addWidget(splitter, 1);
    set_active_tab(active_tab_);
}

void TradeVizScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void TradeVizScreen::retranslateUi() {
    if (tab_buttons_.size() == 4) {
        tab_buttons_[0]->setText(tr("21) Flow"));
        tab_buttons_[1]->setText(tr("22) Table"));
        tab_buttons_[2]->setText(tr("23) Impact"));
        tab_buttons_[3]->setText(tr("24) Sources"));
    }
    if (flow_title_)
        flow_title_->setText(tr("Trade Intelligence"));
    if (refresh_btn_)
        refresh_btn_->setText(tr("REFRESH"));
    if (order_caption_)
        order_caption_->setText(tr("Rank"));
    if (period_caption_)
        period_caption_->setText(tr("Period"));
    if (partner_table_)
        partner_table_->setHorizontalHeaderLabels({tr("#"), tr("Partner"), tr("Imports"), tr("Exports"), tr("Balance"), tr("YoY")});
    update_intelligence_panel();
    if (chord_widget_)
        chord_widget_->update();
}

QVariantMap TradeVizScreen::save_state() const {
    QVariantMap s;
    if (country_combo_)
        s.insert("country", country_combo_->currentIndex());
    if (commodity_combo_)
        s.insert("commodity", commodity_combo_->currentIndex());
    if (order_combo_)
        s.insert("order", order_combo_->currentIndex());
    if (period_combo_)
        s.insert("period", period_combo_->currentIndex());
    if (year_combo_)
        s.insert("year", year_combo_->currentIndex());
    s.insert("active_tab", active_tab_);
    return s;
}

void TradeVizScreen::restore_state(const QVariantMap& state) {
    auto apply = [&](QComboBox* box, const char* key) {
        if (!box)
            return;
        const int v = state.value(key, -1).toInt();
        if (v >= 0 && v < box->count())
            box->setCurrentIndex(v);
    };
    apply(country_combo_, "country");
    apply(commodity_combo_, "commodity");
    apply(order_combo_, "order");
    apply(period_combo_, "period");
    apply(year_combo_, "year");
    set_active_tab(state.value("active_tab", active_tab_).toInt());
}

} // namespace openmarketterminal::screens
