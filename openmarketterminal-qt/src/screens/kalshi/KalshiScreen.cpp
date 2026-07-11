#include "screens/kalshi/KalshiScreen.h"

#include "services/prediction/PredictionExchangeAdapter.h"
#include "services/prediction/PredictionExchangeRegistry.h"
#include "services/prediction/kalshi/KalshiAdapter.h"
#include "services/prediction/kalshi/KalshiEvidenceEngine.h"
#include "screens/crypto_trading/CryptoOrderBook.h"
#include "trading/ExchangeService.h"
#include "trading/ExchangeSessionManager.h"
#include "ui/theme/Theme.h"

#include <QComboBox>
#include <QAbstractItemView>
#include <QAbstractSocket>
#include <QColor>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QSplitterHandle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QUrl>
#include <QWebSocket>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <cmath>
#include <functional>

namespace openmarketterminal::screens::kalshi {
namespace pred = openmarketterminal::services::prediction;
namespace kalshi_data = openmarketterminal::services::prediction::kalshi_ns;
using namespace openmarketterminal::ui;

namespace {
QString money(double value) { return QStringLiteral("$%1").arg(value, 0, 'f', 2); }
QString probability(double value) { return QStringLiteral("%1%").arg(std::round(value * 100.0), 0, 'f', 0); }
QString contract_quote(double value) {
    return QStringLiteral("%1 · $%2").arg(probability(value)).arg(value, 0, 'f', 2);
}
double outcome_price(const pred::PredictionMarket& market, int index) {
    return index >= 0 && index < market.outcomes.size() ? market.outcomes[index].price : 0.0;
}
QString contract_label(const pred::PredictionMarket& market) {
    const QString yes = market.extras.value(QStringLiteral("yes_sub_title")).toString().trimmed();
    const QString no = market.extras.value(QStringLiteral("no_sub_title")).toString().trimmed();
    if (!yes.isEmpty()) return yes;
    if (!no.isEmpty()) return no;
    return market.question;
}
double lane_score(const pred::PredictionMarket& market) {
    const double yes = outcome_price(market, 0);
    const double no = outcome_price(market, 1);
    const bool eligible = (yes >= 0.85 && yes <= 0.92) || (no >= 0.85 && no <= 0.92);
    if (eligible) return 1000.0 + market.volume;
    const double favorite = qMax(yes, no);
    return 100.0 - std::abs(favorite - 0.885) * 100.0 + market.volume / 1000000.0;
}
bool in_selected_time_horizon(const pred::PredictionMarket& market, const QString& cadence) {
    const QDateTime close = QDateTime::fromString(market.end_date_iso, Qt::ISODate);
    if (!close.isValid()) return true;
    const qint64 remaining = QDateTime::currentDateTimeUtc().secsTo(close.toUTC());
    if (remaining < -300) return false;
    if (cadence == QStringLiteral("fifteen_min")) return remaining <= 30 * 60;
    if (cadence == QStringLiteral("hourly")) return remaining <= 2 * 60 * 60;
    if (cadence == QStringLiteral("daily")) return remaining <= 36 * 60 * 60;
    if (cadence == QStringLiteral("weekly")) return remaining <= 8 * 24 * 60 * 60;
    return true;
}
QVector<double> contract_bounds(const pred::PredictionMarket& market) {
    const QString label = contract_label(market);
    if (!label.contains(QLatin1Char('$')) && !label.contains(QStringLiteral("above"), Qt::CaseInsensitive)
        && !label.contains(QStringLiteral(" to "), Qt::CaseInsensitive)) return {};
    static const QRegularExpression number(QStringLiteral(R"((\d[\d,]*(?:\.\d+)?))"));
    QVector<double> values;
    auto match = number.globalMatch(label);
    while (match.hasNext() && values.size() < 2) {
        QString token = match.next().captured(1);
        token.remove(QLatin1Char(','));
        bool ok = false;
        const double value = token.toDouble(&ok);
        if (ok && value > 0.0) values.append(value);
    }
    return values;
}
QString duration_text(qint64 seconds) {
    if (seconds <= 0) return QStringLiteral("CLOSED");
    const qint64 days = seconds / 86400;
    seconds %= 86400;
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 secs = seconds % 60;
    if (days > 0) return QStringLiteral("%1d %2h").arg(days).arg(hours);
    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
}
struct VenueSnapshot {
    QString venue;
    double bid = 0.0;
    double ask = 0.0;
    double mid = 0.0;
    double bid_notional = 0.0;
    double ask_notional = 0.0;
    double imbalance = 0.0;
    bool valid = false;
};

VenueSnapshot summarize_venue_book(const QString& venue,
                                   const openmarketterminal::trading::OrderBookData& book) {
    VenueSnapshot snapshot;
    snapshot.venue = venue.toUpper();
    snapshot.bid = book.best_bid > 0.0 ? book.best_bid
                                        : (book.bids.isEmpty() ? 0.0 : book.bids.first().first);
    snapshot.ask = book.best_ask > 0.0 ? book.best_ask
                                        : (book.asks.isEmpty() ? 0.0 : book.asks.first().first);
    snapshot.mid = snapshot.bid > 0.0 && snapshot.ask > 0.0
        ? (snapshot.bid + snapshot.ask) / 2.0
        : qMax(snapshot.bid, snapshot.ask);
    for (const auto& level : book.bids) snapshot.bid_notional += level.first * level.second;
    for (const auto& level : book.asks) snapshot.ask_notional += level.first * level.second;
    const double total = snapshot.bid_notional + snapshot.ask_notional;
    snapshot.imbalance = total > 0.0 ? (snapshot.bid_notional - snapshot.ask_notional) / total : 0.0;
    snapshot.valid = snapshot.mid > 0.0 && total > 0.0;
    return snapshot;
}

openmarketterminal::trading::OrderBookData fetch_public_book(
    openmarketterminal::trading::ExchangeSession* session, const QString& symbol) {
    openmarketterminal::trading::OrderBookData book;
    if (!session) return book;
    try {
        book = session->fetch_orderbook(symbol, 30);
    } catch (...) {
        // Keep the Kalshi UI useful when one public venue is temporarily unavailable.
    }
    if (book.bids.isEmpty() && book.asks.isEmpty() && symbol.endsWith(QStringLiteral("/USD"))) {
        try {
            book = session->fetch_orderbook(symbol + QStringLiteral("T"), 30);
        } catch (...) {
            // Some exchanges publish depth only against USDT.
        }
    }
    return book;
}

void append_venue_feature_snapshot(const QJsonObject& row) {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(directory);
    QFile file(directory + QStringLiteral("/kalshi-venue-features.jsonl"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    file.write(QJsonDocument(row).toJson(QJsonDocument::Compact));
    file.write("\n");
}

QPushButton* segment(const QString& label, QWidget* parent) {
    auto* button = new QPushButton(label, parent);
    button->setCheckable(true);
    button->setFixedHeight(28);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}
}

class CompactTableWidget final : public QTableWidget {
  public:
    using QTableWidget::QTableWidget;
    QSize sizeHint() const override { return QSize(520, 260); }
    QSize minimumSizeHint() const override { return QSize(120, 120); }
};

class KalshiSimplePlot final : public QWidget {
  public:
    explicit KalshiSimplePlot(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(180);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void set_symbol(const QString& symbol) { symbol_ = symbol; update(); }
    void set_targets(const QVector<double>& targets) { targets_ = targets; update(); }
    void set_probability_mode(bool enabled) { probability_mode_ = enabled; update(); }
    void set_window_seconds(qint64 seconds) { window_seconds_ = seconds; update(); }
    void set_candles(const QVector<openmarketterminal::trading::Candle>& candles) {
        candles_ = candles;
        if (candles_.size() > 2'400) candles_.remove(0, candles_.size() - 2'400);
        update();
    }
    void append_candle(const openmarketterminal::trading::Candle& candle) {
        if (!candles_.isEmpty() && candles_.last().timestamp == candle.timestamp)
            candles_.last() = candle;
        else
            candles_.append(candle);
        if (candles_.size() > 2'400) candles_.remove(0, candles_.size() - 2'400);
        update();
    }
    void clear() { candles_.clear(); update(); }
    bool has_data() const { return !candles_.isEmpty(); }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor(colors::BG_BASE()));

        const QRectF plot(12.0, 8.0, qMax(1, width() - 102), qMax(1, height() - 34));
        painter.setFont(QFont(QStringLiteral("Consolas"), 9));

        QVector<openmarketterminal::trading::Candle> points;
        if (window_seconds_ > 0 && !candles_.isEmpty()) {
            const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() - window_seconds_ * 1'000;
            for (const auto& candle : candles_) {
                if (candle.timestamp >= cutoff) points.append(candle);
            }
        }
        if (points.isEmpty()) points = candles_;
        if (points.isEmpty()) {
            painter.setPen(QColor(colors::TEXT_SECONDARY()));
            painter.drawText(plot, Qt::AlignCenter,
                             QStringLiteral("WAITING FOR %1 PRICE").arg(symbol_));
            return;
        }

        double minimum = points.first().close;
        double maximum = minimum;
        for (const auto& candle : points) {
            minimum = qMin(minimum, candle.close);
            maximum = qMax(maximum, candle.close);
        }
        for (double target : targets_) {
            if (target > 0.0) {
                minimum = qMin(minimum, target);
                maximum = qMax(maximum, target);
            }
        }
        const double raw_span = qMax(0.000001, maximum - minimum);
        minimum -= raw_span * 0.14;
        maximum += raw_span * 0.14;
        if (std::abs(maximum - minimum) < 0.000001) {
            minimum *= 0.999;
            maximum *= 1.001;
        }

        auto y_for = [&](double price) {
            return plot.bottom() - (price - minimum) / (maximum - minimum) * plot.height();
        };
        auto x_for = [&](int index) {
            if (points.size() <= 1) return plot.right();
            return plot.left() + (double(index) / double(points.size() - 1)) * plot.width();
        };

        QPen grid{QColor(colors::BORDER_DIM())};
        grid.setStyle(Qt::DotLine);
        painter.setPen(grid);
        for (int i = 0; i < 5; ++i) {
            const double fraction = double(i) / 4.0;
            const double y = plot.top() + fraction * plot.height();
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            const double price = maximum - fraction * (maximum - minimum);
            painter.setPen(QColor(colors::TEXT_SECONDARY()));
            const QString axis = probability_mode_
                ? QStringLiteral("%1%").arg(price * 100.0, 0, 'f', 0)
                : QStringLiteral("$%1").arg(price, 0, price >= 1000.0 ? 'f' : 'g', price >= 1000.0 ? 2 : 5);
            painter.drawText(QRectF(plot.right() + 8, y - 9, 82, 18), Qt::AlignLeft | Qt::AlignVCenter,
                             axis);
            painter.setPen(grid);
        }

        QPen target_pen(QColor(colors::POSITIVE()), 2);
        target_pen.setStyle(Qt::DashLine);
        painter.setPen(target_pen);
        if (targets_.size() == 1 && targets_.first() > 0.0) {
            const double y = y_for(targets_.first());
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.drawText(QRectF(plot.left() + 6, y - 22, 210, 20), Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("TO BEAT  $%1").arg(targets_.first(), 0, 'f', 2));
        } else if (targets_.size() > 1) {
            const double y1 = y_for(targets_[0]);
            const double y2 = y_for(targets_[1]);
            QColor band(colors::POSITIVE());
            band.setAlpha(26);
            painter.fillRect(QRectF(plot.left(), qMin(y1, y2), plot.width(), std::abs(y2 - y1)), band);
            painter.drawLine(QPointF(plot.left(), y1), QPointF(plot.right(), y1));
            painter.drawLine(QPointF(plot.left(), y2), QPointF(plot.right(), y2));
        }

        QPainterPath path;
        path.moveTo(x_for(0), y_for(points.first().close));
        for (int i = 1; i < points.size(); ++i)
            path.lineTo(x_for(i), y_for(points[i].close));
        painter.setPen(QPen(QColor(QStringLiteral("#f7931a")), 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path);

        const double last_price = points.last().close;
        const QPointF latest(x_for(points.size() - 1), y_for(last_price));
        painter.setPen(QPen(QColor(QStringLiteral("#f7931a")), 2));
        painter.setBrush(QColor(QStringLiteral("#f7931a")));
        painter.drawEllipse(latest, 5, 5);
        painter.setPen(QColor(colors::TEXT_PRIMARY()));
        const QString latest_text = probability_mode_
            ? QStringLiteral("KALSHI  %1%").arg(last_price * 100.0, 0, 'f', 1)
            : QStringLiteral("NOW  $%1").arg(last_price, 0, 'f', 2);
        painter.drawText(QRectF(plot.right() - 180, plot.top() + 4, 172, 20), Qt::AlignRight | Qt::AlignVCenter,
                         latest_text);

        painter.setPen(QColor(colors::TEXT_SECONDARY()));
        const auto first_time = QDateTime::fromMSecsSinceEpoch(points.first().timestamp).toString(QStringLiteral("hh:mm:ss"));
        const auto last_time = QDateTime::fromMSecsSinceEpoch(points.last().timestamp).toString(QStringLiteral("hh:mm:ss"));
        painter.drawText(QRectF(plot.left(), plot.bottom() + 5, 100, 18), Qt::AlignLeft, first_time);
        painter.drawText(QRectF(plot.right() - 100, plot.bottom() + 5, 100, 18), Qt::AlignRight, last_time);
    }

  private:
    QString symbol_ = QStringLiteral("BTC/USD");
    QVector<double> targets_;
    QVector<openmarketterminal::trading::Candle> candles_;
    qint64 window_seconds_ = 90;
    bool probability_mode_ = false;
};

class KalshiSimpleChart final : public QWidget {
  public:
    explicit KalshiSimpleChart(QWidget* parent = nullptr) : QWidget(parent) {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        plot_ = new KalshiSimplePlot(this);
        layout->addWidget(plot_, 1);
        auto* timeframes = new QHBoxLayout;
        timeframes->setContentsMargins(0, 0, 0, 0);
        timeframes->setSpacing(4);
        const QStringList labels{QStringLiteral("live"), QStringLiteral("1m"), QStringLiteral("5m"),
                                 QStringLiteral("15m"), QStringLiteral("1h"), QStringLiteral("1d")};
        for (const auto& label : labels) {
            auto* button = new QPushButton(label.toUpper(), this);
            button->setCheckable(true);
            button->setFixedHeight(28);
            button->setProperty("timeframe", label);
            button->setChecked(label == QStringLiteral("live"));
            connect(button, &QPushButton::clicked, this, [this, button, label]() {
                for (auto* peer : findChildren<QPushButton*>()) peer->setChecked(peer == button);
                plot_->set_window_seconds(window_seconds_for(label));
                if (timeframe_changed_) timeframe_changed_(label);
            });
            timeframes->addWidget(button);
        }
        timeframes->addStretch();
        layout->addLayout(timeframes);
    }

    void set_timeframe_changed(std::function<void(const QString&)> callback) {
        timeframe_changed_ = std::move(callback);
    }
    void set_symbol(const QString& symbol) { plot_->set_symbol(symbol); }
    void set_targets(const QVector<double>& targets) { plot_->set_targets(targets); }
    void set_probability_mode(bool enabled) { plot_->set_probability_mode(enabled); }
    void set_candles(const QVector<openmarketterminal::trading::Candle>& candles) { plot_->set_candles(candles); }
    void append_candle(const openmarketterminal::trading::Candle& candle) { plot_->append_candle(candle); }
    void clear() { plot_->clear(); }
    bool has_data() const { return plot_->has_data(); }
    void set_timeframe(const QString& timeframe) { plot_->set_window_seconds(window_seconds_for(timeframe)); }

  private:
    static qint64 window_seconds_for(const QString& timeframe) {
        const QString value = timeframe.toLower();
        if (value == QStringLiteral("live")) return 90;
        if (value == QStringLiteral("1m")) return 60;
        if (value == QStringLiteral("5m")) return 5 * 60;
        if (value == QStringLiteral("15m")) return 15 * 60;
        if (value == QStringLiteral("1h")) return 60 * 60;
        if (value == QStringLiteral("1d")) return 24 * 60 * 60;
        return 90;
    }

    KalshiSimplePlot* plot_ = nullptr;
    std::function<void(const QString&)> timeframe_changed_;
};

KalshiScreen::KalshiScreen(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("kalshiWorkspace"));
    recorder_started_ms_ = QDateTime::currentMSecsSinceEpoch();
    build_ui();
    spot_dom_timer_ = new QTimer(this);
    spot_dom_timer_->setInterval(1'000);
    connect(spot_dom_timer_, &QTimer::timeout, this, &KalshiScreen::refresh_spot_dom);
    reference_dom_reconnect_timer_ = new QTimer(this);
    reference_dom_reconnect_timer_->setSingleShot(true);
    connect(reference_dom_reconnect_timer_, &QTimer::timeout, this, &KalshiScreen::start_spot_dom_stream);
    dom_timer_ = new QTimer(this);
    dom_timer_->setInterval(1500);
    connect(dom_timer_, &QTimer::timeout, this, &KalshiScreen::refresh_venue_consensus);
    connect(dom_timer_, &QTimer::timeout, this, &KalshiScreen::refresh_reference_chart);
    evidence_timer_ = new QTimer(this);
    evidence_timer_->setInterval(1'000);
    connect(evidence_timer_, &QTimer::timeout, this, &KalshiScreen::record_ladder_evidence);
    clock_timer_ = new QTimer(this);
    clock_timer_->setInterval(1'000);
    clock_timer_->setTimerType(Qt::PreciseTimer);
    connect(clock_timer_, &QTimer::timeout, this, [this]() {
        update_observation_strip();
        update_market_health();
    });
}

KalshiScreen::~KalshiScreen() {
    for (const auto& connection : connections_) QObject::disconnect(connection);
}

QSize KalshiScreen::sizeHint() const { return QSize(1120, 800); }
QSize KalshiScreen::minimumSizeHint() const { return QSize(760, 560); }

void KalshiScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    pred::PredictionExchangeRegistry::instance().set_active(QStringLiteral("kalshi"));
    if (first_show_) {
        first_show_ = false;
        wire_adapter();
    }
    dom_timer_->start();
    spot_dom_timer_->start();
    evidence_timer_->start();
    clock_timer_->start();
    start_spot_dom_stream();
    refresh_spot_dom();
    refresh_venue_consensus();
    QTimer::singleShot(0, this, &KalshiScreen::ensure_workspace_panes_visible);
    refresh();
}

void KalshiScreen::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    QTimer::singleShot(0, this, &KalshiScreen::ensure_workspace_panes_visible);
}

void KalshiScreen::ensure_workspace_panes_visible() {
    if (!workspace_splitter_ || !dom_panel_) return;
    const int total = workspace_splitter_->width();
    if (total <= 0) return;

    const QList<int> sizes = workspace_splitter_->sizes();
    const bool dom_visible = sizes.size() >= 3 && sizes[2] >= 300;
    const bool content_overflows = sizes.size() >= 3 && (sizes[0] + sizes[1] + sizes[2]) > total + 40;
    if (dom_visible && !content_overflows) return;

    const int right = qBound(300, total * 18 / 100, 420);
    const int left = qBound(240, total * 16 / 100, 340);
    const int center = qMax(120, total - left - right - workspace_splitter_->handleWidth() * 2);
    workspace_splitter_->setSizes({left, center, right});
    dom_panel_->show();
}

pred::PredictionExchangeAdapter* KalshiScreen::adapter() const {
    return pred::PredictionExchangeRegistry::instance().adapter(QStringLiteral("kalshi"));
}

void KalshiScreen::build_ui() {
    setStyleSheet(QStringLiteral(
        "#kalshiWorkspace { background:%1; color:%2; }"
        "QFrame#ksBand { background:%3; border-bottom:1px solid %4; }"
        "QComboBox,QLineEdit,QDoubleSpinBox,QSpinBox { background:%1; color:%2; border:1px solid %5; padding:5px 8px; }"
        "QPushButton { background:transparent; color:%6; border:1px solid %5; padding:5px 10px; font-weight:700; }"
        "QPushButton:hover { color:%2; border-color:%7; }"
        "QPushButton:checked { color:%8; border-color:%8; background:rgba(0,196,154,0.12); }"
        "QListWidget,QTableWidget,QTextEdit { background:%1; color:%2; border:0; gridline-color:%4; }"
        "QListWidget::item { border-bottom:1px solid %4; padding:10px; }"
        "QListWidget::item:selected { background:rgba(0,196,154,0.12); color:%2; border-left:3px solid %8; }"
        "QHeaderView::section { background:%3; color:%6; border:0; border-bottom:1px solid %4; padding:6px; font-weight:700; }"
    ).arg(colors::BG_BASE(), colors::TEXT_PRIMARY(), colors::BG_RAISED(), colors::BORDER_DIM(),
          colors::BORDER_MED(), colors::TEXT_SECONDARY(), colors::BORDER_BRIGHT(), colors::CYAN()));

    auto* root = new QVBoxLayout(this);
    root->setSizeConstraint(QLayout::SetNoConstraint);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* command = new QFrame(this);
    command->setObjectName(QStringLiteral("ksBand"));
    command->setFixedHeight(52);
    auto* command_layout = new QHBoxLayout(command);
    command_layout->setContentsMargins(12, 8, 12, 8);
    command_layout->setSpacing(8);
    auto* venue_badge = new QLabel(QStringLiteral("KALSHI"), command);
    venue_badge->setStyleSheet(QStringLiteral("color:%1;border:1px solid %1;padding:6px 10px;font-weight:900;").arg(colors::CYAN()));
    command_layout->addWidget(venue_badge);
    auto* title = new QLabel(QStringLiteral("KALSHI TRADING"), command);
    title->setStyleSheet(QStringLiteral("color:%1;font-size:13px;font-weight:900;").arg(colors::CYAN()));
    command_layout->addWidget(title);
    family_combo_ = new QComboBox(command);
    family_combo_->addItems({QStringLiteral("Crypto"), QStringLiteral("Sports"), QStringLiteral("Politics"),
                             QStringLiteral("Economics"), QStringLiteral("Weather"), QStringLiteral("Entertainment"),
                             QStringLiteral("Science & Tech")});
    family_combo_->setFixedWidth(150);
    command_layout->addWidget(family_combo_);
    search_ = new QLineEdit(command);
    search_->setPlaceholderText(QStringLiteral("Search Kalshi contracts"));
    search_->setMinimumWidth(200);
    command_layout->addWidget(search_, 1);
    count_label_ = new QLabel(QStringLiteral("0 CONTRACTS"), command);
    command_layout->addWidget(count_label_);
    auto* refresh_button = new QPushButton(QStringLiteral("REFRESH"), command);
    refresh_button->setToolTip(QStringLiteral("Refresh the current Kalshi ladder"));
    command_layout->addWidget(refresh_button);
    connection_badge_ = new QLabel(QStringLiteral("PUBLIC DATA"), command);
    connection_badge_->setStyleSheet(QStringLiteral("color:%1;font-weight:800;padding:6px;").arg(colors::GREEN()));
    command_layout->addWidget(connection_badge_);
    polymarket_button_ = new QPushButton(QStringLiteral("POLYMARKET"), command);
    polymarket_button_->setToolTip(QStringLiteral("Open the separate Polymarket workspace"));
    command_layout->addWidget(polymarket_button_);
    root->addWidget(command);

    auto* filters = new QFrame(this);
    filters->setObjectName(QStringLiteral("ksBand"));
    auto* filters_layout = new QHBoxLayout(filters);
    filters_layout->setContentsMargins(12, 7, 12, 7);
    filters_layout->setSpacing(6);
    filters_layout->addWidget(new QLabel(QStringLiteral("ASSET")));
    asset_bar_ = new QWidget(filters);
    auto* asset_layout = new QHBoxLayout(asset_bar_);
    asset_layout->setContentsMargins(0, 0, 0, 0);
    asset_layout->setSpacing(4);
    for (const QString& asset : {QStringLiteral("BTC"), QStringLiteral("ETH"), QStringLiteral("SOL"), QStringLiteral("DOGE"), QStringLiteral("XRP")}) {
        auto* button = segment(asset, asset_bar_);
        button->setChecked(asset == asset_);
        connect(button, &QPushButton::clicked, this, [this, asset]() { set_asset(asset); });
        asset_layout->addWidget(button);
    }
    filters_layout->addWidget(asset_bar_);
    filters_layout->addSpacing(16);
    filters_layout->addWidget(new QLabel(QStringLiteral("DURATION")));
    cadence_bar_ = new QWidget(filters);
    auto* cadence_layout = new QHBoxLayout(cadence_bar_);
    cadence_layout->setContentsMargins(0, 0, 0, 0);
    cadence_layout->setSpacing(4);
    const QVector<QPair<QString, QString>> cadences = {{QStringLiteral("15 MIN"), QStringLiteral("fifteen_min")},
                                                        {QStringLiteral("1 HOUR"), QStringLiteral("hourly")},
                                                        {QStringLiteral("DAILY"), QStringLiteral("daily")},
                                                        {QStringLiteral("WEEKLY"), QStringLiteral("weekly")}};
    for (const auto& cadence : cadences) {
        auto* button = segment(cadence.first, cadence_bar_);
        button->setChecked(cadence.second == cadence_);
        connect(button, &QPushButton::clicked, this, [this, value = cadence.second]() { set_cadence(value); });
        cadence_layout->addWidget(button);
    }
    filters_layout->addWidget(cadence_bar_);
    filters_layout->addStretch();
    auto* lane = new QLabel(QStringLiteral("LANE: 85–92% MAKER FAVORITES"), filters);
    lane->setStyleSheet(QStringLiteral("color:%1;font-weight:800;").arg(colors::WARNING()));
    filters_layout->addWidget(lane);
    root->addWidget(filters);

    workspace_splitter_ = new QSplitter(Qt::Horizontal, this);
    workspace_splitter_->setObjectName(QStringLiteral("ksWorkspaceSplitter"));
    workspace_splitter_->setHandleWidth(9);
    workspace_splitter_->setStyleSheet(QStringLiteral(
        "QSplitter#ksWorkspaceSplitter::handle:horizontal{"
        "background:%1;border-left:1px solid %2;border-right:1px solid %2;}"
        "QSplitter#ksWorkspaceSplitter::handle:horizontal:hover{background:%3;}"
    ).arg(colors::BG_RAISED(), colors::BORDER_BRIGHT(), colors::CYAN()));
    market_list_ = new QListWidget(workspace_splitter_);
    market_list_->setMinimumWidth(260);
    market_list_->setMaximumWidth(340);

    auto* center = new QWidget(workspace_splitter_);
    center->setMinimumWidth(0);
    center->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* center_layout = new QVBoxLayout(center);
    center_layout->setSizeConstraint(QLayout::SetNoConstraint);
    center_layout->setContentsMargins(14, 12, 14, 8);
    market_title_ = new QLabel(QStringLiteral("Choose a Kalshi contract"), center);
    market_title_->setWordWrap(true);
    market_title_->setStyleSheet(QStringLiteral("font-size:17px;font-weight:900;color:%1;").arg(colors::TEXT_PRIMARY()));
    center_layout->addWidget(market_title_);
    market_meta_ = new QLabel(QStringLiteral("The event ladder appears on the left."), center);
    market_meta_->setStyleSheet(QStringLiteral("color:%1;").arg(colors::TEXT_SECONDARY()));
    center_layout->addWidget(market_meta_);
    auto* quotes = new QHBoxLayout;
    yes_quote_ = new QLabel(QStringLiteral("YES —"), center);
    no_quote_ = new QLabel(QStringLiteral("NO —"), center);
    yes_quote_->setStyleSheet(QStringLiteral("color:%1;font-size:22px;font-weight:900;padding:10px 0;").arg(colors::CYAN()));
    no_quote_->setStyleSheet(QStringLiteral("color:%1;font-size:22px;font-weight:900;padding:10px 0;").arg(colors::RED()));
    quotes->addWidget(yes_quote_);
    quotes->addWidget(no_quote_);
    quotes->addStretch();
    center_layout->addLayout(quotes);

    auto* pulse = new QFrame(center);
    pulse->setObjectName(QStringLiteral("ksPulse"));
    pulse->setStyleSheet(QStringLiteral("QFrame#ksPulse{background:%1;border:1px solid %2;}")
                             .arg(colors::BG_RAISED(), colors::BORDER_DIM()));
    auto* pulse_layout = new QHBoxLayout(pulse);
    pulse_layout->setContentsMargins(12, 7, 12, 7);
    pulse_layout->setSpacing(18);
    target_pulse_ = new QLabel(QStringLiteral("TO BEAT\n—"), pulse);
    spot_pulse_ = new QLabel(QStringLiteral("NOW\nWAITING FOR PRICE"), pulse);
    clock_pulse_ = new QLabel(QStringLiteral("CLOSE\n—"), pulse);
    target_pulse_->setStyleSheet(QStringLiteral("color:%1;font-size:14px;font-weight:900;").arg(colors::TEXT_PRIMARY()));
    spot_pulse_->setStyleSheet(QStringLiteral("color:%1;font-size:14px;font-weight:900;").arg(colors::TEXT_PRIMARY()));
    clock_pulse_->setStyleSheet(QStringLiteral("color:%1;font-size:14px;font-weight:900;").arg(colors::WARNING()));
    pulse_layout->addWidget(target_pulse_, 1);
    pulse_layout->addWidget(spot_pulse_, 1);
    pulse_layout->addStretch();
    pulse_layout->addWidget(clock_pulse_);
    center_layout->addWidget(pulse);

    venue_consensus_ = new QLabel(QStringLiteral("VISUAL SPOT REFERENCE ONLY · CONNECTING KRAKEN + COINBASE"), center);
    venue_consensus_->setWordWrap(true);
    venue_consensus_->setToolTip(QStringLiteral(
        "Visual public order-book reference only. It does not set Kalshi contract prices, settlement values, or ladder decisions."));
    venue_consensus_->setStyleSheet(QStringLiteral("color:%1;background:%2;border:1px solid %3;padding:6px 10px;font-size:11px;font-weight:800;")
                                        .arg(colors::TEXT_SECONDARY(), colors::BG_RAISED(), colors::BORDER_DIM()));
    center_layout->addWidget(venue_consensus_);

    reference_chart_ = new KalshiSimpleChart(center);
    reference_chart_->setMinimumHeight(220);
    reference_chart_->setMaximumHeight(320);
    reference_chart_->set_timeframe_changed([this](const QString& timeframe) {
        set_chart_timeframe(timeframe);
    });
    auto* chart_container = new QWidget(center);
    auto* chart_layout = new QVBoxLayout(chart_container);
    chart_layout->setContentsMargins(0, 0, 0, 0);
    chart_layout->setSpacing(0);
    auto* chart_title = new QLabel(QStringLiteral("KALSHI CONTRACT PROBABILITY · DIRECT KALSHI FEED"),
                                   chart_container);
    chart_title->setStyleSheet(QStringLiteral("color:%1;background:%2;padding:5px 8px;font-weight:900;")
                                   .arg(colors::CYAN(), colors::BG_RAISED()));
    chart_layout->addWidget(chart_title);
    chart_status_ = new QLabel(QStringLiteral("LOADING PRICE HISTORY"), chart_container);
    chart_status_->setAlignment(Qt::AlignCenter);
    chart_status_->setStyleSheet(QStringLiteral("color:%1;background:%2;padding:4px;font-weight:800;")
                                     .arg(colors::WARNING(), colors::BG_RAISED()));
    chart_layout->addWidget(chart_status_);
    chart_layout->addWidget(reference_chart_, 1);
    center_layout->addWidget(chart_container);

    auto* tabs = new QTabWidget(center);
    tabs->setMinimumWidth(0);
    tabs->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* maker_scroll = new QScrollArea(tabs);
    maker_scroll->setWidgetResizable(true);
    maker_scroll->setFrameShape(QFrame::NoFrame);
    auto* maker = new QWidget(maker_scroll);
    auto* maker_layout = new QVBoxLayout(maker);
    maker_layout->setContentsMargins(14, 12, 14, 12);
    maker_layout->setSpacing(10);
    auto* order_title = new QLabel(QStringLiteral("MAKER ORDER · REVIEW ONLY"), maker);
    order_title->setStyleSheet(QStringLiteral("font-weight:900;color:%1;").arg(colors::TEXT_PRIMARY()));
    maker_layout->addWidget(order_title);
    quote_health_ = new QLabel(QStringLiteral("QUOTE HEALTH · WAITING FOR KALSHI BOOK"), maker);
    quote_health_->setWordWrap(true);
    quote_health_->setStyleSheet(QStringLiteral("color:%1;background:%2;border:1px solid %3;padding:7px;font-weight:800;")
                                     .arg(colors::WARNING(), colors::BG_RAISED(), colors::BORDER_DIM()));
    quote_health_->setToolTip(QStringLiteral("Executable top-of-book bid and ask, displayed size, and quote age. Midpoints are not executable prices."));
    maker_layout->addWidget(quote_health_);
    auto* side_layout = new QHBoxLayout;
    yes_button_ = segment(QStringLiteral("BUY YES"), maker);
    no_button_ = segment(QStringLiteral("BUY NO"), maker);
    yes_button_->setChecked(true);
    side_layout->addWidget(yes_button_);
    side_layout->addWidget(no_button_);
    side_layout->addStretch();
    maker_layout->addLayout(side_layout);
    auto* form = new QFormLayout;
    price_ = new QDoubleSpinBox(maker);
    price_->setRange(0.01, 0.99);
    price_->setSingleStep(0.01);
    price_->setDecimals(2);
    contracts_ = new QSpinBox(maker);
    contracts_->setRange(1, 1000);
    contracts_->setValue(2);
    form->addRow(QStringLiteral("Limit price"), price_);
    form->addRow(QStringLiteral("Contracts"), contracts_);
    maker_layout->addLayout(form);
    cost_label_ = new QLabel(QStringLiteral("Notional: —"), maker);
    fee_label_ = new QLabel(QStringLiteral("Max fee: —"), maker);
    payout_label_ = new QLabel(QStringLiteral("Payout if correct: —"), maker);
    fee_label_->setWordWrap(true);
    maker_layout->addWidget(cost_label_);
    maker_layout->addWidget(fee_label_);
    maker_layout->addWidget(payout_label_);
    auto* preview = new QPushButton(QStringLiteral("PREVIEW MAKER ORDER"), maker);
    preview->setStyleSheet(QStringLiteral("QPushButton{background:%1;color:#00110d;border:0;padding:10px;font-weight:900;}").arg(colors::CYAN()));
    preview->setToolTip(QStringLiteral("Review only. Live submission is locked by the evidence gate."));
    maker_layout->addWidget(preview);

    auto* separator = new QFrame(maker);
    separator->setFrameShape(QFrame::HLine);
    maker_layout->addWidget(separator);
    auto* automation_title = new QLabel(QStringLiteral("AUTOMATION"), maker);
    automation_title->setStyleSheet(QStringLiteral("font-weight:900;color:%1;").arg(colors::TEXT_PRIMARY()));
    maker_layout->addWidget(automation_title);
    gate_label_ = new QLabel(QStringLiteral("LIVE LOCKED\n0 / 100 maker markets\n0 / 14 evidence days"), maker);
    gate_label_->setWordWrap(true);
    gate_label_->setStyleSheet(QStringLiteral("color:%1;border:1px solid %1;padding:9px;font-weight:800;").arg(colors::RED()));
    maker_layout->addWidget(gate_label_);
    shadow_status_ = new QLabel(QStringLiteral("AUTO SHADOW ACTIVE\nWatching queue depletion and trade-through. No orders are submitted."), maker);
    shadow_status_->setWordWrap(true);
    shadow_status_->setStyleSheet(QStringLiteral("color:%1;").arg(colors::TEXT_SECONDARY()));
    maker_layout->addWidget(shadow_status_);
    ladder_status_ = new QLabel(QStringLiteral("LADDER ENGINE WARMING\nWaiting for complete event books."), maker);
    ladder_status_->setWordWrap(true);
    ladder_status_->setStyleSheet(QStringLiteral("color:%1;border-top:1px solid %2;padding-top:8px;")
                                      .arg(colors::WARNING(), colors::BORDER_DIM()));
    maker_layout->addWidget(ladder_status_);
    recorder_health_ = new QLabel(QStringLiteral("RECORDER STARTING"), maker);
    recorder_health_->setWordWrap(true);
    recorder_health_->setStyleSheet(QStringLiteral("color:%1;background:%2;border:1px solid %3;padding:7px;font-size:10px;")
                                        .arg(colors::TEXT_SECONDARY(), colors::BG_RAISED(), colors::BORDER_DIM()));
    maker_layout->addWidget(recorder_health_);
    shadow_button_ = new QPushButton(QStringLiteral("PAUSE SHADOW"), maker);
    shadow_button_->setToolTip(QStringLiteral("Pause or resume local shadow evidence collection"));
    maker_layout->addWidget(shadow_button_);
    maker_layout->addStretch();
    maker_scroll->setWidget(maker);
    tabs->addTab(maker_scroll, QStringLiteral("MAKER ORDER"));
    ladder_table_ = new CompactTableWidget(0, 4, tabs);
    ladder_table_->setHorizontalHeaderLabels({QStringLiteral("STATUS"), QStringLiteral("EDGE"),
                                               QStringLiteral("TYPE"), QStringLiteral("DETAIL")});
    ladder_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ladder_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ladder_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ladder_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    ladder_table_->verticalHeader()->hide();
    ladder_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ladder_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    ladder_table_->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    ladder_table_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    ladder_table_->setToolTip(QStringLiteral("Relative-value diagnostics across every contract in the selected Kalshi event. Observation only."));
    tabs->addTab(ladder_table_, QStringLiteral("LADDER EDGE"));
    book_table_ = new QTableWidget(0, 4, tabs);
    book_table_->setHorizontalHeaderLabels({QStringLiteral("BID SIZE"), QStringLiteral("BID"), QStringLiteral("ASK"), QStringLiteral("ASK SIZE")});
    book_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    book_table_->verticalHeader()->hide();
    tabs->addTab(book_table_, QStringLiteral("ORDER BOOK"));
    trades_table_ = new QTableWidget(0, 4, tabs);
    trades_table_->setHorizontalHeaderLabels({QStringLiteral("TIME"), QStringLiteral("SIDE"), QStringLiteral("PRICE"), QStringLiteral("SIZE")});
    trades_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    trades_table_->verticalHeader()->hide();
    tabs->addTab(trades_table_, QStringLiteral("RECENT TRADES"));
    rules_ = new QTextEdit(tabs);
    rules_->setReadOnly(true);
    tabs->addTab(rules_, QStringLiteral("RULES"));
    tabs->setCurrentIndex(0);
    connect(tabs, &QTabWidget::currentChanged, maker_scroll, [maker_scroll](int index) {
        if (index == 0) maker_scroll->verticalScrollBar()->setValue(0);
    });
    QTimer::singleShot(0, maker_scroll, [maker_scroll]() {
        maker_scroll->verticalScrollBar()->setValue(0);
    });
    center_layout->addWidget(tabs, 1);

    dom_panel_ = new QFrame(workspace_splitter_);
    dom_panel_->setObjectName(QStringLiteral("ksDomPanel"));
    dom_panel_->setMinimumWidth(300);
    dom_panel_->setMaximumWidth(520);
    dom_panel_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    dom_panel_->setStyleSheet(QStringLiteral("QFrame#ksDomPanel{background:%1;border-left:1px solid %2;}")
                                 .arg(colors::BG_BASE(), colors::BORDER_MED()));
    auto* dom_layout = new QVBoxLayout(dom_panel_);
    dom_layout->setContentsMargins(8, 10, 8, 8);
    dom_layout->setSpacing(5);
    auto* dom_header = new QHBoxLayout;
    dom_header->setContentsMargins(0, 0, 0, 0);
    dom_header->setSpacing(4);
    dom_title_ = new QLabel(QStringLiteral("KRAKEN DOM · BTC/USD"), dom_panel_);
    dom_title_->setStyleSheet(QStringLiteral("color:%1;font-weight:900;").arg(colors::CYAN()));
    dom_header->addWidget(dom_title_, 1);
    dom_layout->addLayout(dom_header);
    auto* dom_controls = new QHBoxLayout;
    dom_controls->setContentsMargins(0, 0, 0, 0);
    dom_controls->setSpacing(4);
    kraken_dom_button_ = new QPushButton(QStringLiteral("KRAKEN"), dom_panel_);
    coinbase_dom_button_ = new QPushButton(QStringLiteral("COINBASE"), dom_panel_);
    kraken_dom_button_->setCheckable(true);
    coinbase_dom_button_->setCheckable(true);
    kraken_dom_button_->setChecked(true);
    kraken_dom_button_->setFixedHeight(24);
    coinbase_dom_button_->setFixedHeight(24);
    dom_controls->addWidget(kraken_dom_button_);
    dom_controls->addWidget(coinbase_dom_button_);
    auto* reset_dom_width = new QPushButton(QStringLiteral("RESET WIDTH"), dom_panel_);
    reset_dom_width->setFixedHeight(24);
    reset_dom_width->setToolTip(QStringLiteral("Restore the default ladder, workspace, and DOM widths"));
    dom_controls->addWidget(reset_dom_width);
    dom_layout->addLayout(dom_controls);
    auto* dom_note = new QLabel(QStringLiteral("Visual reference only. Never used for Kalshi pricing, settlement, or ladder decisions."), dom_panel_);
    dom_note->setWordWrap(true);
    dom_note->setStyleSheet(QStringLiteral("color:%1;font-size:10px;").arg(colors::TEXT_SECONDARY()));
    dom_layout->addWidget(dom_note);
    dom_status_ = new QLabel(QStringLiteral("CONNECTING TO SPOT BOOK"), dom_panel_);
    dom_status_->setStyleSheet(QStringLiteral("color:%1;font-size:10px;font-weight:800;").arg(colors::WARNING()));
    dom_layout->addWidget(dom_status_);
    spot_dom_ = new crypto::CryptoOrderBook(dom_panel_);
    dom_layout->addWidget(spot_dom_, 1);

    workspace_splitter_->addWidget(market_list_);
    workspace_splitter_->addWidget(center);
    workspace_splitter_->addWidget(dom_panel_);
    workspace_splitter_->setStretchFactor(0, 0);
    workspace_splitter_->setStretchFactor(1, 1);
    workspace_splitter_->setStretchFactor(2, 0);
    workspace_splitter_->setCollapsible(0, false);
    workspace_splitter_->setCollapsible(1, false);
    workspace_splitter_->setCollapsible(2, false);
    const auto reset_pane_widths = [this]() { ensure_workspace_panes_visible(); };
    reset_pane_widths();
    connect(reset_dom_width, &QPushButton::clicked, this, reset_pane_widths);
    workspace_splitter_->handle(1)->setToolTip(QStringLiteral("Drag to resize the contract ladder"));
    workspace_splitter_->handle(2)->setToolTip(QStringLiteral("Drag to resize the reference DOM"));
    root->addWidget(workspace_splitter_, 1);

    connect(polymarket_button_, &QPushButton::clicked, this,
            [this]() { emit venue_switch_requested(QStringLiteral("polymarket")); });
    connect(family_combo_, &QComboBox::currentTextChanged, this, &KalshiScreen::set_family);
    connect(refresh_button, &QPushButton::clicked, this, &KalshiScreen::refresh);
    connect(search_, &QLineEdit::returnPressed, this, [this]() {
        if (auto* a = adapter()) a->search(search_->text().trimmed(), 200);
    });
    connect(market_list_, &QListWidget::currentRowChanged, this, &KalshiScreen::select_market);
    connect(yes_button_, &QPushButton::clicked, this, [this]() {
        side_ = QStringLiteral("YES"); yes_button_->setChecked(true); no_button_->setChecked(false); render_market();
    });
    connect(no_button_, &QPushButton::clicked, this, [this]() {
        side_ = QStringLiteral("NO"); yes_button_->setChecked(false); no_button_->setChecked(true); render_market();
    });
    auto update_cost = [this]() {
        const double p = price_->value();
        const int n = contracts_->value();
        const double fee = has_selection_
            ? kalshi_data::KalshiEvidenceEngine::conservative_taker_fee(selected_, p, n)
            : kalshi_data::KalshiEvidenceEngine::conservative_taker_fee(p, n);
        cost_label_->setText(QStringLiteral("Notional: %1").arg(money(p * n)));
        fee_label_->setText(QStringLiteral("Max taker fee: %1 · maker may be lower").arg(money(fee)));
        payout_label_->setText(QStringLiteral("Payout if correct: %1").arg(money(n)));
    };
    connect(price_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [update_cost](double) { update_cost(); });
    connect(contracts_, QOverload<int>::of(&QSpinBox::valueChanged), this, [update_cost](int) { update_cost(); });
    connect(preview, &QPushButton::clicked, this, &KalshiScreen::preview_order);
    connect(shadow_button_, &QPushButton::clicked, this, &KalshiScreen::toggle_shadow_collector);
    connect(kraken_dom_button_, &QPushButton::clicked, this,
            [this]() { set_reference_dom_venue(QStringLiteral("kraken")); });
    connect(coinbase_dom_button_, &QPushButton::clicked, this,
            [this]() { set_reference_dom_venue(QStringLiteral("coinbase")); });
    update_cost();
}

void KalshiScreen::wire_adapter() {
    auto* a = adapter();
    if (!a) { connection_badge_->setText(QStringLiteral("ADAPTER OFFLINE")); return; }
    connections_ << connect(a, &pred::PredictionExchangeAdapter::events_ready, this, &KalshiScreen::populate_events);
    connections_ << connect(a, &pred::PredictionExchangeAdapter::markets_ready, this, &KalshiScreen::populate_markets);
    connections_ << connect(a, &pred::PredictionExchangeAdapter::search_results_ready, this,
                            [this](const QVector<pred::PredictionMarket>& markets, const QVector<pred::PredictionEvent>& events) {
                                if (!markets.isEmpty()) populate_markets(markets); else populate_events(events);
                            });
    connections_ << connect(a, &pred::PredictionExchangeAdapter::order_book_ready, this, &KalshiScreen::render_order_book);
    connections_ << connect(a, &pred::PredictionExchangeAdapter::recent_trades_ready, this, &KalshiScreen::render_trades);
    connections_ << connect(a, &pred::PredictionExchangeAdapter::price_history_ready, this,
                            [this](const pred::PriceHistory& history) {
        chart_fetching_ = false;
        if (!has_selection_ || chart_asset_id_.isEmpty() ||
            history.asset_id != chart_asset_id_) return;
        QVector<openmarketterminal::trading::Candle> candles;
        candles.reserve(history.points.size());
        for (const auto& point : history.points) {
            if (point.ts_ms <= 0 || point.price <= 0.0) continue;
            openmarketterminal::trading::Candle candle;
            candle.timestamp = point.ts_ms;
            candle.open = candle.high = candle.low = candle.close = point.price;
            candles.append(candle);
        }
        if (!candles.isEmpty()) {
            reference_chart_->set_candles(candles);
            live_chart_seeded_ = true;
            last_chart_fetch_ms_ = QDateTime::currentMSecsSinceEpoch();
            chart_status_->hide();
        } else {
            chart_status_->setText(QStringLiteral("KALSHI CONTRACT HAS NO CANDLE/TRADES YET"));
            chart_status_->show();
        }
    });
    connections_ << connect(a, &pred::PredictionExchangeAdapter::ws_orderbook_updated, this,
                            [this](const QString&, const pred::PredictionOrderBook& book) { render_order_book(book); });
    connections_ << connect(a, &pred::PredictionExchangeAdapter::ws_connection_changed, this, [this](bool connected) {
        connection_badge_->setText(connected ? QStringLiteral("LIVE BOOK") : QStringLiteral("PUBLIC DATA"));
    });
    connections_ << connect(a, &pred::PredictionExchangeAdapter::error_occurred, this,
                            [this](const QString& context, const QString& message) {
                                if (context == QStringLiteral("fetch_price_history"))
                                    chart_fetching_ = false;
                                if (context == QStringLiteral("fetch_market") &&
                                    (message.contains(QStringLiteral("404")) ||
                                     message.contains(QStringLiteral("not_found"))))
                                    return;
                                connection_badge_->setText(QStringLiteral("DATA ISSUE"));
                                connection_badge_->setToolTip(message);
                            });
    if (auto* kalshi = qobject_cast<kalshi_data::KalshiAdapter*>(a)) {
        connections_ << connect(kalshi, &kalshi_data::KalshiAdapter::ws_trade_received,
                                this, &KalshiScreen::record_kalshi_trade);
        connections_ << connect(kalshi, &kalshi_data::KalshiAdapter::ws_trade_event,
                                this, [this](const QString& ticker, const QJsonObject& payload) {
            QJsonObject row = payload;
            row.insert(QStringLiteral("event"), QStringLiteral("kalshi_trade_raw"));
            row.insert(QStringLiteral("market_ticker"), ticker);
            row.insert(QStringLiteral("received_ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            row.insert(QStringLiteral("live_eligible"), false);
            kalshi_data::KalshiEvidenceEngine::append_jsonl(
                evidence_path(QStringLiteral("kalshi-trade-events.jsonl")), row);
        });
        connections_ << connect(kalshi, &kalshi_data::KalshiAdapter::ws_market_lifecycle_event,
                                this, [this, a](const QString& ticker, const QString&, const QJsonObject& payload) {
            QJsonObject row = payload;
            row.insert(QStringLiteral("event"), QStringLiteral("kalshi_market_lifecycle"));
            row.insert(QStringLiteral("received_ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            row.insert(QStringLiteral("live_eligible"), false);
            kalshi_data::KalshiEvidenceEngine::append_jsonl(
                evidence_path(QStringLiteral("kalshi-lifecycle.jsonl")), row);
            pred::MarketKey key;
            key.exchange_id = QStringLiteral("kalshi");
            key.market_id = ticker;
            a->fetch_market(key);
        });
        connections_ << connect(kalshi, &kalshi_data::KalshiAdapter::ws_ticker_event,
                                this, [this](const QString& ticker, const QJsonObject& payload) {
            QJsonObject row = payload;
            row.insert(QStringLiteral("event"), QStringLiteral("kalshi_ticker"));
            row.insert(QStringLiteral("market_ticker"), ticker);
            row.insert(QStringLiteral("received_ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            row.insert(QStringLiteral("live_eligible"), false);
            if (kalshi_data::KalshiEvidenceEngine::append_jsonl(
                    evidence_path(QStringLiteral("kalshi-tickers.jsonl")), row))
                ++ticker_events_recorded_;
            if (has_selection_ && ticker == selected_.key.market_id)
                last_kalshi_ticker_ms_ = QDateTime::currentMSecsSinceEpoch();
            apply_live_market_prices(ticker, payload);
        });
        connections_ << connect(kalshi, &kalshi_data::KalshiAdapter::ws_orderbook_event,
                                this, [this](const QString& type, const QString& ticker,
                                             qint64 sequence, const QJsonObject& payload) {
            QJsonObject event{{QStringLiteral("message_type"), type},
                              {QStringLiteral("sequence"), QString::number(sequence)},
                              {QStringLiteral("payload"), payload}};
            orderbook_event_buffer_[ticker].append(event);
        });
        connections_ << connect(kalshi, &kalshi_data::KalshiAdapter::ws_account_event,
                                this, [this](const QString& type, const QJsonObject& payload) {
            QJsonObject row = payload;
            row.insert(QStringLiteral("event"), QStringLiteral("kalshi_account_event"));
            row.insert(QStringLiteral("message_type"), type);
            row.insert(QStringLiteral("received_ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            row.insert(QStringLiteral("live_eligible"), false);
            kalshi_data::KalshiEvidenceEngine::append_jsonl(
                evidence_path(QStringLiteral("kalshi-account-events.jsonl")), row);
        });
        connections_ << connect(kalshi, &kalshi_data::KalshiAdapter::batch_order_books_ready,
                                this, [this](const QHash<QString, pred::PredictionOrderBook>& books) {
            for (const auto& book : books) render_order_book(book);
        });
        connections_ << connect(kalshi, &kalshi_data::KalshiAdapter::series_detail_ready,
                                this, [this](const QString& ticker, const QJsonObject& series) {
            const auto apply = [&ticker, &series](pred::PredictionMarket& market) {
                if (market.extras.value(QStringLiteral("series_ticker")).toString() != ticker) return;
                market.extras.insert(QStringLiteral("fee_type"), series.value(QStringLiteral("fee_type")).toVariant());
                market.extras.insert(QStringLiteral("fee_multiplier"), series.value(QStringLiteral("fee_multiplier")).toVariant());
            };
            for (auto& market : all_markets_) apply(market);
            for (auto& market : markets_) apply(market);
            if (has_selection_) apply(selected_);
        });
        connections_ << connect(kalshi, &kalshi_data::KalshiAdapter::historical_markets_ready,
                                this, [this](const QVector<pred::PredictionMarket>& markets, const QString&) {
            for (const auto& market : markets) {
                if (!market.extras.value(QStringLiteral("result")).toString().isEmpty() ||
                    !market.extras.value(QStringLiteral("expiration_value")).toString().isEmpty())
                    reconcile_settlement(market);
            }
        });
        connections_ << connect(kalshi, &kalshi_data::KalshiAdapter::settlements_ready,
                                this, [this](const QJsonArray& settlements) {
            for (const auto& value : settlements) {
                QJsonObject row = value.toObject();
                const QString key = QString::fromUtf8(QJsonDocument(row).toJson(QJsonDocument::Compact));
                if (key.isEmpty() || recorded_account_settlements_.contains(key)) continue;
                recorded_account_settlements_.insert(key);
                row.insert(QStringLiteral("event"), QStringLiteral("kalshi_account_settlement"));
                row.insert(QStringLiteral("received_ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                row.insert(QStringLiteral("live_eligible"), false);
                kalshi_data::KalshiEvidenceEngine::append_jsonl(
                    evidence_path(QStringLiteral("kalshi-account-settlements.jsonl")), row);
            }
        });
        const auto record_exchange_json = [this](const QString& event, const QJsonValue& payload) {
            QJsonObject row{{QStringLiteral("event"), event},
                            {QStringLiteral("received_ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                            {QStringLiteral("payload"), payload},
                            {QStringLiteral("live_eligible"), false}};
            kalshi_data::KalshiEvidenceEngine::append_jsonl(
                evidence_path(QStringLiteral("kalshi-exchange-metadata.jsonl")), row);
        };
        connections_ << connect(kalshi, &kalshi_data::KalshiAdapter::exchange_status_ready,
                                this, [record_exchange_json](const QJsonObject& status) {
            record_exchange_json(QStringLiteral("kalshi_exchange_status"), status);
        });
        connections_ << connect(kalshi, &kalshi_data::KalshiAdapter::exchange_schedule_ready,
                                this, [record_exchange_json](const QJsonObject& schedule) {
            record_exchange_json(QStringLiteral("kalshi_exchange_schedule"), schedule);
        });
        connections_ << connect(kalshi, &kalshi_data::KalshiAdapter::series_fee_changes_ready,
                                this, [record_exchange_json](const QJsonArray& changes) {
            record_exchange_json(QStringLiteral("kalshi_series_fee_changes"), changes);
        });
    }
    connections_ << connect(a, &pred::PredictionExchangeAdapter::market_detail_ready,
                            this, [this](const pred::PredictionMarket& market) {
        const QString result = market.extras.value(QStringLiteral("result")).toString();
        const QString expiration = market.extras.value(QStringLiteral("expiration_value")).toString();
        if (!result.isEmpty() || !expiration.isEmpty()) reconcile_settlement(market);
        if (has_selection_ && market.key.market_id == selected_.key.market_id) {
            selected_ = market;
            render_market();
        }
    });
}

QString KalshiScreen::category_slug() const {
    if (family_ != QStringLiteral("Crypto")) return family_;
    return QStringLiteral("Crypto#%1@%2").arg(asset_, cadence_);
}

void KalshiScreen::refresh() {
    if (auto* a = adapter()) {
        connection_badge_->setText(QStringLiteral("LOADING"));
        a->list_events(category_slug(), QStringLiteral("volume"), 200, 0);
        a->fetch_open_orders();
        if (auto* kalshi = qobject_cast<kalshi_data::KalshiAdapter*>(a); kalshi) {
            kalshi->fetch_exchange_status();
            kalshi->fetch_exchange_schedule();
            kalshi->fetch_series_fee_changes();
            if (kalshi->has_credentials()) kalshi->fetch_settlements(100);
        }
    }
}

void KalshiScreen::set_family(const QString& family) {
    family_ = family;
    asset_bar_->setVisible(family_ == QStringLiteral("Crypto"));
    cadence_bar_->setVisible(family_ == QStringLiteral("Crypto"));
    refresh();
}

void KalshiScreen::set_asset(const QString& asset) {
    asset_ = asset;
    set_spot_symbol(asset_);
    for (auto* button : asset_bar_->findChildren<QPushButton*>()) button->setChecked(button->text() == asset_);
    refresh();
}

void KalshiScreen::set_cadence(const QString& cadence) {
    cadence_ = cadence;
    const QHash<QString, QString> labels = {{QStringLiteral("fifteen_min"), QStringLiteral("15 MIN")},
                                            {QStringLiteral("hourly"), QStringLiteral("1 HOUR")},
                                            {QStringLiteral("daily"), QStringLiteral("DAILY")},
                                            {QStringLiteral("weekly"), QStringLiteral("WEEKLY")}};
    for (auto* button : cadence_bar_->findChildren<QPushButton*>()) button->setChecked(button->text() == labels.value(cadence_));
    refresh();
}

void KalshiScreen::populate_events(const QVector<pred::PredictionEvent>& events) {
    QVector<pred::PredictionMarket> flattened;
    for (const auto& event : events) {
        for (auto market : event.markets) {
            if (market.question.trimmed().isEmpty()) market.question = event.title;
            market.extras.insert(QStringLiteral("event_title"), event.title);
            flattened.push_back(market);
        }
    }
    populate_markets(flattened);
}

void KalshiScreen::populate_markets(const QVector<pred::PredictionMarket>& markets) {
    all_markets_ = markets;
    if (auto* kalshi = qobject_cast<kalshi_data::KalshiAdapter*>(adapter())) {
        QSet<QString> series;
        for (const auto& market : all_markets_) {
            const QString ticker = market.extras.value(QStringLiteral("series_ticker")).toString();
            if (!ticker.isEmpty()) series.insert(ticker);
        }
        for (const auto& ticker : series) {
            kalshi->fetch_series_detail(ticker);
            if (!backfilled_series_.contains(ticker)) {
                backfilled_series_.insert(ticker);
                kalshi->fetch_historical_markets(ticker, 100);
            }
        }
    }
    markets_.clear();
    markets_.reserve(markets.size());
    for (const auto& market : markets) {
        if (family_ == QStringLiteral("Crypto") && !in_selected_time_horizon(market, cadence_)) continue;
        const double yes = outcome_price(market, 0);
        const double no = outcome_price(market, 1);
        // The 1%/99% tails are not actionable and dominate large ladders with
        // visual noise. Keep every contract from 2% through 98% available.
        const double favorite = qMax(yes, no);
        if (favorite >= 0.99 || (yes <= 0.01 && no <= 0.01)) continue;
        markets_.push_back(market);
    }
    std::stable_sort(markets_.begin(), markets_.end(), [](const auto& left, const auto& right) {
        return lane_score(left) > lane_score(right);
    });
    market_list_->clear();
    for (const auto& market : markets_) {
        const double yes = outcome_price(market, 0);
        const double no = outcome_price(market, 1);
        const QString event = market.extras.value(QStringLiteral("event_title")).toString();
        const QString label = QStringLiteral("%1\nYES %2   NO %3\n%4")
                                  .arg(contract_label(market), contract_quote(yes), contract_quote(no),
                                       event.isEmpty() ? market.question : event);
        market_list_->addItem(label);
    }
    count_label_->setText(QStringLiteral("%1 CONTRACTS").arg(markets_.size()));
    connection_badge_->setText(QStringLiteral("PUBLIC DATA"));
    if (!markets_.isEmpty()) market_list_->setCurrentRow(0);
}

void KalshiScreen::select_market(int row) {
    if (row < 0 || row >= markets_.size()) return;
    if (adapter() && !subscribed_ladder_assets_.isEmpty())
        adapter()->unsubscribe_market(subscribed_ladder_assets_);
    selected_ = markets_[row];
    has_selection_ = true;
    last_kalshi_ticker_ms_ = 0;
    last_kalshi_trade_ms_ = 0;
    last_kalshi_book_ms_ = 0;
    side_ = outcome_price(selected_, 1) > outcome_price(selected_, 0)
                ? QStringLiteral("NO") : QStringLiteral("YES");
    chart_asset_id_ = selected_.key.asset_ids.value(side_ == QStringLiteral("NO") ? 1 : 0);
    reference_chart_->clear();
    reference_chart_->set_probability_mode(true);
    reference_chart_->set_symbol(selected_.key.market_id + QLatin1Char(' ') + side_);
    reference_chart_->set_targets({});
    live_chart_seeded_ = false;
    last_chart_fetch_ms_ = 0;
    chart_status_->setText(QStringLiteral("LOADING KALSHI CONTRACT HISTORY"));
    chart_status_->show();
    yes_button_->setChecked(side_ == QStringLiteral("YES"));
    no_button_->setChecked(side_ == QStringLiteral("NO"));
    render_market();
    if (auto* a = adapter()) {
        if (!selected_.outcomes.isEmpty()) a->fetch_order_book(selected_.outcomes.first().asset_id);
        a->fetch_recent_trades(selected_.key, 100);
        subscribed_ladder_assets_.clear();
        for (const auto& market : all_markets_) {
            if (market.key.event_id == selected_.key.event_id)
                subscribed_ladder_assets_.append(market.key.asset_ids);
        }
        subscribed_ladder_assets_.removeDuplicates();
        if (auto* kalshi = qobject_cast<kalshi_data::KalshiAdapter*>(a)) {
            QStringList tickers;
            for (const auto& asset : subscribed_ladder_assets_) {
                const QString ticker = asset.section(QLatin1Char(':'), 0, 0);
                if (!ticker.isEmpty() && !tickers.contains(ticker)) tickers.append(ticker);
            }
            for (int offset = 0; offset < tickers.size(); offset += 100)
                kalshi->fetch_batch_order_books(tickers.mid(offset, 100));
        }
        a->subscribe_market(subscribed_ladder_assets_.isEmpty()
                                ? selected_.key.asset_ids : subscribed_ladder_assets_);
        refresh_reference_chart();
    }
}

void KalshiScreen::render_market() {
    if (!has_selection_) return;
    const double yes = outcome_price(selected_, 0);
    const double no = outcome_price(selected_, 1);
    const QString event_title = selected_.extras.value(QStringLiteral("event_title")).toString();
    market_title_->setText(event_title.isEmpty()
                               ? contract_label(selected_)
                               : QStringLiteral("%1\n%2").arg(contract_label(selected_), event_title));
    market_meta_->setText(QStringLiteral("%1 · %2 · closes %3")
                              .arg(selected_.key.market_id, selected_.active ? QStringLiteral("OPEN") : QStringLiteral("CLOSED"),
                                   selected_.end_date_iso.isEmpty() ? QStringLiteral("—") : selected_.end_date_iso));
    yes_quote_->setText(QStringLiteral("YES %1").arg(contract_quote(yes)));
    no_quote_->setText(QStringLiteral("NO %1").arg(contract_quote(no)));
    rules_->setPlainText(selected_.description);
    const double selected_price = side_ == QStringLiteral("YES") ? yes : no;
    if (selected_price > 0.0) price_->setValue(selected_price);
    const QString desired_chart_asset = selected_.key.asset_ids.value(
        side_ == QStringLiteral("NO") ? 1 : 0);
    if (!desired_chart_asset.isEmpty() && desired_chart_asset != chart_asset_id_) {
        chart_asset_id_ = desired_chart_asset;
        reference_chart_->clear();
        reference_chart_->set_probability_mode(true);
        reference_chart_->set_symbol(selected_.key.market_id + QLatin1Char(' ') + side_);
        live_chart_seeded_ = false;
        last_chart_fetch_ms_ = 0;
        chart_status_->setText(QStringLiteral("LOADING KALSHI %1 HISTORY").arg(side_));
        chart_status_->show();
        QTimer::singleShot(0, this, &KalshiScreen::refresh_reference_chart);
    }
    update_strike_overlay();
    update_observation_strip();
}

void KalshiScreen::apply_live_market_prices(const QString& ticker, const QJsonObject& payload) {
    if (!has_selection_ || ticker != selected_.key.market_id || selected_.outcomes.size() < 2) return;
    const auto value = [&payload](const QString& key) {
        return payload.value(key).toVariant().toDouble();
    };
    double yes_bid = value(QStringLiteral("yes_bid_dollars"));
    double yes_ask = value(QStringLiteral("yes_ask_dollars"));
    double no_bid = value(QStringLiteral("no_bid_dollars"));
    double no_ask = value(QStringLiteral("no_ask_dollars"));
    if (no_bid <= 0.0 && yes_ask > 0.0) no_bid = 1.0 - yes_ask;
    if (no_ask <= 0.0 && yes_bid > 0.0) no_ask = 1.0 - yes_bid;
    if (yes_bid <= 0.0 && no_ask > 0.0) yes_bid = 1.0 - no_ask;
    if (yes_ask <= 0.0 && no_bid > 0.0) yes_ask = 1.0 - no_bid;
    const auto midpoint = [](double bid, double ask) {
        if (bid > 0.0 && ask > 0.0) return (bid + ask) / 2.0;
        return qMax(bid, ask);
    };
    double yes = midpoint(yes_bid, yes_ask);
    double no = midpoint(no_bid, no_ask);
    if (yes <= 0.0 && no > 0.0) yes = 1.0 - no;
    if (no <= 0.0 && yes > 0.0) no = 1.0 - yes;
    if (yes <= 0.0 || no <= 0.0) return;

    selected_.outcomes[0].price = qBound(0.0, yes, 1.0);
    selected_.outcomes[1].price = qBound(0.0, no, 1.0);
    selected_.extras.insert(QStringLiteral("yes_ask_dollars"), yes_ask);
    selected_.extras.insert(QStringLiteral("no_ask_dollars"), no_ask);
    yes_quote_->setText(QStringLiteral("YES %1").arg(contract_quote(selected_.outcomes[0].price)));
    no_quote_->setText(QStringLiteral("NO %1").arg(contract_quote(selected_.outcomes[1].price)));

    const auto update_market = [this](pred::PredictionMarket& market) {
        if (market.key.market_id != selected_.key.market_id) return;
        market.outcomes = selected_.outcomes;
        market.extras.insert(QStringLiteral("yes_ask_dollars"),
                             selected_.extras.value(QStringLiteral("yes_ask_dollars")));
        market.extras.insert(QStringLiteral("no_ask_dollars"),
                             selected_.extras.value(QStringLiteral("no_ask_dollars")));
    };
    for (auto& market : all_markets_) update_market(market);
    for (int row = 0; row < markets_.size(); ++row) {
        update_market(markets_[row]);
        if (markets_[row].key.market_id != selected_.key.market_id) continue;
        const QString event = markets_[row].extras.value(QStringLiteral("event_title")).toString();
        const QString label = QStringLiteral("%1\nYES %2   NO %3\n%4")
                                  .arg(contract_label(markets_[row]),
                                       contract_quote(markets_[row].outcomes[0].price),
                                       contract_quote(markets_[row].outcomes[1].price),
                                       event.isEmpty() ? markets_[row].question : event);
        if (auto* item = market_list_->item(row)) item->setText(label);
    }

    const double chart_price = chart_asset_id_.endsWith(QStringLiteral(":no"))
        ? selected_.outcomes[1].price : selected_.outcomes[0].price;
    openmarketterminal::trading::Candle tick;
    tick.timestamp = QDateTime::currentMSecsSinceEpoch();
    tick.open = tick.high = tick.low = tick.close = chart_price;
    reference_chart_->append_candle(tick);
    live_chart_seeded_ = true;
    chart_status_->hide();
    if (!price_->hasFocus()) price_->setValue(chart_price);
    update_market_health();
}

void KalshiScreen::render_order_book(const pred::PredictionOrderBook& book) {
    if (book.asset_id.isEmpty()) return;
    kalshi_books_.insert(book.asset_id, book);
    observe_shadow_book(book);
    if (!has_selection_ || !selected_.key.asset_ids.contains(book.asset_id)) return;
    last_kalshi_book_ms_ = book.last_update_ms > 0
        ? book.last_update_ms : QDateTime::currentMSecsSinceEpoch();
    const int rows = qMax(book.bids.size(), book.asks.size());
    book_table_->setRowCount(rows);
    for (int row = 0; row < rows; ++row) {
        if (row < book.bids.size()) {
            book_table_->setItem(row, 0, new QTableWidgetItem(QString::number(book.bids[row].size, 'f', 2)));
            book_table_->setItem(row, 1, new QTableWidgetItem(probability(book.bids[row].price)));
        }
        if (row < book.asks.size()) {
            book_table_->setItem(row, 2, new QTableWidgetItem(probability(book.asks[row].price)));
            book_table_->setItem(row, 3, new QTableWidgetItem(QString::number(book.asks[row].size, 'f', 2)));
        }
    }
    update_market_health();
}

void KalshiScreen::update_market_health() {
    if (!quote_health_ || !recorder_health_) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto age_text = [now](qint64 ts) {
        if (ts <= 0) return QStringLiteral("WAITING");
        const double seconds = qMax<qint64>(0, now - ts) / 1000.0;
        return seconds < 60.0 ? QStringLiteral("%1s").arg(seconds, 0, 'f', 1)
                              : QStringLiteral("%1m").arg(seconds / 60.0, 0, 'f', 1);
    };
    const auto quote_text = [this](int index, const QString& label) {
        if (!has_selection_) return label + QStringLiteral(" —");
        const QString asset = selected_.key.asset_ids.value(index);
        const auto it = kalshi_books_.constFind(asset);
        if (it == kalshi_books_.cend()) return label + QStringLiteral(" WAITING");
        const double bid = it->bids.isEmpty() ? 0.0 : it->bids.first().price;
        const double ask = it->asks.isEmpty() ? 0.0 : it->asks.first().price;
        const double bid_size = it->bids.isEmpty() ? 0.0 : it->bids.first().size;
        const double ask_size = it->asks.isEmpty() ? 0.0 : it->asks.first().size;
        const QString spread = bid > 0.0 && ask > 0.0
            ? QStringLiteral(" · spread %1c").arg((ask - bid) * 100.0, 0, 'f', 1)
            : QString();
        return QStringLiteral("%1 %2c × %3 / %4c × %5%6")
            .arg(label).arg(bid * 100.0, 0, 'f', 1).arg(bid_size, 0, 'f', 0)
            .arg(ask * 100.0, 0, 'f', 1).arg(ask_size, 0, 'f', 0).arg(spread);
    };

    const qint64 book_age = last_kalshi_book_ms_ > 0 ? now - last_kalshi_book_ms_ : -1;
    const bool websocket = adapter() && adapter()->is_ws_connected();
    const bool live = websocket && book_age >= 0 && book_age <= 3'000;
    const bool delayed = websocket && book_age >= 0 && book_age <= 10'000;
    const QString state = live ? QStringLiteral("LIVE")
                               : delayed ? QStringLiteral("DELAYED") : QStringLiteral("STALE");
    const QString color = live ? colors::GREEN() : delayed ? colors::WARNING() : colors::RED();
    quote_health_->setText(QStringLiteral("KALSHI %1 · BOOK %2 · TICKER %3 · LAST TRADE %4\n%5\n%6")
                               .arg(state, age_text(last_kalshi_book_ms_), age_text(last_kalshi_ticker_ms_),
                                    age_text(last_kalshi_trade_ms_), quote_text(0, QStringLiteral("YES")),
                                    quote_text(1, QStringLiteral("NO"))));
    quote_health_->setStyleSheet(QStringLiteral("color:%1;background:%2;border:1px solid %1;padding:7px;font-weight:800;")
                                     .arg(color, colors::BG_RAISED()));

    const qint64 uptime = qMax<qint64>(0, (now - recorder_started_ms_) / 1000);
    const qint64 dom_age = reference_dom_last_update_ms_ > 0 ? now - reference_dom_last_update_ms_ : -1;
    const QString dom_state = dom_age >= 0 && dom_age <= 3'000
        ? QStringLiteral("LIVE %1").arg(age_text(reference_dom_last_update_ms_))
        : QStringLiteral("STALE %1").arg(age_text(reference_dom_last_update_ms_));
    recorder_health_->setText(QStringLiteral("RECORDER ACTIVE %1 · DOM %2\n%3 tickers · %4 trades · %5 book batches · %6 ladder snapshots")
                                  .arg(duration_text(uptime), dom_state)
                                  .arg(ticker_events_recorded_).arg(trade_events_recorded_)
                                  .arg(book_batches_recorded_).arg(ladder_snapshots_recorded_));
}

QString KalshiScreen::evidence_path(const QString& filename) const {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
           QStringLiteral("/") + filename;
}

void KalshiScreen::record_kalshi_trade(const pred::PredictionTrade& trade) {
    QJsonObject row{{QStringLiteral("event"), QStringLiteral("kalshi_trade")},
                    {QStringLiteral("ts"), QDateTime::fromMSecsSinceEpoch(trade.ts_ms).toUTC().toString(Qt::ISODateWithMs)},
                    {QStringLiteral("asset_id"), trade.asset_id},
                    {QStringLiteral("side"), trade.side},
                    {QStringLiteral("price"), trade.price},
                    {QStringLiteral("size"), trade.size},
                    {QStringLiteral("live_eligible"), false}};
    if (kalshi_data::KalshiEvidenceEngine::append_jsonl(
            evidence_path(QStringLiteral("kalshi-trades.jsonl")), row))
        ++trade_events_recorded_;
    if (has_selection_ && selected_.key.asset_ids.contains(trade.asset_id))
        last_kalshi_trade_ms_ = trade.ts_ms > 0 ? trade.ts_ms : QDateTime::currentMSecsSinceEpoch();
    if (!has_selection_ || chart_asset_id_.isEmpty() ||
        trade.asset_id != selected_.key.asset_ids.first() || trade.price <= 0.0) return;
    openmarketterminal::trading::Candle tick;
    tick.timestamp = trade.ts_ms > 0 ? trade.ts_ms : QDateTime::currentMSecsSinceEpoch();
    const double chart_price = chart_asset_id_.endsWith(QStringLiteral(":no"))
        ? 1.0 - trade.price : trade.price;
    tick.open = tick.high = tick.low = tick.close = chart_price;
    reference_chart_->append_candle(tick);
    live_chart_seeded_ = true;
    chart_status_->hide();
}

void KalshiScreen::record_ladder_evidence() {
    if (!orderbook_event_buffer_.isEmpty()) {
        const auto buffered = orderbook_event_buffer_;
        orderbook_event_buffer_.clear();
        const QString received = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        for (auto it = buffered.cbegin(); it != buffered.cend(); ++it) {
            QJsonObject row{{QStringLiteral("event"), QStringLiteral("kalshi_orderbook_event_batch")},
                            {QStringLiteral("received_ts"), received},
                            {QStringLiteral("market_ticker"), it.key()},
                            {QStringLiteral("events"), it.value()},
                            {QStringLiteral("live_eligible"), false}};
            if (kalshi_data::KalshiEvidenceEngine::append_jsonl(
                    evidence_path(QStringLiteral("kalshi-book-events.jsonl")), row))
                ++book_batches_recorded_;
        }
    }
    const QString features = evidence_path(QStringLiteral("kalshi-venue-features.jsonl"));
    const QString labels = evidence_path(QStringLiteral("kalshi-venue-labels.jsonl"));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - last_forward_reconcile_ms_ >= 60'000) {
        last_forward_reconcile_ms_ = now;
        kalshi_data::KalshiEvidenceEngine::reconcile_forward_labels(features, labels);
    }
    if (!has_selection_ || selected_.key.event_id.isEmpty()) return;
    if (now - last_ladder_snapshot_ms_ < 5'000) return;
    last_ladder_snapshot_ms_ = now;
    const QJsonObject snapshot = kalshi_data::KalshiEvidenceEngine::ladder_snapshot(
        all_markets_, kalshi_books_, selected_.key.event_id, now);
    if (kalshi_data::KalshiEvidenceEngine::append_jsonl(
            evidence_path(QStringLiteral("kalshi-ladders.jsonl")), snapshot))
        ++ladder_snapshots_recorded_;
    const QJsonArray diagnostics = snapshot.value(QStringLiteral("diagnostics")).toArray();
    int opportunities = 0;
    int warnings = 0;
    QString top;
    for (const auto& value : diagnostics) {
        const auto row = value.toObject();
        if (row.value(QStringLiteral("severity")).toString() == QStringLiteral("opportunity")) ++opportunities;
        else ++warnings;
        if (top.isEmpty()) top = row.value(QStringLiteral("detail")).toString();
    }
    ladder_status_->setText(QStringLiteral("LADDER ENGINE ACTIVE\n%1 contracts · %2 executable opportunities · %3 diagnostics%4")
                                .arg(snapshot.value(QStringLiteral("contracts")).toArray().size())
                                .arg(opportunities).arg(warnings)
                                .arg(top.isEmpty() ? QString() : QStringLiteral("\n") + top));
    ladder_status_->setStyleSheet(QStringLiteral("color:%1;border-top:1px solid %2;padding-top:8px;")
                                      .arg(opportunities > 0 ? colors::GREEN() : colors::TEXT_SECONDARY(),
                                           colors::BORDER_DIM()));
    render_ladder_diagnostics(diagnostics);
}

void KalshiScreen::render_ladder_diagnostics(const QJsonArray& diagnostics) {
    if (!ladder_table_) return;
    QVector<QJsonObject> rows;
    rows.reserve(diagnostics.size());
    for (const auto& value : diagnostics) rows.append(value.toObject());
    const auto rank = [](const QString& severity) {
        if (severity == QStringLiteral("opportunity")) return 0;
        if (severity == QStringLiteral("warning")) return 1;
        return 2;
    };
    std::stable_sort(rows.begin(), rows.end(), [rank](const auto& left, const auto& right) {
        const int severity = rank(left.value(QStringLiteral("severity")).toString()) -
                             rank(right.value(QStringLiteral("severity")).toString());
        if (severity != 0) return severity < 0;
        return left.value(QStringLiteral("net_edge")).toDouble() >
               right.value(QStringLiteral("net_edge")).toDouble();
    });
    ladder_table_->setRowCount(qMin(12, rows.size()));
    if (rows.isEmpty()) {
        ladder_table_->setRowCount(1);
        ladder_table_->setItem(0, 0, new QTableWidgetItem(QStringLiteral("CLEAR")));
        ladder_table_->setItem(0, 3, new QTableWidgetItem(
            QStringLiteral("No ladder inconsistency survives current prices, fees, and displayed depth.")));
        return;
    }
    for (int row = 0; row < ladder_table_->rowCount(); ++row) {
        const auto diagnostic = rows[row];
        const QString severity = diagnostic.value(QStringLiteral("severity")).toString();
        const QString edge = diagnostic.value(QStringLiteral("net_edge")).toDouble() > 0.0
            ? QStringLiteral("%1c").arg(diagnostic.value(QStringLiteral("net_edge")).toDouble() * 100.0, 0, 'f', 1)
            : QStringLiteral("—");
        const QString tickers = diagnostic.value(QStringLiteral("tickers")).toArray().toVariantList().isEmpty()
            ? QString() : QStringLiteral(" · ") + diagnostic.value(QStringLiteral("tickers")).toArray().first().toString();
        const QStringList values{severity.toUpper(), edge,
                                 diagnostic.value(QStringLiteral("kind")).toString().replace(QLatin1Char('_'), QLatin1Char(' ')).toUpper(),
                                 diagnostic.value(QStringLiteral("detail")).toString() + tickers};
        const QColor foreground(severity == QStringLiteral("opportunity") ? colors::GREEN()
                                : severity == QStringLiteral("warning") ? colors::WARNING()
                                : colors::TEXT_SECONDARY());
        for (int column = 0; column < values.size(); ++column) {
            auto* item = new QTableWidgetItem(values[column]);
            item->setForeground(foreground);
            ladder_table_->setItem(row, column, item);
        }
    }
}

void KalshiScreen::reconcile_settlement(const pred::PredictionMarket& market) {
    if (!settlement_index_loaded_) {
        settlement_index_loaded_ = true;
        QFile existing(evidence_path(QStringLiteral("kalshi-settlements.jsonl")));
        if (existing.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!existing.atEnd()) {
                const QJsonDocument document = QJsonDocument::fromJson(existing.readLine());
                if (document.isObject()) {
                    const QString ticker = document.object().value(QStringLiteral("kalshi_market_id")).toString();
                    if (!ticker.isEmpty()) reconciled_settlements_.insert(ticker);
                }
            }
        }
    }
    if (market.key.market_id.isEmpty() || reconciled_settlements_.contains(market.key.market_id)) return;
    const QJsonObject label = kalshi_data::KalshiEvidenceEngine::settlement_label(
        market, evidence_path(QStringLiteral("kalshi-venue-features.jsonl")));
    if (kalshi_data::KalshiEvidenceEngine::append_jsonl(
            evidence_path(QStringLiteral("kalshi-settlements.jsonl")), label))
        reconciled_settlements_.insert(market.key.market_id);
}

void KalshiScreen::render_trades(const QVector<pred::PredictionTrade>& trades) {
    trades_table_->setRowCount(trades.size());
    for (int row = 0; row < trades.size(); ++row) {
        const auto& trade = trades[row];
        trades_table_->setItem(row, 0, new QTableWidgetItem(QDateTime::fromMSecsSinceEpoch(trade.ts_ms).toString(QStringLiteral("hh:mm:ss"))));
        trades_table_->setItem(row, 1, new QTableWidgetItem(trade.side));
        trades_table_->setItem(row, 2, new QTableWidgetItem(probability(trade.price)));
        trades_table_->setItem(row, 3, new QTableWidgetItem(QString::number(trade.size, 'f', 2)));
    }
}

void KalshiScreen::preview_order() {
    if (!has_selection_) { QMessageBox::information(this, QStringLiteral("Kalshi"), QStringLiteral("Select a contract first.")); return; }
    const double p = price_->value();
    const int n = contracts_->value();
    const double fee = kalshi_data::KalshiEvidenceEngine::conservative_taker_fee(selected_, p, n);
    QMessageBox::information(this, QStringLiteral("Maker order preview"),
        QStringLiteral("%1 %2\n%3 contracts at %4\nNotional %5\nMaximum fee estimate %6\nMaximum loss %7\nPayout if correct %8\n\nLIVE SUBMISSION LOCKED\nThe maker evidence gate requires 100 independent markets across 14 days.")
            .arg(QStringLiteral("BUY"), side_).arg(n).arg(probability(p)).arg(money(p * n)).arg(money(fee))
            .arg(money(p * n + fee)).arg(money(n)));
}

void KalshiScreen::toggle_shadow_collector() {
    shadow_enabled_ = !shadow_enabled_;
    shadow_button_->setText(shadow_enabled_ ? QStringLiteral("PAUSE SHADOW") : QStringLiteral("RESUME SHADOW"));
    shadow_status_->setText(shadow_enabled_
        ? QStringLiteral("AUTO SHADOW ACTIVE\n%1 candidates · %2 confirmed trade-throughs\nNo orders are submitted.")
              .arg(shadow_candidates_).arg(shadow_confirmed_)
        : QStringLiteral("SHADOW PAUSED\nNo observations are being recorded."));
}

void KalshiScreen::observe_shadow_book(const pred::PredictionOrderBook& book) {
    if (!shadow_enabled_ || book.asset_id.isEmpty()) return;
    if (book.bids.isEmpty()) return;
    const double bid = book.bids.first().price;
    const double queue = book.bids.first().size;
    auto it = shadow_quotes_.find(book.asset_id);
    if (it == shadow_quotes_.end()) {
        if (bid < 0.85 || bid > 0.92) return;
        shadow_quotes_.insert(book.asset_id, ShadowQuote{bid, queue + 1.0, queue + 1.0});
        ++shadow_candidates_;
        append_shadow_event(QStringLiteral("shadow_maker_candidate"), book.asset_id, bid, queue + 1.0);
    } else {
        it->smallest_queue = qMin(it->smallest_queue, bid == it->price ? queue : 0.0);
        const bool depleted = it->smallest_queue <= 0.0;
        const bool traded_through = !book.asks.isEmpty() && book.asks.first().price <= it->price;
        if (depleted && traded_through) {
            ++shadow_confirmed_;
            append_shadow_event(QStringLiteral("shadow_maker_fill"), book.asset_id, it->price,
                                it->initial_queue, QStringLiteral("queue_depleted_and_book_traded_through"));
            shadow_quotes_.erase(it);
        }
    }
    shadow_status_->setText(QStringLiteral("AUTO SHADOW ACTIVE\n%1 candidates · %2 confirmed trade-throughs\nNo orders are submitted.")
                                .arg(shadow_candidates_).arg(shadow_confirmed_));
}

void KalshiScreen::append_shadow_event(const QString& event, const QString& asset_id,
                                       double quote_price, double queue, const QString& confirmation) {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(directory);
    QFile file(directory + QStringLiteral("/kalshi-shadow-maker.jsonl"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    QJsonObject row{{QStringLiteral("event"), event},
                    {QStringLiteral("execution_style"), QStringLiteral("shadow_maker")},
                    {QStringLiteral("asset_id"), asset_id},
                    {QStringLiteral("quote_price"), quote_price},
                    {QStringLiteral("initial_queue"), queue},
                    {QStringLiteral("ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                    {QStringLiteral("live_eligible"), false}};
    if (!confirmation.isEmpty()) row.insert(QStringLiteral("confirmation"), confirmation);
    file.write(QJsonDocument(row).toJson(QJsonDocument::Compact));
    file.write("\n");
}

void KalshiScreen::set_spot_symbol(const QString& asset) {
    spot_symbol_ = asset.trimmed().toUpper() + QStringLiteral("/USD");
    if (dom_title_) dom_title_->setText(
        QStringLiteral("%1 DOM · %2 · 24 L2").arg(reference_dom_venue_.toUpper(), spot_symbol_));
    if (spot_dom_) spot_dom_->clear();
    if (reference_chart_) {
        reference_chart_->clear();
        reference_chart_->set_probability_mode(true);
        reference_chart_->set_symbol(QStringLiteral("KALSHI CONTRACT"));
    }
    if (chart_status_) { chart_status_->setText(QStringLiteral("LOADING PRICE HISTORY")); chart_status_->show(); }
    if (dom_status_) { dom_status_->setText(QStringLiteral("CONNECTING TO SPOT BOOK")); dom_status_->show(); }
    if (venue_consensus_) {
        venue_consensus_->setText(QStringLiteral("VISUAL SPOT REFERENCE ONLY · CONNECTING KRAKEN + COINBASE"));
        venue_consensus_->setStyleSheet(QStringLiteral(
            "color:%1;background:%2;border:1px solid %3;padding:6px 10px;font-size:11px;font-weight:800;")
            .arg(colors::TEXT_SECONDARY(), colors::BG_RAISED(), colors::BORDER_DIM()));
    }
    chart_timeframe_ = QStringLiteral("live");
    live_chart_seeded_ = false;
    last_chart_fetch_ms_ = 0;
    reference_spot_ = 0.0;
    reference_spot_last_update_ms_ = 0;
    reference_spot_source_.clear();
    trend_anchor_spot_ = 0.0;
    trend_anchor_ms_ = 0;
    update_observation_strip();
    start_spot_dom_stream();
    refresh_spot_dom();
    refresh_venue_consensus();
    refresh_reference_chart();
}

void KalshiScreen::start_spot_dom_stream() {
    if (spot_symbol_.isEmpty()) return;
    if (!reference_dom_socket_) {
        reference_dom_socket_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
        connect(reference_dom_socket_, &QWebSocket::connected, this, &KalshiScreen::subscribe_reference_dom);
        connect(reference_dom_socket_, &QWebSocket::textMessageReceived,
                this, &KalshiScreen::handle_reference_dom_message);
        connect(reference_dom_socket_, &QWebSocket::disconnected, this, [this]() {
            if (dom_status_) {
                dom_status_->setText(QStringLiteral("%1 WS STALE · RECONNECTING")
                                         .arg(reference_dom_venue_.toUpper()));
                dom_status_->setStyleSheet(QStringLiteral("color:%1;font-size:10px;font-weight:800;")
                                               .arg(colors::WARNING()));
            }
            if (reference_dom_reconnect_timer_ && !reference_dom_reconnect_timer_->isActive())
                reference_dom_reconnect_timer_->start(1'000);
        });
        connect(reference_dom_socket_, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            if (dom_status_) dom_status_->setText(QStringLiteral("%1 WS ERROR · REST FALLBACK")
                                                       .arg(reference_dom_venue_.toUpper()));
        });
    }
    if (reference_dom_symbol_ != spot_symbol_) {
        reference_dom_symbol_ = spot_symbol_;
        reference_dom_bids_.clear();
        reference_dom_asks_.clear();
        reference_dom_last_update_ms_ = 0;
    }
    if (reference_dom_socket_->state() == QAbstractSocket::ConnectedState) {
        subscribe_reference_dom();
        return;
    }
    if (reference_dom_socket_->state() == QAbstractSocket::ConnectingState) return;
    const QUrl endpoint(reference_dom_venue_ == QStringLiteral("coinbase")
                            ? QStringLiteral("wss://ws-feed.exchange.coinbase.com")
                            : QStringLiteral("wss://ws.kraken.com/v2"));
    reference_dom_socket_->open(endpoint);
}

void KalshiScreen::subscribe_reference_dom() {
    if (!reference_dom_socket_ || reference_dom_socket_->state() != QAbstractSocket::ConnectedState) return;
    QJsonObject request;
    if (reference_dom_venue_ == QStringLiteral("coinbase")) {
        const QString product = QString(reference_dom_symbol_).replace(QLatin1Char('/'), QLatin1Char('-'));
        request.insert(QStringLiteral("type"), QStringLiteral("subscribe"));
        request.insert(QStringLiteral("product_ids"), QJsonArray{product});
        request.insert(QStringLiteral("channels"), QJsonArray{QStringLiteral("level2")});
    } else {
        request.insert(QStringLiteral("method"), QStringLiteral("subscribe"));
        request.insert(QStringLiteral("params"), QJsonObject{
            {QStringLiteral("channel"), QStringLiteral("book")},
            {QStringLiteral("symbol"), QJsonArray{reference_dom_symbol_}},
            {QStringLiteral("depth"), 25},
            {QStringLiteral("snapshot"), true}});
    }
    reference_dom_socket_->sendTextMessage(QJsonDocument(request).toJson(QJsonDocument::Compact));
    if (dom_status_) {
        dom_status_->setText(QStringLiteral("%1 WS CONNECTED · AWAITING BOOK")
                                 .arg(reference_dom_venue_.toUpper()));
        dom_status_->setStyleSheet(QStringLiteral("color:%1;font-size:10px;font-weight:800;")
                                       .arg(colors::WARNING()));
    }
}

void KalshiScreen::handle_reference_dom_message(const QString& message) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return;
    const QJsonObject root = document.object();
    const auto update_level = [](QMap<double, double>& levels, double price, double quantity) {
        if (price <= 0.0) return;
        if (quantity <= 0.0) levels.remove(price); else levels.insert(price, quantity);
    };

    if (reference_dom_venue_ == QStringLiteral("coinbase")) {
        const QString product = QString(reference_dom_symbol_).replace(QLatin1Char('/'), QLatin1Char('-'));
        if (root.value(QStringLiteral("product_id")).toString() != product) return;
        const QString type = root.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("snapshot")) {
            reference_dom_bids_.clear();
            reference_dom_asks_.clear();
            for (const auto& value : root.value(QStringLiteral("bids")).toArray()) {
                const auto pair = value.toArray();
                if (pair.size() >= 2) update_level(reference_dom_bids_, pair[0].toVariant().toDouble(), pair[1].toVariant().toDouble());
            }
            for (const auto& value : root.value(QStringLiteral("asks")).toArray()) {
                const auto pair = value.toArray();
                if (pair.size() >= 2) update_level(reference_dom_asks_, pair[0].toVariant().toDouble(), pair[1].toVariant().toDouble());
            }
        } else if (type == QStringLiteral("l2update")) {
            for (const auto& value : root.value(QStringLiteral("changes")).toArray()) {
                const auto change = value.toArray();
                if (change.size() < 3) continue;
                QMap<double, double>& side = change[0].toString() == QStringLiteral("buy")
                    ? reference_dom_bids_ : reference_dom_asks_;
                update_level(side, change[1].toVariant().toDouble(), change[2].toVariant().toDouble());
            }
        } else {
            return;
        }
    } else {
        if (root.value(QStringLiteral("channel")).toString() != QStringLiteral("book")) return;
        const QString type = root.value(QStringLiteral("type")).toString();
        const QJsonArray data = root.value(QStringLiteral("data")).toArray();
        if (data.isEmpty()) return;
        const QJsonObject payload = data.first().toObject();
        if (payload.value(QStringLiteral("symbol")).toString() != reference_dom_symbol_) return;
        if (type == QStringLiteral("snapshot")) {
            reference_dom_bids_.clear();
            reference_dom_asks_.clear();
        } else if (type != QStringLiteral("update")) {
            return;
        }
        for (const auto& value : payload.value(QStringLiteral("bids")).toArray()) {
            const auto level = value.toObject();
            update_level(reference_dom_bids_, level.value(QStringLiteral("price")).toVariant().toDouble(),
                         level.value(QStringLiteral("qty")).toVariant().toDouble());
        }
        for (const auto& value : payload.value(QStringLiteral("asks")).toArray()) {
            const auto level = value.toObject();
            update_level(reference_dom_asks_, level.value(QStringLiteral("price")).toVariant().toDouble(),
                         level.value(QStringLiteral("qty")).toVariant().toDouble());
        }
    }

    while (reference_dom_bids_.size() > 100) reference_dom_bids_.erase(reference_dom_bids_.begin());
    while (reference_dom_asks_.size() > 100) {
        auto last = reference_dom_asks_.end();
        reference_dom_asks_.erase(--last);
    }
    openmarketterminal::trading::OrderBookData book;
    book.symbol = reference_dom_symbol_;
    for (auto it = reference_dom_bids_.constEnd(); it != reference_dom_bids_.constBegin() && book.bids.size() < 25;) {
        --it;
        book.bids.append(qMakePair(it.key(), it.value()));
    }
    for (auto it = reference_dom_asks_.constBegin(); it != reference_dom_asks_.constEnd() && book.asks.size() < 25; ++it)
        book.asks.append(qMakePair(it.key(), it.value()));
    if (book.bids.isEmpty() || book.asks.isEmpty()) return;
    book.best_bid = book.bids.first().first;
    book.best_ask = book.asks.first().first;
    book.spread = book.best_ask - book.best_bid;
    book.spread_pct = book.best_bid > 0.0 ? book.spread / book.best_bid * 100.0 : 0.0;
    reference_dom_last_update_ms_ = QDateTime::currentMSecsSinceEpoch();
    render_spot_book(book, reference_dom_venue_.toUpper() + QStringLiteral(" WS"));
}

void KalshiScreen::set_reference_dom_venue(const QString& venue) {
    const QString normalized = venue.toLower();
    if (normalized != QStringLiteral("kraken") && normalized != QStringLiteral("coinbase")) return;
    if (reference_dom_venue_ == normalized && reference_dom_socket_ &&
        reference_dom_socket_->state() != QAbstractSocket::UnconnectedState) return;
    reference_dom_venue_ = normalized;
    if (kraken_dom_button_) kraken_dom_button_->setChecked(normalized == QStringLiteral("kraken"));
    if (coinbase_dom_button_) coinbase_dom_button_->setChecked(normalized == QStringLiteral("coinbase"));
    reference_dom_bids_.clear();
    reference_dom_asks_.clear();
    reference_dom_last_update_ms_ = 0;
    if (spot_dom_) spot_dom_->clear();
    if (dom_title_) dom_title_->setText(
        QStringLiteral("%1 DOM · %2 · 24 L2").arg(normalized.toUpper(), spot_symbol_));
    if (reference_dom_socket_ && reference_dom_socket_->state() != QAbstractSocket::UnconnectedState)
        reference_dom_socket_->close();
    start_spot_dom_stream();
}

void KalshiScreen::render_spot_book(const openmarketterminal::trading::OrderBookData& book, const QString& source) {
    if (!spot_dom_) return;
    const QString expected_base = spot_symbol_.section(QLatin1Char('/'), 0, 0).toUpper();
    const QString returned_base = book.symbol.section(QLatin1Char('/'), 0, 0).toUpper();
    const bool symbol_matches = returned_base.isEmpty() || returned_base == expected_base;
    if (!symbol_matches || (book.bids.isEmpty() && book.asks.isEmpty())) return;

    spot_dom_->set_data(book.bids, book.asks, book.spread, book.spread_pct);
    const double bid = book.best_bid > 0.0 ? book.best_bid
        : (!book.bids.isEmpty() ? book.bids.first().first : 0.0);
    const double ask = book.best_ask > 0.0 ? book.best_ask
        : (!book.asks.isEmpty() ? book.asks.first().first : 0.0);
    const double midpoint = bid > 0.0 && ask > 0.0 ? (bid + ask) / 2.0 : qMax(bid, ask);
    if (midpoint > 0.0) {
        const qint64 tick_now = QDateTime::currentMSecsSinceEpoch();
        reference_spot_ = midpoint;
        reference_spot_last_update_ms_ = tick_now;
        reference_spot_source_ = source;
        if (trend_anchor_spot_ <= 0.0 || tick_now - trend_anchor_ms_ >= 60'000) {
            trend_anchor_spot_ = midpoint;
            trend_anchor_ms_ = tick_now;
        }
        update_observation_strip();
    }
    dom_status_->setText(QStringLiteral("%1 · %2")
                             .arg(source, QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss"))));
    dom_status_->setStyleSheet(QStringLiteral("color:%1;font-size:10px;font-weight:800;")
                                   .arg(colors::POSITIVE()));
    dom_status_->show();
}

void KalshiScreen::refresh_spot_dom() {
    if (!spot_dom_ || dom_fetching_.exchange(true)) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (reference_dom_last_update_ms_ > 0 && now - reference_dom_last_update_ms_ < 2'500) {
        dom_fetching_ = false;
        return;
    }
    const QString venue = reference_dom_venue_;
    auto* session = openmarketterminal::trading::ExchangeSessionManager::instance().session(venue);
    QPointer<KalshiScreen> self(this);
    const QString symbol = spot_symbol_;
    (void)QtConcurrent::run([self, symbol, venue, session]() {
        const auto book = fetch_public_book(session, symbol);
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, symbol, venue, book]() {
            if (!self) return;
            self->dom_fetching_ = false;
            if (symbol != self->spot_symbol_) return;
            if (!book.bids.isEmpty() || !book.asks.isEmpty()) {
                self->render_spot_book(book, venue.toUpper() + QStringLiteral(" REST FALLBACK"));
            } else {
                self->dom_status_->setText(QStringLiteral("%1 WS STALE · WAITING FOR REST BOOK")
                                               .arg(venue.toUpper()));
                self->dom_status_->setStyleSheet(QStringLiteral("color:%1;font-size:10px;font-weight:800;")
                                                     .arg(colors::WARNING()));
                self->dom_status_->show();
            }
        }, Qt::QueuedConnection);
    });
}

void KalshiScreen::refresh_venue_consensus() {
    if (!venue_consensus_ || consensus_fetching_.exchange(true)) return;

    // Explicit sessions keep reference-data collection separate from the active
    // execution venue. These calls use public books only; no account or order API
    // is touched here.
    auto* kraken = openmarketterminal::trading::ExchangeSessionManager::instance().session(QStringLiteral("kraken"));
    auto* coinbase = openmarketterminal::trading::ExchangeSessionManager::instance().session(QStringLiteral("coinbase"));
    QPointer<KalshiScreen> self(this);
    const QString symbol = spot_symbol_;
    (void)QtConcurrent::run([self, symbol, kraken, coinbase]() {
        const auto kraken_snapshot = summarize_venue_book(QStringLiteral("Kraken"), fetch_public_book(kraken, symbol));
        const auto coinbase_snapshot = summarize_venue_book(QStringLiteral("Coinbase"), fetch_public_book(coinbase, symbol));
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, symbol, kraken_snapshot, coinbase_snapshot]() {
            if (!self) return;
            self->consensus_fetching_ = false;
            if (symbol != self->spot_symbol_) return;

            const QVector<VenueSnapshot> valid = [&]() {
                QVector<VenueSnapshot> result;
                if (kraken_snapshot.valid) result.append(kraken_snapshot);
                if (coinbase_snapshot.valid) result.append(coinbase_snapshot);
                return result;
            }();
            if (valid.isEmpty()) {
                self->venue_consensus_->setText(
                    QStringLiteral("VISUAL SPOT REFERENCE ONLY · WAITING FOR KRAKEN / COINBASE PUBLIC BOOKS"));
                self->venue_consensus_->setStyleSheet(QStringLiteral(
                    "color:%1;background:%2;border:1px solid %3;padding:6px 10px;font-size:11px;font-weight:800;")
                    .arg(colors::WARNING(), colors::BG_RAISED(), colors::BORDER_DIM()));
                return;
            }

            double mean_mid = 0.0;
            double mean_imbalance = 0.0;
            for (const auto& snapshot : valid) {
                mean_mid += snapshot.mid;
                mean_imbalance += snapshot.imbalance;
            }
            mean_mid /= valid.size();
            mean_imbalance /= valid.size();
            const bool aligned_up = valid.size() == 2 && kraken_snapshot.imbalance >= 0.10
                && coinbase_snapshot.imbalance >= 0.10;
            const bool aligned_down = valid.size() == 2 && kraken_snapshot.imbalance <= -0.10
                && coinbase_snapshot.imbalance <= -0.10;
            const QString observation = aligned_up ? QStringLiteral("ALIGNED UPWARD DEPTH")
                : aligned_down ? QStringLiteral("ALIGNED DOWNWARD DEPTH")
                : QStringLiteral("MIXED DEPTH");
            const QString direction = mean_imbalance >= 0.0 ? QStringLiteral("+") : QString();
            const QColor tone = aligned_up ? QColor(colors::POSITIVE())
                : aligned_down ? QColor(colors::NEGATIVE()) : QColor(colors::WARNING());
            const QString kraken_text = kraken_snapshot.valid
                ? QStringLiteral("KRAKEN $%1 I %2%3")
                      .arg(kraken_snapshot.mid, 0, 'f', 2).arg(kraken_snapshot.imbalance >= 0.0 ? "+" : QString())
                      .arg(kraken_snapshot.imbalance, 0, 'f', 2)
                : QStringLiteral("KRAKEN WAITING");
            const QString coinbase_text = coinbase_snapshot.valid
                ? QStringLiteral("COINBASE $%1 I %2%3")
                      .arg(coinbase_snapshot.mid, 0, 'f', 2).arg(coinbase_snapshot.imbalance >= 0.0 ? "+" : QString())
                      .arg(coinbase_snapshot.imbalance, 0, 'f', 2)
                : QStringLiteral("COINBASE WAITING");
            self->venue_consensus_->setText(QStringLiteral(
                "VISUAL SPOT REFERENCE ONLY · %1 · %2 · %3 · AVG I %4%5")
                .arg(kraken_text, coinbase_text, observation, direction)
                .arg(mean_imbalance, 0, 'f', 2));
            self->venue_consensus_->setStyleSheet(QStringLiteral(
                "color:%1;background:%2;border:1px solid %3;padding:6px 10px;font-size:11px;font-weight:800;")
                .arg(tone.name(), colors::BG_RAISED(), colors::BORDER_DIM()));

            // The external spot number is display-only. Keep it fresh from the
            // venue consensus when the selected DOM stream is delayed.
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (self->reference_dom_last_update_ms_ <= 0 ||
                now - self->reference_dom_last_update_ms_ >= 2'500) {
                self->reference_spot_ = mean_mid;
                self->reference_spot_last_update_ms_ = now;
                self->reference_spot_source_ = QStringLiteral("KRAKEN + COINBASE");
                self->update_observation_strip();
            }

            // The recorder is intentionally sparse enough for week-long local
            // evidence collection. Future tooling joins this row to 15s/1m/5m
            // forward moves, Kalshi price evolution, and final settlement.
            if (now - self->last_consensus_snapshot_ms_ < 5'000) return;
            self->last_consensus_snapshot_ms_ = now;
            const QDateTime close = self->has_selection_
                ? QDateTime::fromString(self->selected_.end_date_iso, Qt::ISODate) : QDateTime{};
            QJsonObject row{{QStringLiteral("event"), QStringLiteral("kalshi_venue_feature_snapshot")},
                            {QStringLiteral("ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                            {QStringLiteral("symbol"), symbol},
                            {QStringLiteral("reference_mid"), mean_mid},
                            {QStringLiteral("mean_top20_imbalance"), mean_imbalance},
                            {QStringLiteral("observation"), observation},
                            {QStringLiteral("labels_pending"), QStringLiteral("spot_15s,spot_1m,spot_5m,kalshi_price,settlement")},
                            {QStringLiteral("live_eligible"), false}};
            if (self->has_selection_) {
                row.insert(QStringLiteral("kalshi_market_id"), self->selected_.key.market_id);
                row.insert(QStringLiteral("kalshi_yes_price"), outcome_price(self->selected_, 0));
                row.insert(QStringLiteral("kalshi_no_price"), outcome_price(self->selected_, 1));
                row.insert(QStringLiteral("seconds_to_close"), close.isValid()
                    ? QDateTime::currentDateTimeUtc().secsTo(close.toUTC()) : -1);
            }
            const auto as_json = [](const VenueSnapshot& snapshot) {
                return QJsonObject{{QStringLiteral("mid"), snapshot.mid},
                                   {QStringLiteral("bid"), snapshot.bid},
                                   {QStringLiteral("ask"), snapshot.ask},
                                   {QStringLiteral("top20_bid_notional"), snapshot.bid_notional},
                                   {QStringLiteral("top20_ask_notional"), snapshot.ask_notional},
                                   {QStringLiteral("top20_imbalance"), snapshot.imbalance},
                                   {QStringLiteral("available"), snapshot.valid}};
            };
            row.insert(QStringLiteral("kraken"), as_json(kraken_snapshot));
            row.insert(QStringLiteral("coinbase"), as_json(coinbase_snapshot));
            append_venue_feature_snapshot(row);
        }, Qt::QueuedConnection);
    });
}

void KalshiScreen::set_chart_timeframe(const QString& timeframe) {
    chart_timeframe_ = timeframe.toLower();
    last_chart_fetch_ms_ = 0;
    if (reference_chart_) reference_chart_->set_timeframe(chart_timeframe_);
    if (chart_status_) {
        chart_status_->setText(QStringLiteral("FETCHING %1 KALSHI CONTRACT HISTORY")
                                   .arg(chart_timeframe_.toUpper()));
        chart_status_->show();
    }
    refresh_reference_chart();
}

void KalshiScreen::refresh_reference_chart() {
    if (!reference_chart_ || !has_selection_ || chart_asset_id_.isEmpty() ||
        chart_fetching_.exchange(true)) return;
    update_observation_strip();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (last_chart_fetch_ms_ > 0 && now - last_chart_fetch_ms_ < 30'000) {
        chart_fetching_ = false;
        return;
    }
    QString interval = QStringLiteral("1min");
    if (chart_timeframe_ == QStringLiteral("1h")) interval = QStringLiteral("1h");
    else if (chart_timeframe_ == QStringLiteral("1d")) interval = QStringLiteral("1d");
    last_chart_fetch_ms_ = now;
    adapter()->fetch_price_history(chart_asset_id_, interval, 100);
}

void KalshiScreen::update_strike_overlay() {
    if (!reference_chart_) return;
    reference_chart_->set_targets({});
}

void KalshiScreen::update_observation_strip() {
    if (!target_pulse_ || !spot_pulse_ || !clock_pulse_) return;
    const auto bounds = has_selection_ ? contract_bounds(selected_) : QVector<double>{};
    if (!bounds.isEmpty()) {
        if (bounds.size() == 1)
            target_pulse_->setText(QStringLiteral("TO BEAT\n$%1").arg(bounds[0], 0, 'f', 2));
        else
            target_pulse_->setText(QStringLiteral("RANGE\n$%1 – $%2")
                                       .arg(bounds[0], 0, 'f', 2).arg(bounds[1], 0, 'f', 2));
    } else {
        target_pulse_->setText(QStringLiteral("TO BEAT\n—"));
    }

    bool expiration_ok = false;
    const double expiration_value = has_selection_
        ? selected_.extras.value(QStringLiteral("expiration_value")).toString().toDouble(&expiration_ok)
        : 0.0;
    const QString result = has_selection_
        ? selected_.extras.value(QStringLiteral("result")).toString() : QString();
    if (expiration_ok && expiration_value > 0.0 && (!selected_.active || !result.isEmpty())) {
        spot_pulse_->setText(QStringLiteral("KALSHI FINAL\n$%1\nSETTLED %2")
                                 .arg(expiration_value, 0, 'f', 2)
                                 .arg(result.toUpper()));
        spot_pulse_->setToolTip(QStringLiteral("Official Kalshi expiration_value used for settlement."));
        spot_pulse_->setStyleSheet(QStringLiteral("color:%1;font-size:14px;font-weight:900;")
                                       .arg(colors::CYAN()));
    } else if (reference_spot_ > 0.0) {
        QString distance_text = QStringLiteral("LIVE");
        bool favorable = true;
        if (bounds.size() == 1) {
            const double dollars = reference_spot_ - bounds[0];
            const double pct = dollars / bounds[0] * 100.0;
            favorable = dollars >= 0.0;
            distance_text = QStringLiteral("%1$%2  (%3%4%)")
                                .arg(dollars >= 0.0 ? QStringLiteral("+") : QString())
                                .arg(dollars, 0, 'f', 2)
                                .arg(pct >= 0.0 ? QStringLiteral("+") : QString())
                                .arg(pct, 0, 'f', 3);
        } else if (bounds.size() > 1) {
            favorable = reference_spot_ >= bounds[0] && reference_spot_ <= bounds[1];
            if (favorable) {
                distance_text = QStringLiteral("INSIDE RANGE");
            } else {
                const double nearest = reference_spot_ < bounds[0] ? bounds[0] : bounds[1];
                const double dollars = reference_spot_ - nearest;
                distance_text = QStringLiteral("%1$%2 TO RANGE")
                                    .arg(dollars >= 0.0 ? QStringLiteral("+") : QString())
                                    .arg(dollars, 0, 'f', 2);
            }
        }
        const qint64 age_ms = reference_spot_last_update_ms_ > 0
            ? QDateTime::currentMSecsSinceEpoch() - reference_spot_last_update_ms_ : -1;
        const bool stale = age_ms < 0 || age_ms > 5'000;
        const QString source = reference_spot_source_.isEmpty()
            ? QStringLiteral("EXTERNAL") : reference_spot_source_;
        spot_pulse_->setText(QStringLiteral("EXTERNAL SPOT · %1%2\n$%3\n%4")
                                 .arg(source, stale ? QStringLiteral(" · STALE") : QString())
                                 .arg(reference_spot_, 0, 'f', 2).arg(distance_text));
        spot_pulse_->setToolTip(QStringLiteral(
            "Visual reference only. This external venue price is not Kalshi's settlement index and is not used by the ladder engine."));
        spot_pulse_->setStyleSheet(QStringLiteral("color:%1;font-size:14px;font-weight:900;")
                                       .arg(stale ? colors::WARNING()
                                                  : favorable ? colors::POSITIVE() : colors::NEGATIVE()));
    } else {
        spot_pulse_->setText(QStringLiteral("EXTERNAL SPOT\nWAITING FOR VISUAL REFERENCE"));
        spot_pulse_->setToolTip(QStringLiteral(
            "Kalshi's public Trade API does not expose the live underlying CF Benchmarks index. Official expiration_value appears after settlement."));
        spot_pulse_->setStyleSheet(QStringLiteral("color:%1;font-size:14px;font-weight:900;")
                                       .arg(colors::TEXT_SECONDARY()));
    }

    const QDateTime close = has_selection_ ? QDateTime::fromString(selected_.end_date_iso, Qt::ISODate) : QDateTime{};
    clock_pulse_->setText(close.isValid()
        ? QStringLiteral("CLOSE\n%1\n%2")
              .arg(duration_text(QDateTime::currentDateTimeUtc().secsTo(close.toUTC())),
                   close.toLocalTime().toString(QStringLiteral("h:mm ap")))
        : QStringLiteral("CLOSE\n—"));
}

} // namespace openmarketterminal::screens::kalshi
