#include "screens/kalshi/KalshiScreen.h"
#include "screens/kalshi/AdvisorCanaryPresentation.h"
#include "screens/kalshi/CliLocator.h"

#include "cli/ServeCommand.h"
#include "core/config/ProfileManager.h"
#include "core/events/EventBus.h"
#include "services/crypto/CoinbaseEndpoints.h"
#include "services/prediction/PredictionExchangeAdapter.h"
#include "services/prediction/PredictionCredentialStore.h"
#include "services/prediction/PredictionExchangeRegistry.h"
#include "services/prediction/kalshi/KalshiAdapter.h"
#include "services/prediction/kalshi/KalshiEvidenceEngine.h"
#include "services/prediction/kalshi/KalshiSettlementScoreboard.h"
#include "services/edge_radar/KalshiUniversalEdgeModel.h"
#include "services/edge_radar/KalshiAutoEngine.h"
#include "storage/repositories/EdgePredictionModelRepository.h"
#include "storage/repositories/SettingsRepository.h"
#include "storage/sqlite/Database.h"
#include "screens/crypto_trading/CryptoOrderBook.h"
#include "screens/common/DineroNetworkGadget.h"
#include "screens/polymarket/PredictionAccountDialog.h"
#include "trading/ExchangeService.h"
#include "trading/ExchangeSessionManager.h"
#include "ui/theme/Theme.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QAbstractItemView>
#include <QAbstractSocket>
#include <QColor>
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
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
#include <QProcess>
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
#include <QTimeZone>
#include <QTimer>
#include <QUuid>
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

constexpr int kContractDetailsRole = Qt::UserRole + 41;

void make_contract_link(QTableWidgetItem* item, const QJsonObject& details) {
    if (!item) return;
    item->setData(kContractDetailsRole,
                  QString::fromUtf8(QJsonDocument(details).toJson(QJsonDocument::Compact)));
    item->setForeground(QColor(colors::CYAN()));
    QFont font = item->font();
    font.setUnderline(true);
    item->setFont(font);
    item->setToolTip(QStringLiteral("Click to open the complete contract audit."));
}

QString cf_index_for_asset(const QString& asset) {
    const QString key = asset.trimmed().toUpper();
    if (key == QStringLiteral("BTC")) return QStringLiteral("BRTI");
    if (key == QStringLiteral("ETH")) return QStringLiteral("ETHUSD_RTI");
    if (key == QStringLiteral("SOL")) return QStringLiteral("SOLUSD_RTI");
    if (key == QStringLiteral("DOGE")) return QStringLiteral("DOGEUSD_RTI");
    return {};
}

QJsonObject fresh_btc_news_pulse(qint64 now_ms) {
    const QString path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                         QStringLiteral("/Open Terminal/Open Terminal/btc-news-pulse-latest.json");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return {};
    const QJsonObject pulse = document.object();
    const qint64 as_of = pulse.value(QStringLiteral("as_of_ms")).toString().toLongLong();
    if (as_of <= 0 || as_of > now_ms || now_ms - as_of > 2 * 60 * 60 * 1000) return {};
    return pulse;
}
QJsonObject fresh_btc_intelligence(qint64 now_ms) {
    const QString path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                         QStringLiteral("/Open Terminal/Open Terminal/btc-intelligence-latest.json");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return {};
    const QJsonObject intelligence = document.object();
    const qint64 as_of = intelligence.value(QStringLiteral("as_of_ms")).toString().toLongLong();
    if (as_of <= 0 || as_of > now_ms || now_ms - as_of > 2 * 60 * 60 * 1000) return {};
    return intelligence;
}
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
    kalshi_data::KalshiEvidenceEngine::append_jsonl(
        directory + QStringLiteral("/kalshi-venue-features.jsonl"), row);
}

QJsonObject read_json_object(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const auto document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}

QJsonObject read_last_jsonl_object(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text) || file.size() <= 0) return {};
    const qint64 window = qMin<qint64>(file.size(), 128 * 1024);
    file.seek(file.size() - window);
    const QList<QByteArray> lines = file.readAll().split('\n');
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        if (it->trimmed().isEmpty()) continue;
        const auto document = QJsonDocument::fromJson(*it);
        if (document.isObject()) return document.object();
    }
    return {};
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
        controls_ = new QWidget(this);
        auto* timeframes = new QHBoxLayout(controls_);
        timeframes->setContentsMargins(0, 0, 0, 0);
        timeframes->setSpacing(4);
        const QStringList labels{QStringLiteral("live"), QStringLiteral("1m"), QStringLiteral("5m"),
                                 QStringLiteral("15m"), QStringLiteral("1h"), QStringLiteral("1d")};
        for (const auto& label : labels) {
            auto* button = new QPushButton(label.toUpper(), controls_);
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
        layout->addWidget(controls_);
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
    void set_timeframe(const QString& timeframe) {
        const QString normalized = timeframe.toLower();
        plot_->set_window_seconds(window_seconds_for(normalized));
        if (!controls_) return;
        for (auto* button : controls_->findChildren<QPushButton*>())
            button->setChecked(button->property("timeframe").toString() == normalized);
    }
    void set_controls_visible(bool visible) { if (controls_) controls_->setVisible(visible); }

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
    QWidget* controls_ = nullptr;
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
        refresh_flow_meter();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (auto* a = adapter(); a && a->has_credentials() &&
            now - last_account_activity_fetch_ms_ >= 30'000) {
            last_account_activity_fetch_ms_ = now;
            a->fetch_user_activity(100);
        }
        if (now - last_live_status_fetch_ms_ >= 5'000) {
            last_live_status_fetch_ms_ = now;
            refresh_live_automation_status();
            refresh_daemon_status();
            refresh_advisor_canary_status();
            update_calibrator_readout();
        }
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
    refresh_account_status();
    refresh_daemon_status();
    refresh_advisor_canary_status();
    if (auto* kalshi = qobject_cast<kalshi_data::KalshiAdapter*>(adapter()))
        kalshi->subscribe_cf_benchmarks({cf_index_for_asset(asset_)});
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

void KalshiScreen::refresh_account_status() {
    if (!account_badge_ || !account_button_) return;

    const auto creds = pred::PredictionCredentialStore::load_kalshi();
    if (!creds) {
        account_button_->setText(QStringLiteral("CONNECT ACCOUNT"));
        account_badge_->setText(QStringLiteral("ACCOUNT: NOT CONNECTED"));
        account_badge_->setStyleSheet(QStringLiteral("color:%1;font-weight:800;padding:6px;")
                                          .arg(colors::TEXT_SECONDARY()));
        if (live_order_button_) live_order_button_->setText(QStringLiteral("CONNECT KALSHI TO TRADE"));
        return;
    }

    account_button_->setText(QStringLiteral("ACCOUNT"));
    const bool demo = creds->use_demo;
    account_badge_->setText(demo ? QStringLiteral("ACCOUNT: DEMO / PAPER")
                                 : QStringLiteral("ACCOUNT: LIVE CONFIGURED"));
    account_badge_->setStyleSheet(QStringLiteral("color:%1;font-weight:900;padding:6px;")
                                      .arg(demo ? colors::WARNING() : colors::GREEN()));
    if (live_order_button_)
        live_order_button_->setText(demo ? QStringLiteral("PLACE DEMO MAKER ORDER")
                                         : QStringLiteral("PLACE LIVE MAKER ORDER"));
}

void KalshiScreen::show_account_dialog() {
    auto* dialog = new polymarket::PredictionAccountDialog(this);
    dialog->set_active_exchange(QStringLiteral("kalshi"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    connect(dialog, &polymarket::PredictionAccountDialog::credentials_saved,
            this, [this](const QString& exchange_id) {
                if (exchange_id != QStringLiteral("kalshi")) return;
                if (auto* kalshi = qobject_cast<kalshi_data::KalshiAdapter*>(adapter())) {
                    const auto creds = pred::PredictionCredentialStore::load_kalshi();
                    kalshi->set_credentials(creds.value_or(kalshi_data::KalshiCredentials{}));
                    kalshi->subscribe_cf_benchmarks({cf_index_for_asset(asset_)});
                }
                refresh_account_status();
            });
    connect(dialog, &polymarket::PredictionAccountDialog::test_requested,
            dialog, [dialog](const QString& exchange_id) {
                if (exchange_id != QStringLiteral("kalshi")) return;
                auto* active = pred::PredictionExchangeRegistry::instance().adapter(QStringLiteral("kalshi"));
                if (!active) {
                    dialog->show_test_error(exchange_id, QObject::tr("Kalshi adapter is not registered."));
                    return;
                }
                dialog->show_test_running(exchange_id);
                QObject::connect(active, &pred::PredictionExchangeAdapter::balance_ready, dialog,
                                 [dialog, exchange_id](const pred::AccountBalance& balance) {
                                     dialog->show_test_success(
                                         exchange_id,
                                         QObject::tr("Kalshi balance: %1 %2")
                                             .arg(QString::number(balance.available, 'f', 2), balance.currency));
                                 },
                                 Qt::SingleShotConnection);
                QObject::connect(active, &pred::PredictionExchangeAdapter::error_occurred, dialog,
                                 [dialog, exchange_id](const QString& context, const QString& message) {
                                     dialog->show_test_error(exchange_id, context + QStringLiteral(": ") + message);
                                 },
                                 Qt::SingleShotConnection);
                active->fetch_balance();
            });
    dialog->open();
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
    account_button_ = new QPushButton(QStringLiteral("ACCOUNT"), command);
    account_button_->setToolTip(QStringLiteral("View or test this Kalshi account"));
    command_layout->addWidget(account_button_);
    account_badge_ = new QLabel(QStringLiteral("ACCOUNT: NOT CONNECTED"), command);
    account_badge_->setToolTip(QStringLiteral(
        "Credential state for this Kalshi workspace. Use ACCOUNT to test the live connection."));
    command_layout->addWidget(account_badge_);
    daemon_badge_ = new QLabel(QStringLiteral("DAEMON: CHECKING"), command);
    daemon_badge_->setToolTip(QStringLiteral(
        "The local daemon keeps Kalshi WebSocket monitoring, paper evidence, and armed automation running when the GUI is idle."));
    daemon_badge_->setStyleSheet(QStringLiteral("color:%1;font-weight:900;padding:6px;")
                                     .arg(colors::WARNING()));
    command_layout->addWidget(daemon_badge_);
    daemon_restart_button_ = new QPushButton(QStringLiteral("RESTART"), command);
    daemon_restart_button_->setToolTip(QStringLiteral(
        "Restart the local daemon. An active bounded trading session resumes after the daemon returns."));
    command_layout->addWidget(daemon_restart_button_);
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
    close_countdown_ = new QLabel(QStringLiteral("CLOSE IN —"), center);
    close_countdown_->setToolTip(QStringLiteral("Time remaining until the selected Kalshi contract closes."));
    close_countdown_->setStyleSheet(QStringLiteral("color:%1;font-size:13px;font-weight:900;padding:8px 0;")
                                        .arg(colors::WARNING()));
    quotes->addWidget(close_countdown_);
    center_layout->addLayout(quotes);
    calibrator_readout_ = new QLabel(QStringLiteral("CALIBRATOR · select a contract"), center);
    calibrator_readout_->setWordWrap(true);
    calibrator_readout_->setToolTip(QStringLiteral(
        "Advisory-only readout from the spot calibrator (calibrator.json): strike distance in "
        "sigmas, its calibrated P(YES), and its measured Brier record vs the market baseline. "
        "It never trades."));
    calibrator_readout_->setStyleSheet(QStringLiteral(
        "color:%1;background:%2;border:1px solid %3;padding:4px 8px;font-size:10px;font-weight:800;")
                                           .arg(colors::TEXT_SECONDARY(), colors::BG_RAISED(),
                                                colors::BORDER_DIM()));
    center_layout->addWidget(calibrator_readout_);

    reference_chart_ = new KalshiSimpleChart(center);
    reference_chart_->setMinimumHeight(180);
    reference_chart_->set_probability_mode(false);
    reference_chart_->set_symbol(spot_symbol_);
    reference_chart_->set_timeframe_changed([this](const QString& timeframe) {
        set_chart_timeframe(timeframe);
    });
    auto* chart_container = new QWidget(center);
    auto* chart_layout = new QVBoxLayout(chart_container);
    chart_layout->setContentsMargins(0, 0, 0, 0);
    chart_layout->setSpacing(6);
    auto* chart_title = new QLabel(QStringLiteral("SETTLEMENT RACE · INDEPENDENT SPOT VS KALSHI TARGET"),
                                   chart_container);
    chart_title->setStyleSheet(QStringLiteral("color:%1;background:%2;padding:5px 8px;font-weight:900;")
                                   .arg(colors::CYAN(), colors::BG_RAISED()));
    chart_layout->addWidget(chart_title);

    auto* chart_lanes = new QHBoxLayout;
    chart_lanes->setContentsMargins(0, 0, 0, 0);
    chart_lanes->setSpacing(6);

    auto* spot_lane = new QFrame(chart_container);
    spot_lane->setObjectName(QStringLiteral("ksSettlementSpotLane"));
    spot_lane->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* spot_lane_layout = new QVBoxLayout(spot_lane);
    spot_lane_layout->setContentsMargins(0, 0, 0, 0);
    spot_lane_layout->setSpacing(0);
    venue_consensus_ = new QLabel(QStringLiteral("VISUAL SPOT REFERENCE ONLY · CONNECTING KRAKEN + COINBASE"), spot_lane);
    venue_consensus_->setWordWrap(true);
    venue_consensus_->setToolTip(QStringLiteral(
        "Visual public order-book reference only. It does not set Kalshi contract prices, settlement values, or ladder decisions."));
    venue_consensus_->setStyleSheet(QStringLiteral("color:%1;background:%2;border:1px solid %3;padding:4px 8px;font-size:9px;font-weight:800;")
                                        .arg(colors::TEXT_SECONDARY(), colors::BG_RAISED(), colors::BORDER_DIM()));
    spot_lane_layout->addWidget(venue_consensus_);
    spot_lane_layout->addWidget(reference_chart_, 1);

    auto* contract_lane = new QFrame(chart_container);
    contract_lane->setObjectName(QStringLiteral("ksSettlementContractLane"));
    contract_lane->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* contract_lane_layout = new QVBoxLayout(contract_lane);
    contract_lane_layout->setContentsMargins(0, 0, 0, 0);
    contract_lane_layout->setSpacing(0);
    contract_strip_ = new QLabel(QStringLiteral("KALSHI CONTRACT · WAITING FOR EXECUTABLE BOOK"), contract_lane);
    contract_strip_->setWordWrap(true);
    contract_strip_->setStyleSheet(QStringLiteral(
        "color:%1;background:%2;border:1px solid %3;padding:5px 8px;font-size:10px;font-weight:900;")
                                      .arg(colors::TEXT_SECONDARY(), colors::BG_RAISED(), colors::BORDER_DIM()));
    contract_lane_layout->addWidget(contract_strip_);
    auto* contract_title = new QLabel(QStringLiteral("KALSHI CONTRACT HISTORY · YES / NO PROBABILITY"),
                                      contract_lane);
    contract_title->setStyleSheet(QStringLiteral("color:%1;background:%2;padding:4px 8px;font-size:10px;font-weight:900;")
                                       .arg(colors::CYAN(), colors::BG_RAISED()));
    contract_lane_layout->addWidget(contract_title);
    chart_status_ = new QLabel(QStringLiteral("LOADING KALSHI CONTRACT HISTORY"), contract_lane);
    chart_status_->setAlignment(Qt::AlignCenter);
    chart_status_->setStyleSheet(QStringLiteral("color:%1;background:%2;padding:4px;font-weight:800;")
                                     .arg(colors::WARNING(), colors::BG_RAISED()));
    contract_lane_layout->addWidget(chart_status_);
    contract_chart_ = new KalshiSimpleChart(contract_lane);
    contract_chart_->setMinimumHeight(180);
    contract_chart_->set_probability_mode(true);
    contract_chart_->set_timeframe_changed([this](const QString& timeframe) {
        set_chart_timeframe(timeframe);
    });
    contract_lane_layout->addWidget(contract_chart_, 1);

    chart_lanes->addWidget(spot_lane, 1);
    chart_lanes->addWidget(contract_lane, 1);
    chart_layout->addLayout(chart_lanes, 1);
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
    maker_layout->setSpacing(6);
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
    price_ = new QDoubleSpinBox(maker);
    price_->setRange(0.01, 0.99);
    price_->setSingleStep(0.01);
    price_->setDecimals(2);
    price_->setFixedWidth(120);
    contracts_ = new QSpinBox(maker);
    contracts_->setRange(1, 1000);
    contracts_->setValue(2);
    contracts_->setFixedWidth(92);
    auto* order_inputs = new QHBoxLayout;
    order_inputs->setContentsMargins(0, 0, 0, 0);
    order_inputs->setSpacing(7);
    auto* price_label = new QLabel(QStringLiteral("LIMIT PRICE"), maker);
    auto* contracts_label = new QLabel(QStringLiteral("CONTRACTS"), maker);
    for (auto* label : {price_label, contracts_label}) {
        label->setStyleSheet(QStringLiteral("color:%1;font-size:10px;font-weight:900;")
                                 .arg(colors::TEXT_SECONDARY()));
    }
    order_inputs->addWidget(price_label);
    order_inputs->addWidget(price_);
    order_inputs->addSpacing(10);
    order_inputs->addWidget(contracts_label);
    order_inputs->addWidget(contracts_);
    order_inputs->addStretch();
    maker_layout->addLayout(order_inputs);
    cost_label_ = new QLabel(QStringLiteral("Notional: —"), maker);
    fee_label_ = new QLabel(QStringLiteral("Max fee: —"), maker);
    payout_label_ = new QLabel(QStringLiteral("Payout if correct: —"), maker);
    auto* order_summary = new QHBoxLayout;
    order_summary->setContentsMargins(0, 0, 0, 0);
    order_summary->setSpacing(18);
    for (auto* label : {cost_label_, fee_label_, payout_label_}) {
        label->setWordWrap(false);
        label->setStyleSheet(QStringLiteral("color:%1;padding:2px 0;font-weight:800;")
                                 .arg(colors::TEXT_PRIMARY()));
        order_summary->addWidget(label);
    }
    order_summary->addStretch();
    maker_layout->addLayout(order_summary);
    auto* preview = new QPushButton(QStringLiteral("REVIEW ORDER"), maker);
    preview->setStyleSheet(QStringLiteral("QPushButton{background:%1;color:#00110d;border:0;padding:10px;font-weight:900;}").arg(colors::CYAN()));
    preview->setToolTip(QStringLiteral("Show cost, payout, and maximum-loss details without sending an order."));
    live_order_button_ = new QPushButton(QStringLiteral("CONNECT KALSHI TO TRADE"), maker);
    live_order_button_->setStyleSheet(QStringLiteral(
        "QPushButton{background:%1;color:%2;border:0;padding:10px;font-weight:900;}"
        "QPushButton:disabled{background:%3;color:%4;}")
                                          .arg(colors::POSITIVE(), colors::BG_BASE(), colors::BG_RAISED(), colors::TEXT_DIM()));
    live_order_button_->setToolTip(QStringLiteral(
        "Submit this post-only limit order after a final confirmation. Automated strategies remain separately locked."));
    auto* order_actions = new QHBoxLayout;
    order_actions->setContentsMargins(0, 0, 0, 0);
    order_actions->setSpacing(6);
    order_actions->addWidget(preview, 1);
    order_actions->addWidget(live_order_button_, 1);
    maker_layout->addLayout(order_actions);

    auto* account_separator = new QFrame(maker);
    account_separator->setFrameShape(QFrame::HLine);
    maker_layout->addWidget(account_separator);
    auto* account_title = new QLabel(QStringLiteral("YOUR KALSHI POSITION"), maker);
    account_title->setStyleSheet(QStringLiteral("font-weight:900;color:%1;").arg(colors::TEXT_PRIMARY()));
    maker_layout->addWidget(account_title);
    account_balance_label_ = new QLabel(QStringLiteral("AVAILABLE: connect a Kalshi account"), maker);
    position_label_ = new QLabel(QStringLiteral("POSITION: select a contract to load your position"), maker);
    cashout_label_ = new QLabel(QStringLiteral("CASH-OUT VALUE: waiting for position and executable bid"), maker);
    for (QLabel* label : {account_balance_label_, position_label_, cashout_label_}) {
        label->setWordWrap(true);
        label->setStyleSheet(QStringLiteral("color:%1;background:%2;border:1px solid %3;padding:7px;font-weight:800;")
                                 .arg(colors::TEXT_SECONDARY(), colors::BG_RAISED(), colors::BORDER_DIM()));
        maker_layout->addWidget(label);
    }
    cashout_button_ = new QPushButton(QStringLiteral("CASH OUT POSITION"), maker);
    cashout_button_->setEnabled(false);
    cashout_button_->setToolTip(QStringLiteral(
        "Place a reduce-only immediate-or-cancel exit at the displayed executable bid."));
    cashout_button_->setStyleSheet(QStringLiteral(
        "QPushButton{background:%1;color:%2;border:0;padding:9px;font-weight:900;}QPushButton:disabled{background:%3;color:%4;}")
                                       .arg(colors::WARNING(), colors::BG_BASE(), colors::BG_RAISED(), colors::TEXT_DIM()));
    maker_layout->addWidget(cashout_button_);

    auto* separator = new QFrame(maker);
    separator->setFrameShape(QFrame::HLine);
    maker_layout->addWidget(separator);
    auto* automation_title = new QLabel(QStringLiteral("AUTOMATION"), maker);
    automation_title->setStyleSheet(QStringLiteral("font-weight:900;color:%1;").arg(colors::TEXT_PRIMARY()));
    maker_layout->addWidget(automation_title);
    gate_label_ = new QLabel(QStringLiteral("PAPER EV ENGINE WARMING\nLive automation remains locked."), maker);
    gate_label_->setWordWrap(true);
    gate_label_->setStyleSheet(QStringLiteral("color:%1;border:1px solid %1;padding:9px;font-weight:800;").arg(colors::RED()));
    maker_layout->addWidget(gate_label_);
    shadow_status_ = new QLabel(QStringLiteral("BOT SHADOW ACTIVE\nCoherent Auto Cockpit decisions plus legacy maker-fill simulation. No orders are submitted."), maker);
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
    auto* auto_cockpit = new QWidget(tabs);
    auto* auto_cockpit_layout = new QVBoxLayout(auto_cockpit);
    auto_cockpit_layout->setContentsMargins(8, 6, 8, 6);
    auto_cockpit_layout->setSpacing(6);
    auto* live_controls = new QHBoxLayout;
    live_controls->setContentsMargins(0, 0, 0, 0);
    live_controls->setSpacing(6);
    live_automation_button_ = new QPushButton(QStringLiteral("ARM AUTOMATION"), auto_cockpit);
    live_automation_button_->setToolTip(QStringLiteral(
        "Arm bounded autonomous Kalshi execution for 1H, 6H, 12H, or 24/7."));
    live_automation_button_->setStyleSheet(QStringLiteral(
        "QPushButton{background:%1;color:%2;border:1px solid %1;padding:7px 12px;font-weight:900;}")
                                               .arg(colors::CYAN(), colors::BG_BASE()));
    live_controls->addWidget(live_automation_button_);
    kill_live_button_ = new QPushButton(QStringLiteral("KILL AUTOMATED TRADING"), auto_cockpit);
    kill_live_button_->setToolTip(QStringLiteral("Engage the global trading kill switch and stop the Kalshi session."));
    kill_live_button_->setStyleSheet(QStringLiteral(
        "QPushButton{background:transparent;color:%1;border:1px solid %1;padding:7px 12px;font-weight:900;}")
                                         .arg(colors::RED()));
    live_controls->addWidget(kill_live_button_);
    live_automation_status_ = new QLabel(QStringLiteral("LIVE EVIDENCE SESSION: loading..."), auto_cockpit);
    live_automation_status_->setStyleSheet(QStringLiteral("color:%1;font-weight:800;").arg(colors::TEXT_SECONDARY()));
    live_controls->addWidget(live_automation_status_, 1);
    auto_cockpit_layout->addLayout(live_controls);
    advisor_separation_status_ = new QLabel(QStringLiteral(
        "LEGACY LIVE SESSION: UNKNOWN / FAIL CLOSED  |  CODEX CANARY: UNKNOWN / FAIL CLOSED"), auto_cockpit);
    advisor_separation_status_->setWordWrap(true);
    advisor_separation_status_->setStyleSheet(QStringLiteral(
        "color:%1;background:%2;border:2px solid %1;padding:8px;font-weight:900;")
        .arg(colors::RED(), colors::BG_RAISED()));
    auto_cockpit_layout->addWidget(advisor_separation_status_);
    flow_status_ = new QLabel(QStringLiteral("FLOW METER · waiting for local Kalshi WebSocket evidence"),
                              auto_cockpit);
    flow_status_->setWordWrap(true);
    flow_status_->setToolTip(QStringLiteral(
        "Read-only public contract flow: resting YES/NO depth, recent taker contracts, and liquidity added or pulled. It is not a count of people and never submits an order."));
    flow_status_->setStyleSheet(QStringLiteral(
        "color:%1;background:%2;border:1px solid %3;padding:7px;font-weight:800;")
                                    .arg(colors::TEXT_SECONDARY(), colors::BG_RAISED(), colors::BORDER_DIM()));
    auto_cockpit_layout->addWidget(flow_status_);
    flow_detail_ = new QLabel(QStringLiteral("FLOW CONTEXT · waiting for multi-window, divergence, cost, and ladder evidence"),
                              auto_cockpit);
    flow_detail_->setWordWrap(true);
    flow_detail_->setToolTip(QStringLiteral(
        "Local decision evidence: several flow windows, spot/contract divergence, immediate taker cost, and ladder checks. Advisory only; it does not place or approve orders."));
    flow_detail_->setStyleSheet(QStringLiteral(
        "color:%1;background:%2;border:1px solid %3;padding:6px;font-weight:700;")
                                    .arg(colors::TEXT_SECONDARY(), colors::BG_BASE(), colors::BORDER_DIM()));
    auto_cockpit_layout->addWidget(flow_detail_);
    ladder_table_ = new CompactTableWidget(0, 8, auto_cockpit);
    ladder_table_->setHorizontalHeaderLabels({QStringLiteral("CONTRACT"), QStringLiteral("SIDE"),
                                               QStringLiteral("ASK"), QStringLiteral("MARKET"),
                                               QStringLiteral("MODEL RAW -> CAL"), QStringLiteral("AUTH"),
                                               QStringLiteral("NET EV"), QStringLiteral("ACTION")});
    ladder_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ladder_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ladder_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ladder_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    ladder_table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    ladder_table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    ladder_table_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    ladder_table_->horizontalHeader()->setSectionResizeMode(7, QHeaderView::Stretch);
    ladder_table_->verticalHeader()->hide();
    ladder_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ladder_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    ladder_table_->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    ladder_table_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    ladder_table_->setToolTip(QStringLiteral("Timestamped paper-only probability surface and constrained portfolio plan. No live order can be submitted here."));
    auto_cockpit_layout->addWidget(ladder_table_, 1);
    tabs->addTab(auto_cockpit, QStringLiteral("AUTO COCKPIT"));

    auto* advisor_scroll = new QScrollArea(tabs);
    advisor_scroll->setWidgetResizable(true);
    advisor_scroll->setFrameShape(QFrame::NoFrame);
    auto* advisor_page = new QWidget(advisor_scroll);
    auto* advisor_layout = new QVBoxLayout(advisor_page);
    advisor_layout->setContentsMargins(10, 10, 10, 10);
    advisor_layout->setSpacing(8);
    auto* advisor_title = new QLabel(QStringLiteral("CODEX ADVISOR & CANARY · READ-ONLY CONTROL PLANE"), advisor_page);
    advisor_title->setStyleSheet(QStringLiteral("color:%1;font-weight:900;font-size:13px;").arg(colors::TEXT_PRIMARY()));
    advisor_layout->addWidget(advisor_title);
    auto* advisor_note = new QLabel(QStringLiteral(
        "This panel displays authoritative persisted supervisor state. It cannot enable trading, edit safety state, or submit orders."), advisor_page);
    advisor_note->setWordWrap(true);
    advisor_note->setStyleSheet(QStringLiteral("color:%1;").arg(colors::TEXT_SECONDARY()));
    advisor_layout->addWidget(advisor_note);
    auto* badges = new QHBoxLayout;
    legacy_live_badge_ = new QLabel(QStringLiteral("LEGACY LIVE SESSION: UNKNOWN / FAIL CLOSED"), advisor_page);
    canary_badge_ = new QLabel(QStringLiteral("CODEX CANARY: UNKNOWN / FAIL CLOSED"), advisor_page);
    for (auto* badge : {legacy_live_badge_, canary_badge_}) {
        badge->setWordWrap(true);
        badge->setStyleSheet(QStringLiteral("color:%1;background:%2;border:2px solid %1;padding:9px;font-weight:900;")
                                 .arg(colors::RED(), colors::BG_RAISED()));
        badges->addWidget(badge, 1);
    }
    advisor_layout->addLayout(badges);
    auto add_advisor_card = [advisor_page, advisor_layout](const QString& title, QLabel*& value) {
        auto* heading = new QLabel(title, advisor_page);
        heading->setStyleSheet(QStringLiteral("color:%1;font-weight:900;").arg(colors::CYAN()));
        advisor_layout->addWidget(heading);
        value = new QLabel(QStringLiteral("UNKNOWN / FAIL CLOSED"), advisor_page);
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setStyleSheet(QStringLiteral("color:%1;background:%2;border:1px solid %3;padding:9px;font-weight:700;")
                                 .arg(colors::TEXT_SECONDARY(), colors::BG_RAISED(), colors::BORDER_DIM()));
        advisor_layout->addWidget(value);
    };
    add_advisor_card(QStringLiteral("SYSTEM STATUS"), advisor_system_status_);
    add_advisor_card(QStringLiteral("QUALIFICATION"), advisor_qualification_status_);
    add_advisor_card(QStringLiteral("SAFETY & PROMOTION"), advisor_safety_status_);
    add_advisor_card(QStringLiteral("LATEST ACTIVITY"), advisor_activity_status_);
    advisor_layout->addStretch();
    advisor_scroll->setWidget(advisor_page);
    tabs->addTab(advisor_scroll, QStringLiteral("ADVISOR & CANARY"));

    auto* pnl_page = new QWidget(tabs);
    auto* pnl_layout = new QVBoxLayout(pnl_page);
    pnl_layout->setContentsMargins(8, 8, 8, 8);
    pnl_layout->setSpacing(6);
    auto* position_tabs = new QTabWidget(pnl_page);
    auto* active_page = new QWidget(position_tabs);
    auto* active_layout = new QVBoxLayout(active_page);
    active_layout->setContentsMargins(0, 6, 0, 0);
    active_layout->setSpacing(6);
    live_positions_summary_ = new QLabel(QStringLiteral("ACTIVE POSITIONS · loading authenticated Kalshi account..."), active_page);
    live_positions_summary_->setWordWrap(true);
    live_positions_summary_->setStyleSheet(
        QStringLiteral("color:%1;background:%2;border:1px solid %3;padding:7px;font-weight:900;")
            .arg(colors::TEXT_SECONDARY(), colors::BG_RAISED(), colors::BORDER_DIM()));
    active_layout->addWidget(live_positions_summary_);
    active_positions_table_ = new QTableWidget(0, 9, active_page);
    active_positions_table_->setHorizontalHeaderLabels({
        QStringLiteral("MODE"), QStringLiteral("CONTRACT"), QStringLiteral("ORIGIN"), QStringLiteral("SIDE"),
        QStringLiteral("CONTRACTS"), QStringLiteral("AVG PRICE"), QStringLiteral("COST"),
        QStringLiteral("CURRENT VALUE"), QStringLiteral("CURRENT P/L")});
    active_positions_table_->verticalHeader()->hide();
    active_positions_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    active_positions_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    active_positions_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int column = 0; column < 9; ++column) {
        if (column == 1) continue;
        active_positions_table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    active_layout->addWidget(active_positions_table_, 1);
    connect(active_positions_table_, &QTableWidget::itemClicked, this,
            [this](QTableWidgetItem* item) {
                if (item && item->column() == 1) show_contract_details(item);
            });
    position_tabs->addTab(active_page, QStringLiteral("ACTIVE POSITIONS"));

    auto* closed_page = new QWidget(position_tabs);
    auto* closed_layout = new QVBoxLayout(closed_page);
    closed_layout->setContentsMargins(0, 6, 0, 0);
    closed_layout->setSpacing(6);
    pnl_summary_ = new QLabel(QStringLiteral("CLOSED BETS · loading authenticated Kalshi settlements..."), closed_page);
    pnl_summary_->setStyleSheet(QStringLiteral("color:%1;font-weight:900;padding:7px;")
                                    .arg(colors::TEXT_SECONDARY()));
    closed_layout->addWidget(pnl_summary_);
    pnl_scoreboard_ = new QLabel(
        QStringLiteral("LIVE SCOREBOARD · loading authenticated Kalshi settlements..."), closed_page);
    pnl_scoreboard_->setWordWrap(true);
    pnl_scoreboard_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pnl_scoreboard_->setToolTip(QStringLiteral(
        "Hit rate and P/L by average entry price (implied probability paid), computed from "
        "exact-accounting live settlements only. Mixed YES/NO turnover rows are excluded and counted."));
    pnl_scoreboard_->setStyleSheet(
        QStringLiteral("color:%1;background:%2;border:1px solid %3;padding:6px 8px;font-weight:700;")
            .arg(colors::TEXT_SECONDARY(), colors::BG_RAISED(), colors::BORDER_DIM()));
    closed_layout->addWidget(pnl_scoreboard_);
    pnl_table_ = new QTableWidget(0, 9, closed_page);
    pnl_table_->setHorizontalHeaderLabels({QStringLiteral("CONTRACT"), QStringLiteral("MODE"),
                                           QStringLiteral("ORIGIN"), QStringLiteral("TYPE"),
                                           QStringLiteral("SIDE / RESULT"),
                                           QStringLiteral("STAKE"), QStringLiteral("FEES"),
                                           QStringLiteral("PAYOUT"), QStringLiteral("REALIZED P/L")});
    pnl_table_->verticalHeader()->hide();
    pnl_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pnl_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    pnl_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 9; ++column)
        pnl_table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    closed_layout->addWidget(pnl_table_, 1);
    connect(pnl_table_, &QTableWidget::itemClicked, this,
            [this](QTableWidgetItem* item) {
                if (item && item->column() == 0) show_contract_details(item);
            });
    position_tabs->addTab(closed_page, QStringLiteral("CLOSED P/L"));
    pnl_layout->addWidget(position_tabs, 1);
    tabs->addTab(pnl_page, QStringLiteral("POSITIONS / P&L"));
    connect(live_automation_button_, &QPushButton::clicked, this, &KalshiScreen::show_live_automation_dialog);
    connect(kill_live_button_, &QPushButton::clicked, this, &KalshiScreen::kill_live_automation);
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
    dom_layout->addWidget(new DineroNetworkGadget(dom_panel_));

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
    connect(account_button_, &QPushButton::clicked, this, &KalshiScreen::show_account_dialog);
    connect(daemon_restart_button_, &QPushButton::clicked, this, &KalshiScreen::restart_daemon);
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
    connect(live_order_button_, &QPushButton::clicked, this, &KalshiScreen::place_live_order);
    connect(cashout_button_, &QPushButton::clicked, this, &KalshiScreen::cash_out_selected_position);
    connect(shadow_button_, &QPushButton::clicked, this, &KalshiScreen::toggle_shadow_collector);
    connect(kraken_dom_button_, &QPushButton::clicked, this,
            [this]() { set_reference_dom_venue(QStringLiteral("kraken")); });
    connect(coinbase_dom_button_, &QPushButton::clicked, this,
            [this]() { set_reference_dom_venue(QStringLiteral("coinbase")); });
    update_cost();
    refresh_account_status();
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
            contract_chart_->set_candles(candles);
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
    connections_ << connect(a, &pred::PredictionExchangeAdapter::balance_ready, this,
                            [this](const pred::AccountBalance& balance) {
                                if (!account_balance_label_) return;
                                account_balance_label_->setText(
                                    QStringLiteral("AVAILABLE: %1 %2 · PORTFOLIO: %3 %4")
                                        .arg(balance.available, 0, 'f', 2).arg(balance.currency)
                                        .arg(balance.total_value, 0, 'f', 2).arg(balance.currency));
                            });
    connections_ << connect(a, &pred::PredictionExchangeAdapter::positions_ready, this,
                            [this, a](const QVector<pred::PredictionPosition>& positions) {
                                positions_ = positions;
                                update_position_panel();
                                update_live_positions_summary();
                                if (auto* kalshi = qobject_cast<kalshi_data::KalshiAdapter*>(a)) {
                                    QStringList tickers;
                                    for (const auto& position : positions) {
                                        if (position.size <= 0.0 || position.market_id.isEmpty() ||
                                            tickers.contains(position.market_id)) continue;
                                        tickers.append(position.market_id);
                                    }
                                    for (int offset = 0; offset < tickers.size(); offset += 100)
                                        kalshi->fetch_batch_order_books(tickers.mid(offset, 100));
                                }
                                if (!position_snapshot_pending_) return;
                                position_snapshot_pending_ = false;
                                QJsonArray snapshot;
                                for (const auto& position : positions) {
                                    snapshot.append(QJsonObject{
                                        {QStringLiteral("asset_id"), position.asset_id},
                                        {QStringLiteral("market_id"), position.market_id},
                                        {QStringLiteral("outcome"), position.outcome},
                                        {QStringLiteral("contracts"), position.size},
                                        {QStringLiteral("average_price"), position.avg_price},
                                        {QStringLiteral("realized_pnl"), position.realized_pnl},
                                        {QStringLiteral("current_value"), position.current_value},
                                        {QStringLiteral("fees_paid"), position.fees_paid}});
                                }
                                kalshi_data::KalshiEvidenceEngine::append_jsonl(
                                    evidence_path(QStringLiteral("kalshi-trade-ledger.jsonl")),
                                    QJsonObject{{QStringLiteral("event"), QStringLiteral("kalshi_position_snapshot")},
                                                {QStringLiteral("ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                                                {QStringLiteral("positions"), snapshot}});
                            });
    connections_ << connect(a, &pred::PredictionExchangeAdapter::account_activity_ready, this,
                            [this, a](const QVariantList& activities) {
                                if (!record_account_fills(activities)) return;
                                position_snapshot_pending_ = true;
                                a->fetch_positions();
                            });
    connections_ << connect(a, &pred::PredictionExchangeAdapter::order_placed, this,
                            [this, a](const pred::OrderResult& result) {
                                const QString kind = pending_order_kind_;
                                pending_order_kind_.clear();
                                if (kind == QStringLiteral("manual") && !pending_manual_order_.isEmpty()) {
                                    QJsonObject row = pending_manual_order_;
                                    row.insert(QStringLiteral("event"), QStringLiteral("kalshi_manual_order_submitted"));
                                    row.insert(QStringLiteral("submitted_ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                                    row.insert(QStringLiteral("order_id"), result.order_id);
                                    row.insert(QStringLiteral("status"), result.status);
                                    kalshi_data::KalshiEvidenceEngine::append_jsonl(
                                        evidence_path(QStringLiteral("kalshi-trade-ledger.jsonl")), row);
                                    pending_manual_order_ = {};
                                }
                                if (kind == QStringLiteral("manual") && live_order_button_) {
                                    live_order_button_->setEnabled(true);
                                    refresh_account_status();
                                }
                                if (kind == QStringLiteral("cashout") && cashout_button_)
                                    cashout_button_->setEnabled(true);
                                if (!result.ok) {
                                    QMessageBox::warning(this, QStringLiteral("Kalshi order failed"),
                                                         result.error_message.isEmpty()
                                                             ? QStringLiteral("Kalshi did not accept the order.")
                                                             : result.error_message);
                                    return;
                                }
                                const bool exit = kind == QStringLiteral("cashout");
                                QMessageBox::information(this,
                                                         exit ? QStringLiteral("Kalshi exit submitted")
                                                              : QStringLiteral("Kalshi order submitted"),
                                                         QStringLiteral("%1: %2\nStatus: %3")
                                                             .arg(exit ? QStringLiteral("Reduce-only exit submitted")
                                                                       : QStringLiteral("Post-only maker order submitted"),
                                                                  result.order_id.left(12),
                                                                  result.status.isEmpty()
                                                                      ? QStringLiteral("SUBMITTED")
                                                                      : result.status));
                                a->fetch_balance();
                                a->fetch_positions();
                                a->fetch_open_orders();
                                a->fetch_user_activity(100);
                            });
    connections_ << connect(a, &pred::PredictionExchangeAdapter::ws_connection_changed, this, [this](bool connected) {
        connection_badge_->setText(connected ? QStringLiteral("LIVE BOOK") : QStringLiteral("PUBLIC DATA"));
    });
    connections_ << connect(a, &pred::PredictionExchangeAdapter::error_occurred, this,
                            [this](const QString& context, const QString& message) {
                                if (context == QStringLiteral("fetch_price_history"))
                                    chart_fetching_ = false;
                                if (context == QStringLiteral("place_order") ||
                                    context == QStringLiteral("preflight_order")) {
                                    const QString kind = pending_order_kind_;
                                    pending_order_kind_.clear();
                                    if (kind == QStringLiteral("manual") && !pending_manual_order_.isEmpty()) {
                                        QJsonObject row = pending_manual_order_;
                                        row.insert(QStringLiteral("event"), QStringLiteral("kalshi_manual_order_rejected"));
                                        row.insert(QStringLiteral("rejected_ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                                        row.insert(QStringLiteral("error"), message);
                                        kalshi_data::KalshiEvidenceEngine::append_jsonl(
                                            evidence_path(QStringLiteral("kalshi-trade-ledger.jsonl")), row);
                                        pending_manual_order_ = {};
                                    }
                                    if (kind == QStringLiteral("manual") && live_order_button_) {
                                        live_order_button_->setEnabled(true);
                                        refresh_account_status();
                                    }
                                    if (kind == QStringLiteral("cashout") && cashout_button_)
                                        cashout_button_->setEnabled(true);
                                    QMessageBox::warning(this, QStringLiteral("Kalshi order rejected"), message);
                                    return;
                                }
                                if (context == QStringLiteral("fetch_market") &&
                                    (message.contains(QStringLiteral("404")) ||
                                     message.contains(QStringLiteral("not_found"))))
                                    return;
                                connection_badge_->setText(QStringLiteral("DATA ISSUE"));
                                connection_badge_->setToolTip(message);
                            });
    connections_ << connect(a, &pred::PredictionExchangeAdapter::credentials_changed,
                            this, &KalshiScreen::refresh_account_status);
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
        connections_ << connect(kalshi, &kalshi_data::KalshiAdapter::ws_cf_benchmark_event,
                                this, [this](const QString& index, double value, qint64 ts_ms,
                                             const QJsonObject& payload) {
            if (index != cf_index_for_asset(asset_)) return;
            official_settlement_index_ = index;
            official_settlement_reference_ = value;
            official_settlement_reference_ms_ = ts_ms > 0 ? ts_ms : QDateTime::currentMSecsSinceEpoch();
            QJsonObject row = payload;
            row.insert(QStringLiteral("event"), QStringLiteral("kalshi_cf_benchmark_value"));
            row.insert(QStringLiteral("index_id"), index);
            row.insert(QStringLiteral("value"), value);
            row.insert(QStringLiteral("ts_ms"), QString::number(official_settlement_reference_ms_));
            row.insert(QStringLiteral("received_ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            row.insert(QStringLiteral("live_eligible"), false);
            kalshi_data::KalshiEvidenceEngine::append_jsonl(
                evidence_path(QStringLiteral("kalshi-cf-benchmarks.jsonl")), row);
        });
        connections_ << connect(kalshi, &kalshi_data::KalshiAdapter::batch_order_books_ready,
                                this, [this](const QHash<QString, pred::PredictionOrderBook>& books) {
            for (const auto& book : books) render_order_book(book);
            update_live_positions_summary();
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
            render_closed_bets(settlements);
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
        if (a->has_credentials()) {
            a->fetch_balance();
            a->fetch_positions();
        }
        if (auto* kalshi = qobject_cast<kalshi_data::KalshiAdapter*>(a); kalshi) {
            kalshi->fetch_exchange_status();
            kalshi->fetch_exchange_schedule();
            kalshi->fetch_series_fee_changes();
            if (kalshi->has_credentials()) kalshi->fetch_settlements(100);
        }
    }
    refresh_live_automation_status();
}

void KalshiScreen::set_family(const QString& family) {
    family_ = family;
    asset_bar_->setVisible(family_ == QStringLiteral("Crypto"));
    cadence_bar_->setVisible(family_ == QStringLiteral("Crypto"));
    refresh();
}

void KalshiScreen::set_asset(const QString& asset) {
    asset_ = asset;
    official_settlement_reference_ = 0.0;
    official_settlement_reference_ms_ = 0;
    official_settlement_index_.clear();
    set_spot_symbol(asset_);
    if (auto* kalshi = qobject_cast<kalshi_data::KalshiAdapter*>(adapter()))
        kalshi->subscribe_cf_benchmarks({cf_index_for_asset(asset_)});
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
    contract_chart_->clear();
    contract_chart_->set_probability_mode(true);
    contract_chart_->set_symbol(selected_.key.market_id + QLatin1Char(' ') + side_);
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
        if (a->has_credentials()) a->fetch_positions();
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
        contract_chart_->clear();
        contract_chart_->set_probability_mode(true);
        contract_chart_->set_symbol(selected_.key.market_id + QLatin1Char(' ') + side_);
        live_chart_seeded_ = false;
        last_chart_fetch_ms_ = 0;
        chart_status_->setText(QStringLiteral("LOADING KALSHI %1 HISTORY").arg(side_));
        chart_status_->show();
        QTimer::singleShot(0, this, &KalshiScreen::refresh_reference_chart);
    }
    update_strike_overlay();
    update_observation_strip();
    update_calibrator_readout();
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
    contract_chart_->append_candle(tick);
    live_chart_seeded_ = true;
    chart_status_->hide();
    if (!price_->hasFocus()) price_->setValue(chart_price);
    update_position_panel();
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
    update_position_panel();
    update_market_health();
}

void KalshiScreen::update_position_panel() {
    if (!position_label_ || !cashout_label_ || !cashout_button_) return;
    cashout_button_->setEnabled(false);

    if (!has_selection_) {
        position_label_->setText(QStringLiteral("POSITION: select a Kalshi contract"));
        cashout_label_->setText(QStringLiteral("CASH-OUT VALUE: waiting for a contract"));
        return;
    }

    const auto position_it = std::find_if(positions_.cbegin(), positions_.cend(), [this](const auto& position) {
        return position.market_id == selected_.key.market_id && position.size > 0.0;
    });
    if (position_it == positions_.cend()) {
        QStringList related_positions;
        for (const auto& position : positions_) {
            if (position.size <= 0.0 || position.market_id == selected_.key.market_id) continue;
            const auto market_it = std::find_if(all_markets_.cbegin(), all_markets_.cend(),
                                                [&position](const auto& market) {
                return market.key.market_id == position.market_id;
            });
            if (market_it == all_markets_.cend() ||
                market_it->key.event_id != selected_.key.event_id) continue;
            related_positions.append(QStringLiteral("%1 %2 · %3")
                                         .arg(position.size, 0, 'f', 2)
                                         .arg(position.outcome)
                                         .arg(contract_label(*market_it)));
        }
        if (!related_positions.isEmpty()) {
            position_label_->setText(QStringLiteral("EVENT POSITIONS: %1")
                                         .arg(related_positions.join(QStringLiteral(" | "))));
            cashout_label_->setText(
                QStringLiteral("CASH-OUT: select the matching contract to close that position."));
            return;
        }
        position_label_->setText(QStringLiteral("POSITION: none in this contract"));
        cashout_label_->setText(QStringLiteral("CASH-OUT VALUE: no position to close"));
        return;
    }

    const auto& position = *position_it;
    position_label_->setText(QStringLiteral("POSITION: %1 %2 · traded %3 · fees paid %4 · realized P&L %5")
                                 .arg(position.size, 0, 'f', 2).arg(position.outcome)
                                 .arg(money(position.total_traded)).arg(money(position.fees_paid))
                                 .arg(money(position.realized_pnl)));
    const auto book_it = kalshi_books_.constFind(position.asset_id);
    if (book_it == kalshi_books_.cend() || book_it->bids.isEmpty()) {
        cashout_label_->setText(QStringLiteral("CASH-OUT VALUE: waiting for an executable %1 bid")
                                    .arg(position.outcome));
        return;
    }

    const double bid = book_it->bids.first().price;
    const double displayed_depth = book_it->bids.first().size;
    const double gross = bid * position.size;
    const double fee = kalshi_data::KalshiEvidenceEngine::conservative_taker_fee(selected_, bid, position.size);
    const double net = qMax(0.0, gross - fee);
    const QString estimate = position.total_traded > 0.0
        ? QStringLiteral(" · provisional P&L %1").arg(money(net - position.total_traded - position.fees_paid))
        : QString();
    cashout_label_->setText(QStringLiteral(
        "CASH-OUT NOW: %1 net at %2 %3 bid · fee est. %4 · visible depth %5%6")
                                   .arg(money(net)).arg(position.outcome)
                                   .arg(probability(bid)).arg(money(fee))
                                   .arg(displayed_depth, 0, 'f', 2).arg(estimate));
    cashout_label_->setToolTip(QStringLiteral(
        "Uses the current executable bid and a conservative taker-fee estimate. "
        "Provisional P&L assumes total traded is entry cost; partial exits can make it differ from Kalshi's final accounting."));
    cashout_button_->setEnabled(adapter() && adapter()->has_credentials() && bid > 0.0);
}

void KalshiScreen::update_live_positions_summary() {
    if (!live_positions_summary_ || !active_positions_table_) return;
    active_positions_table_->setRowCount(0);
    int live_count = 0;
    int paper_count = 0;
    double total_cost = 0.0;
    double total_exit_value = 0.0;
    double total_marked_pnl = 0.0;
    bool all_marked = true;
    for (const auto& position : positions_) {
        if (position.size <= 0.0) continue;
        ++live_count;
        QString origin = QStringLiteral("HUMAN");
        QString rationale = QStringLiteral("Human or external Kalshi order; no bot decision snapshot is attached.");
        QString audit_timestamp;
        auto audit = Database::instance().execute(
            "SELECT json_extract(intent_json,'$.decision_rationale'),ts FROM trade_audit "
            "WHERE mode='live' AND decision='filled' "
            "AND json_extract(intent_json,'$.venue')='kalshi' "
            "AND json_extract(intent_json,'$.market_id')=? "
            "AND json_extract(intent_json,'$.experiment_id')='kalshi-micro-live-v1' "
            "ORDER BY id DESC LIMIT 1", {position.market_id});
        if (audit.is_ok() && audit.value().next()) {
            origin = QStringLiteral("BOT");
            rationale = audit.value().value(0).toString().trimmed();
            if (rationale.isEmpty()) rationale = QStringLiteral("Automated Kalshi decision; rationale was not recorded.");
            audit_timestamp = audit.value().value(1).toString();
        }

        const double cost = position.total_traded > 0.0
            ? position.total_traded + position.fees_paid
            : position.avg_price * position.size + position.fees_paid;
        const auto book_it = kalshi_books_.constFind(position.asset_id);
        const bool has_book = book_it != kalshi_books_.cend();
        const bool has_mark = has_book && !book_it->bids.isEmpty();
        double exit_value = 0.0;
        double marked_pnl = 0.0;
        if (has_mark) {
            const double bid = book_it->bids.first().price;
            const double exit_fee = kalshi_data::KalshiEvidenceEngine::conservative_taker_fee(
                bid, position.size);
            exit_value = qMax(0.0, bid * position.size - exit_fee);
            marked_pnl = exit_value - cost;
            total_exit_value += exit_value;
            total_marked_pnl += marked_pnl;
        } else {
            all_marked = false;
        }
        total_cost += cost;

        const int row = active_positions_table_->rowCount();
        active_positions_table_->insertRow(row);
        const auto put = [this, row](int column, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            active_positions_table_->setItem(row, column, item);
            return item;
        };
        auto* mode_item = put(0, QStringLiteral("LIVE"));
        mode_item->setForeground(QColor(colors::POSITIVE()));
        auto* contract_item = put(1, position.market_id);
        make_contract_link(contract_item, QJsonObject{
            {QStringLiteral("market_id"), position.market_id},
            {QStringLiteral("asset_id"), position.asset_id},
            {QStringLiteral("mode"), QStringLiteral("LIVE")},
            {QStringLiteral("origin"), origin},
            {QStringLiteral("state"), QStringLiteral("ACTIVE")},
            {QStringLiteral("side"), position.outcome.toUpper()},
            {QStringLiteral("contracts"), position.size},
            {QStringLiteral("average_price"), position.avg_price},
            {QStringLiteral("stake"), cost},
            {QStringLiteral("fees"), position.fees_paid},
            {QStringLiteral("current_value"), has_mark ? exit_value : -1.0},
            {QStringLiteral("pnl"), has_mark ? QJsonValue(marked_pnl) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("realized_pnl"), position.realized_pnl},
            {QStringLiteral("rationale"), rationale},
            {QStringLiteral("decision_at"), audit_timestamp}});
        auto* origin_item = put(2, origin);
        origin_item->setForeground(QColor(origin == QStringLiteral("BOT") ? colors::CYAN() : colors::WARNING()));
        put(3, position.outcome.toUpper());
        put(4, QString::number(position.size, 'f', 2));
        put(5, QStringLiteral("%1c").arg(position.avg_price * 100.0, 0, 'f', 1));
        put(6, money(cost));
        put(7, has_mark ? money(exit_value)
                        : (has_book ? QStringLiteral("NO EXECUTABLE BID") : QStringLiteral("LOADING BOOK")));
        auto* pnl_item = put(8, has_mark ? money(marked_pnl) : QStringLiteral("--"));
        pnl_item->setForeground(QColor(has_mark
            ? (marked_pnl >= 0.0 ? colors::POSITIVE() : colors::NEGATIVE())
            : colors::TEXT_DIM()));
        for (int column = 4; column < 9; ++column)
            active_positions_table_->item(row, column)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    auto paper = Database::instance().execute(
        "SELECT p.position_id, p.strategy_id, p.decision_id, j.market_id, p.side, p.limit_price, p.qty, "
        "p.entry_fee, p.notional_usd, p.opened_at, s.kind "
        "FROM sandbox_position p JOIN edge_decision_journal j ON j.id=p.decision_id "
        "JOIN sandbox_strategy s ON s.strategy_id=p.strategy_id "
        "WHERE j.source='kalshi auto-plan' AND p.state='open' "
        "ORDER BY p.created_at DESC", {});
    if (paper.is_ok()) {
        auto& rows = paper.value();
        while (rows.next()) {
            ++paper_count;
            const QString position_id = rows.value(0).toString();
            const QString strategy_id = rows.value(1).toString();
            const QString decision_id = rows.value(2).toString();
            const QString market_id = rows.value(3).toString();
            const QString side = rows.value(4).toString().trimmed().toLower();
            const double entry = rows.value(5).toDouble();
            const double quantity = rows.value(6).toDouble();
            const double entry_fee = rows.value(7).toDouble();
            const double notional = rows.value(8).toDouble();
            const qint64 opened_at = rows.value(9).toLongLong();
            const QString book = rows.value(10).toString();
            const double cost = notional + entry_fee;
            double bid = 0.0;
            auto mark = Database::instance().execute(
                "SELECT features_json FROM edge_decision_journal "
                "WHERE source='kalshi auto-plan' AND market_id=? AND created_at>=? "
                "ORDER BY created_at DESC LIMIT 1", {market_id, opened_at});
            if (mark.is_ok() && mark.value().next()) {
                const QJsonObject features = QJsonDocument::fromJson(
                    mark.value().value(0).toString().toUtf8()).object();
                const QJsonObject signal = features.value(QStringLiteral("signal")).toObject();
                bid = signal.value(side == QStringLiteral("no")
                    ? QStringLiteral("no_bid") : QStringLiteral("yes_bid")).toDouble();
            }
            const bool has_mark = bid > 0.0;
            const double exit_fee = has_mark
                ? kalshi_data::KalshiEvidenceEngine::conservative_taker_fee(bid, quantity) : 0.0;
            const double exit_value = has_mark ? qMax(0.0, bid * quantity - exit_fee) : 0.0;
            const double marked_pnl = exit_value - cost;
            total_cost += cost;
            if (has_mark) {
                total_exit_value += exit_value;
                total_marked_pnl += marked_pnl;
            } else {
                all_marked = false;
            }

            const int row = active_positions_table_->rowCount();
            active_positions_table_->insertRow(row);
            const auto put = [this, row](int column, const QString& text, const QString& tooltip = {}) {
                auto* item = new QTableWidgetItem(text);
                if (!tooltip.isEmpty()) item->setToolTip(tooltip);
                active_positions_table_->setItem(row, column, item);
                return item;
            };
            auto* mode_item = put(0, QStringLiteral("PAPER"),
                                  QStringLiteral("Local paper position. No real money was submitted."));
            mode_item->setForeground(QColor(colors::CYAN()));
            auto* contract_item = put(1, market_id, QStringLiteral("Internal position: %1\nBook: %2\nStrategy: %3")
                                                        .arg(position_id, book, strategy_id));
            make_contract_link(contract_item, QJsonObject{
                {QStringLiteral("market_id"), market_id},
                {QStringLiteral("mode"), QStringLiteral("PAPER")},
                {QStringLiteral("origin"), QStringLiteral("BOT")},
                {QStringLiteral("state"), QStringLiteral("ACTIVE")},
                {QStringLiteral("side"), side.toUpper()},
                {QStringLiteral("contracts"), quantity},
                {QStringLiteral("average_price"), entry},
                {QStringLiteral("stake"), cost},
                {QStringLiteral("fees"), entry_fee},
                {QStringLiteral("current_value"), has_mark ? exit_value : -1.0},
                {QStringLiteral("pnl"), has_mark ? QJsonValue(marked_pnl) : QJsonValue(QJsonValue::Null)},
                {QStringLiteral("position_id"), position_id},
                {QStringLiteral("strategy_id"), strategy_id},
                {QStringLiteral("decision_id"), decision_id},
                {QStringLiteral("book"), book},
                {QStringLiteral("opened_at_ms"), QString::number(opened_at)}});
            auto* origin_item = put(2, QStringLiteral("BOT"));
            origin_item->setForeground(QColor(colors::CYAN()));
            put(3, side.toUpper());
            put(4, QString::number(quantity, 'f', 2));
            put(5, QStringLiteral("%1c").arg(entry * 100.0, 0, 'f', 1));
            put(6, money(cost));
            put(7, has_mark ? money(exit_value) : QStringLiteral("WAITING FOR BID"));
            auto* pnl_item = put(8, has_mark ? money(marked_pnl) : QStringLiteral("--"));
            pnl_item->setForeground(QColor(has_mark
                ? (marked_pnl >= 0.0 ? colors::POSITIVE() : colors::NEGATIVE())
                : colors::TEXT_DIM()));
            for (int column = 4; column < 9; ++column)
                active_positions_table_->item(row, column)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        }
    }

    const int open_count = live_count + paper_count;
    if (open_count == 0) {
        live_positions_summary_->setText(QStringLiteral("ACTIVE POSITIONS 0 · no unsettled Kalshi exposure"));
        live_positions_summary_->setToolTip(QStringLiteral("No authenticated live or local paper positions are open."));
        live_positions_summary_->setStyleSheet(
            QStringLiteral("color:%1;background:%2;border:1px solid %3;padding:6px 8px;font-weight:800;")
                .arg(colors::TEXT_SECONDARY(), colors::BG_RAISED(), colors::BORDER_DIM()));
        return;
    }
    live_positions_summary_->setText(
        QStringLiteral("ACTIVE POSITIONS %1 · LIVE %2 FIRST / PAPER %3 BELOW · COST %4 · EXIT VALUE %5 · MARKED P/L %6%7")
            .arg(open_count).arg(live_count).arg(paper_count).arg(money(total_cost))
            .arg(all_marked ? money(total_exit_value) : QStringLiteral("PARTIAL"))
            .arg(all_marked ? money(total_marked_pnl) : QStringLiteral("PARTIAL"))
            .arg(all_marked ? QString() : QStringLiteral(" · waiting for executable bids")));
    live_positions_summary_->setToolTip(QStringLiteral(
        "LIVE rows are authenticated Kalshi positions. PAPER rows are local simulations using the same decision gate. "
        "Marked P/L uses current executable bids and estimated exit fees."));
    live_positions_summary_->setStyleSheet(
        QStringLiteral("color:%1;background:%2;border:1px solid %3;padding:6px 8px;font-weight:800;")
            .arg(all_marked && total_marked_pnl < 0.0 ? colors::NEGATIVE() : colors::POSITIVE(),
                 colors::BG_RAISED(), all_marked && total_marked_pnl < 0.0 ? colors::NEGATIVE() : colors::POSITIVE()));
}

void KalshiScreen::render_closed_bets(const QJsonArray& settlements) {
    if (!pnl_table_ || !pnl_summary_) return;
    if (pnl_scoreboard_) {
        pnl_scoreboard_->setText(kalshi_data::KalshiSettlementScoreboard::format(
            kalshi_data::KalshiSettlementScoreboard::compute(settlements)));
    }
    pnl_table_->setRowCount(0);
    double live_pnl = 0.0;
    double paper_pnl = 0.0;
    int live_rows = 0;
    int paper_rows = 0;
    int wins = 0;
    int losses = 0;
    for (const auto& value : settlements) {
        const QJsonObject settlement = value.toObject();
        const QString market_id = settlement.value(QStringLiteral("market_id")).toString();
        if (market_id.isEmpty()) continue;
        QString origin = QStringLiteral("HUMAN");
        QString rationale = QStringLiteral("Human or external Kalshi order; no bot decision snapshot is attached.");
        auto audit = Database::instance().execute(
            "SELECT json_extract(intent_json,'$.decision_rationale') FROM trade_audit "
            "WHERE mode='live' AND decision='filled' "
            "AND json_extract(intent_json,'$.venue')='kalshi' "
            "AND json_extract(intent_json,'$.market_id')=? "
            "AND json_extract(intent_json,'$.experiment_id')='kalshi-micro-live-v1' "
            "ORDER BY id DESC LIMIT 1", {market_id});
        if (audit.is_ok() && audit.value().next()) {
            origin = QStringLiteral("BOT");
            rationale = audit.value().value(0).toString();
        }
        const QString type = market_id.startsWith(QStringLiteral("KXBTCD"))
            ? QStringLiteral("OR ABOVE")
            : market_id.contains(QRegularExpression(QStringLiteral("-B[0-9]")))
                ? QStringLiteral("RANGE") : QStringLiteral("BINARY");
        const bool pnl_exact = settlement.value(QStringLiteral("realized_pnl")).isDouble();
        const double pnl = pnl_exact ? settlement.value(QStringLiteral("realized_pnl")).toDouble() : 0.0;
        if (pnl_exact) {
            live_pnl += pnl;
            if (pnl > 1e-9) ++wins;
            else if (pnl < -1e-9) ++losses;
        }
        const int row = pnl_table_->rowCount();
        ++live_rows;
        pnl_table_->insertRow(row);
        const auto put = [this, row](int column, const QString& text, const QString& tooltip = {}) {
            auto* item = new QTableWidgetItem(text);
            if (!tooltip.isEmpty()) item->setToolTip(tooltip);
            pnl_table_->setItem(row, column, item);
            return item;
        };
        const QString internal_id = settlement.value(QStringLiteral("internal_id")).toString();
        const QDateTime settled_at = QDateTime::fromString(
            settlement.value(QStringLiteral("settled_time")).toString(), Qt::ISODateWithMs);
        const QString settled_local = settled_at.isValid()
            ? settled_at.toLocalTime().toString(QStringLiteral("MMM d, yyyy h:mm:ss ap"))
            : QStringLiteral("Settlement time unavailable");
        auto* contract_item = put(0, market_id, QStringLiteral("Internal ID: %1\nSettled: %2").arg(internal_id, settled_local));
        QJsonObject details = settlement;
        details.insert(QStringLiteral("mode"), QStringLiteral("LIVE"));
        details.insert(QStringLiteral("origin"), origin);
        details.insert(QStringLiteral("state"), QStringLiteral("CLOSED"));
        details.insert(QStringLiteral("type"), type);
        details.insert(QStringLiteral("rationale"), rationale);
        make_contract_link(contract_item, details);
        auto* mode_item = put(1, QStringLiteral("LIVE"), QStringLiteral("Authenticated Kalshi settlement."));
        mode_item->setForeground(QColor(colors::POSITIVE()));
        auto* origin_item = put(2, origin, rationale);
        origin_item->setForeground(QColor(origin == QStringLiteral("BOT") ? colors::CYAN() : colors::WARNING()));
        put(3, type);
        put(4, QStringLiteral("%1 / %2")
                   .arg(settlement.value(QStringLiteral("side")).toString(),
                        settlement.value(QStringLiteral("market_result")).toString()));
        put(5, QStringLiteral("$%1").arg(settlement.value(QStringLiteral("stake")).toDouble(), 0, 'f', 2));
        put(6, QStringLiteral("$%1").arg(settlement.value(QStringLiteral("fees")).toDouble(), 0, 'f', 2));
        put(7, QStringLiteral("$%1").arg(settlement.value(QStringLiteral("payout")).toDouble(), 0, 'f', 2));
        auto* pnl_item = put(8, pnl_exact
            ? QStringLiteral("%1$%2").arg(pnl >= 0.0 ? QStringLiteral("+") : QStringLiteral(""))
                                    .arg(pnl, 0, 'f', 2)
            : QStringLiteral("RECONCILE FILLS"),
            pnl_exact ? rationale : QStringLiteral("This market contains both YES and NO turnover; complete fill cashflows are required for exact P/L."));
        pnl_item->setForeground(QColor(pnl_exact
            ? (pnl >= 0.0 ? colors::POSITIVE() : colors::NEGATIVE()) : colors::TEXT_DIM()));
        pnl_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    auto paper = Database::instance().execute(
        "SELECT p.position_id, p.decision_id, j.market_id, p.side, p.notional_usd, p.entry_fee, p.exit_fee, "
        "p.realized_pnl, p.close_reason, p.closed_at, p.opened_at, p.qty, p.limit_price, "
        "p.strategy_id, s.kind "
        "FROM sandbox_position p JOIN edge_decision_journal j ON j.id=p.decision_id "
        "JOIN sandbox_strategy s ON s.strategy_id=p.strategy_id "
        "WHERE j.source='kalshi auto-plan' AND p.state IN ('closed','unfilled') "
        "ORDER BY p.closed_at DESC, p.created_at DESC LIMIT 1000", {});
    if (paper.is_ok()) {
        auto& rows = paper.value();
        while (rows.next()) {
            ++paper_rows;
            const QString position_id = rows.value(0).toString();
            const QString decision_id = rows.value(1).toString();
            const QString market_id = rows.value(2).toString();
            const QString side = rows.value(3).toString().toUpper();
            const double stake = rows.value(4).toDouble();
            const double entry_fee = rows.value(5).toDouble();
            const double exit_fee = rows.value(6).toDouble();
            const bool pnl_exact = !rows.value(7).isNull();
            const double pnl = pnl_exact ? rows.value(7).toDouble() : 0.0;
            const QString reason = rows.value(8).toString();
            const qint64 closed_at_ms = rows.value(9).toLongLong();
            const qint64 opened_at_ms = rows.value(10).toLongLong();
            const double quantity = rows.value(11).toDouble();
            const double entry_price = rows.value(12).toDouble();
            const QString strategy_id = rows.value(13).toString();
            const QString book = rows.value(14).toString();
            if (pnl_exact) paper_pnl += pnl;
            const QString type = market_id.startsWith(QStringLiteral("KXBTCD"))
                ? QStringLiteral("OR ABOVE")
                : market_id.contains(QRegularExpression(QStringLiteral("-B[0-9]")))
                    ? QStringLiteral("RANGE") : QStringLiteral("BINARY");
            const int row = pnl_table_->rowCount();
            pnl_table_->insertRow(row);
            const auto put = [this, row](int column, const QString& text, const QString& tooltip = {}) {
                auto* item = new QTableWidgetItem(text);
                if (!tooltip.isEmpty()) item->setToolTip(tooltip);
                pnl_table_->setItem(row, column, item);
                return item;
            };
            const QString closed_local = closed_at_ms > 0
                ? QDateTime::fromMSecsSinceEpoch(closed_at_ms).toLocalTime().toString(QStringLiteral("MMM d, yyyy h:mm:ss ap"))
                : QStringLiteral("Close time unavailable");
            auto* contract_item = put(0, market_id, QStringLiteral("Internal position: %1\nClosed: %2\nBook: %3")
                                                        .arg(position_id, closed_local, book));
            make_contract_link(contract_item, QJsonObject{
                {QStringLiteral("market_id"), market_id},
                {QStringLiteral("mode"), QStringLiteral("PAPER")},
                {QStringLiteral("origin"), QStringLiteral("BOT")},
                {QStringLiteral("state"), QStringLiteral("CLOSED")},
                {QStringLiteral("type"), type},
                {QStringLiteral("side"), side},
                {QStringLiteral("contracts"), quantity},
                {QStringLiteral("average_price"), entry_price},
                {QStringLiteral("stake"), stake},
                {QStringLiteral("fees"), entry_fee + exit_fee},
                {QStringLiteral("pnl"), pnl_exact ? QJsonValue(pnl) : QJsonValue(QJsonValue::Null)},
                {QStringLiteral("close_reason"), reason},
                {QStringLiteral("position_id"), position_id},
                {QStringLiteral("strategy_id"), strategy_id},
                {QStringLiteral("decision_id"), decision_id},
                {QStringLiteral("book"), book},
                {QStringLiteral("opened_at_ms"), QString::number(opened_at_ms)},
                {QStringLiteral("closed_at_ms"), QString::number(closed_at_ms)}});
            auto* mode_item = put(1, QStringLiteral("PAPER"),
                                  QStringLiteral("Local simulation. No real money was submitted."));
            mode_item->setForeground(QColor(colors::CYAN()));
            auto* origin_item = put(2, QStringLiteral("BOT"),
                                    QStringLiteral("Generated by the Kalshi auto-plan decision stream."));
            origin_item->setForeground(QColor(colors::CYAN()));
            put(3, type);
            put(4, QStringLiteral("%1 / %2").arg(side, reason.toUpper()));
            put(5, money(stake));
            put(6, money(entry_fee + exit_fee));
            put(7, pnl_exact ? money(qMax(0.0, stake + entry_fee + exit_fee + pnl)) : QStringLiteral("--"));
            auto* pnl_item = put(8, pnl_exact ? money(pnl) : QStringLiteral("UNFILLED"), reason);
            pnl_item->setForeground(QColor(pnl_exact
                ? (pnl >= 0.0 ? colors::POSITIVE() : colors::NEGATIVE()) : colors::TEXT_DIM()));
            pnl_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        }
    }
    const double total_pnl = live_pnl + paper_pnl;
    pnl_summary_->setText(QStringLiteral(
        "CLOSED BETS %1 · LIVE %2 P/L %3 · PAPER %4 P/L %5 · LIVE EXACT W %6 / L %7")
        .arg(pnl_table_->rowCount()).arg(live_rows).arg(money(live_pnl))
        .arg(paper_rows).arg(money(paper_pnl)).arg(wins).arg(losses));
    pnl_summary_->setStyleSheet(QStringLiteral("color:%1;font-weight:900;padding:7px;")
        .arg(total_pnl >= 0.0 ? colors::POSITIVE() : colors::NEGATIVE()));
}

void KalshiScreen::show_contract_details(QTableWidgetItem* item) {
    if (!item) return;
    const QJsonDocument payload = QJsonDocument::fromJson(
        item->data(kContractDetailsRole).toString().toUtf8());
    if (!payload.isObject()) return;
    const QJsonObject details = payload.object();
    const QString market_id = details.value(QStringLiteral("market_id")).toString();
    if (market_id.isEmpty()) return;

    QString question = details.value(QStringLiteral("question")).toString().trimmed();
    QString close_time;
    const auto market_it = std::find_if(all_markets_.cbegin(), all_markets_.cend(),
                                        [&market_id](const auto& market) {
        return market.key.market_id == market_id;
    });
    if (market_it != all_markets_.cend()) {
        if (question.isEmpty()) question = contract_label(*market_it);
        close_time = market_it->end_date_iso;
    }
    if (question.isEmpty()) question = QStringLiteral("Kalshi contract");

    const auto local_time_from_ms = [](const QJsonValue& value) {
        const qint64 ms = value.toString().toLongLong();
        return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC)
                            .toLocalTime().toString(QStringLiteral("MMM d, yyyy h:mm:ss ap"))
                      : QString();
    };
    const auto value_text = [](const QJsonValue& value) {
        if (value.isString()) return value.toString();
        if (value.isDouble()) return QString::number(value.toDouble(), 'f', 2);
        if (value.isBool()) return value.toBool() ? QStringLiteral("YES") : QStringLiteral("NO");
        return QString();
    };

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Contract Audit · %1").arg(market_id));
    dialog.setModal(true);
    dialog.resize(960, 680);
    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(8);

    auto* title = new QLabel(question, &dialog);
    title->setWordWrap(true);
    title->setStyleSheet(QStringLiteral("color:%1;font-size:18px;font-weight:900;")
                             .arg(colors::TEXT_PRIMARY()));
    root->addWidget(title);
    auto* identity = new QLabel(
        QStringLiteral("%1  ·  %2  ·  %3  ·  %4")
            .arg(market_id,
                 details.value(QStringLiteral("mode")).toString(QStringLiteral("UNKNOWN")),
                 details.value(QStringLiteral("origin")).toString(QStringLiteral("UNKNOWN")),
                 details.value(QStringLiteral("state")).toString(QStringLiteral("UNKNOWN"))),
        &dialog);
    identity->setTextInteractionFlags(Qt::TextSelectableByMouse);
    identity->setStyleSheet(QStringLiteral("color:%1;border-bottom:1px solid %2;padding-bottom:8px;font-weight:800;")
                                .arg(colors::CYAN(), colors::BORDER_DIM()));
    root->addWidget(identity);

    auto* tabs = new QTabWidget(&dialog);
    root->addWidget(tabs, 1);

    auto* overview = new QTableWidget(0, 2, tabs);
    overview->setHorizontalHeaderLabels({QStringLiteral("FIELD"), QStringLiteral("VALUE")});
    overview->verticalHeader()->hide();
    overview->setEditTriggers(QAbstractItemView::NoEditTriggers);
    overview->setSelectionMode(QAbstractItemView::NoSelection);
    overview->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    overview->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    const auto add_overview = [overview](const QString& label, const QString& value) {
        if (value.trimmed().isEmpty()) return;
        const int row = overview->rowCount();
        overview->insertRow(row);
        auto* key = new QTableWidgetItem(label);
        key->setForeground(QColor(colors::TEXT_SECONDARY()));
        auto* content = new QTableWidgetItem(value);
        content->setForeground(QColor(colors::TEXT_PRIMARY()));
        overview->setItem(row, 0, key);
        overview->setItem(row, 1, content);
    };
    add_overview(QStringLiteral("Contract"), market_id);
    add_overview(QStringLiteral("Question"), question);
    add_overview(QStringLiteral("Mode"), details.value(QStringLiteral("mode")).toString());
    add_overview(QStringLiteral("Origin"), details.value(QStringLiteral("origin")).toString());
    add_overview(QStringLiteral("State"), details.value(QStringLiteral("state")).toString());
    add_overview(QStringLiteral("Contract type"), details.value(QStringLiteral("type")).toString());
    add_overview(QStringLiteral("Side / result"),
                 QStringLiteral("%1%2")
                     .arg(details.value(QStringLiteral("side")).toString(),
                          details.value(QStringLiteral("market_result")).toString().isEmpty()
                              ? QString() : QStringLiteral(" / %1").arg(
                                    details.value(QStringLiteral("market_result")).toString())));
    add_overview(QStringLiteral("Contracts"), value_text(details.value(QStringLiteral("contracts"))));
    const double average_price = details.value(QStringLiteral("average_price")).toDouble(-1.0);
    if (average_price >= 0.0)
        add_overview(QStringLiteral("Average entry"), QStringLiteral("%1c").arg(average_price * 100.0, 0, 'f', 1));
    const double stake = details.value(QStringLiteral("stake")).toDouble(-1.0);
    if (stake >= 0.0) add_overview(QStringLiteral("Stake / cost"), money(stake));
    const double fees = details.value(QStringLiteral("fees")).toDouble(-1.0);
    if (fees >= 0.0) add_overview(QStringLiteral("Fees"), money(fees));
    const double current_value = details.value(QStringLiteral("current_value")).toDouble(-1.0);
    if (current_value >= 0.0) add_overview(QStringLiteral("Executable value"), money(current_value));
    const double payout = details.value(QStringLiteral("payout")).toDouble(-1.0);
    if (payout >= 0.0) add_overview(QStringLiteral("Payout"), money(payout));
    if (details.value(QStringLiteral("pnl")).isDouble())
        add_overview(QStringLiteral("P/L"), money(details.value(QStringLiteral("pnl")).toDouble()));
    else if (details.value(QStringLiteral("realized_pnl")).isDouble())
        add_overview(QStringLiteral("Realized P/L"), money(details.value(QStringLiteral("realized_pnl")).toDouble()));
    add_overview(QStringLiteral("Opened"), local_time_from_ms(details.value(QStringLiteral("opened_at_ms"))));
    add_overview(QStringLiteral("Closed"), local_time_from_ms(details.value(QStringLiteral("closed_at_ms"))));
    add_overview(QStringLiteral("Settled"), details.value(QStringLiteral("settled_time")).toString());
    add_overview(QStringLiteral("Scheduled close"), close_time);
    add_overview(QStringLiteral("Position ID"), details.value(QStringLiteral("position_id")).toString());
    add_overview(QStringLiteral("Decision ID"), details.value(QStringLiteral("decision_id")).toString());
    add_overview(QStringLiteral("Strategy"), details.value(QStringLiteral("strategy_id")).toString());
    add_overview(QStringLiteral("Book"), details.value(QStringLiteral("book")).toString());
    add_overview(QStringLiteral("Close reason"), details.value(QStringLiteral("close_reason")).toString());
    overview->resizeRowsToContents();
    tabs->addTab(overview, QStringLiteral("OVERVIEW"));

    auto* evidence = new QTextEdit(tabs);
    evidence->setReadOnly(true);
    QStringList evidence_lines;
    const QString stored_rationale = details.value(QStringLiteral("rationale")).toString().trimmed();
    if (!stored_rationale.isEmpty())
        evidence_lines << QStringLiteral("WHY THIS POSITION\n%1").arg(stored_rationale);
    QString decision_sql = QStringLiteral(
        "SELECT id,created_at,horizon,side,call,gate,market_probability,model_probability,"
        "edge_after_cost,confidence,seconds_left,reasons,features_json,outcome,resolved_at "
        "FROM edge_decision_journal WHERE market_id=? ORDER BY created_at DESC LIMIT 1");
    QVariantList decision_params{market_id};
    const QString decision_id = details.value(QStringLiteral("decision_id")).toString();
    if (!decision_id.isEmpty()) {
        decision_sql = QStringLiteral(
            "SELECT id,created_at,horizon,side,call,gate,market_probability,model_probability,"
            "edge_after_cost,confidence,seconds_left,reasons,features_json,outcome,resolved_at "
            "FROM edge_decision_journal WHERE id=? LIMIT 1");
        decision_params = {decision_id};
    }
    auto decision = Database::instance().execute(decision_sql, decision_params);
    if (decision.is_ok() && decision.value().next()) {
        const qint64 created_at = decision.value().value(1).toLongLong();
        const QString created = created_at > 0
            ? QDateTime::fromMSecsSinceEpoch(created_at, QTimeZone::UTC)
                  .toLocalTime().toString(QStringLiteral("MMM d, yyyy h:mm:ss ap")) : QString();
        evidence_lines << QStringLiteral(
            "DECISION SNAPSHOT\nID: %1\nTime: %2\nHorizon: %3\nSide: %4\nCall: %5\nGate: %6\n"
            "Market probability: %7%\nModel probability: %8%\nNet edge: %9%\nConfidence: %10%\n"
            "Seconds remaining: %11\nOutcome code: %12\nResolved at: %13\nReasons: %14")
            .arg(decision.value().value(0).toString(), created,
                 decision.value().value(2).toString(), decision.value().value(3).toString(),
                 decision.value().value(4).toString(), decision.value().value(5).toString())
            .arg(decision.value().value(6).toDouble() * 100.0, 0, 'f', 1)
            .arg(decision.value().value(7).toDouble() * 100.0, 0, 'f', 1)
            .arg(decision.value().value(8).toDouble() * 100.0, 0, 'f', 1)
            .arg(decision.value().value(9).toDouble() * 100.0, 0, 'f', 1)
            .arg(decision.value().value(10).toLongLong())
            .arg(decision.value().value(13).toInt())
            .arg(decision.value().value(14).toLongLong())
            .arg(decision.value().value(11).toString());
        const QJsonDocument feature_doc = QJsonDocument::fromJson(
            decision.value().value(12).toString().toUtf8());
        if (feature_doc.isObject()) {
            const QJsonObject signal = feature_doc.object().value(QStringLiteral("signal")).toObject();
            if (!signal.isEmpty()) {
                evidence_lines << QStringLiteral(
                    "MODEL AUTHORITY AUDIT\n"
                    "Probability source: %1\nCalibration bucket: %2\nCalibration samples: %3\n"
                    "Raw model: %4%\nCalibrated model: %5%\nMarket curve: %6%\n"
                    "Model authority: %7%\nCausal check: %8 (%9ms)\nCross-horizon check: %10\n"
                    "Settlement estimate: mean $%11, standard deviation $%12\n"
                    "Exposure snapshot: %13\nEvent risk: %14\nTime-risk score: %15")
                    .arg(signal.value(QStringLiteral("probability_source")).toString(),
                         signal.value(QStringLiteral("calibration_bucket")).toString())
                    .arg(signal.value(QStringLiteral("calibration_samples")).toInt())
                    .arg(signal.value(QStringLiteral("model_probability_raw")).toDouble() * 100.0, 0, 'f', 1)
                    .arg(signal.value(QStringLiteral("calibrated_probability")).toDouble() * 100.0, 0, 'f', 1)
                    .arg(signal.value(QStringLiteral("market_curve_probability")).toDouble() * 100.0, 0, 'f', 1)
                    .arg(signal.value(QStringLiteral("model_weight")).toDouble() * 100.0, 0, 'f', 0)
                    .arg(signal.value(QStringLiteral("causal_eligible")).toBool() ? QStringLiteral("PASS")
                                                                                 : QStringLiteral("BLOCK"))
                    .arg(signal.value(QStringLiteral("causal_lag_ms")).toString())
                    .arg(signal.value(QStringLiteral("cross_horizon_consistent")).toBool()
                             ? QStringLiteral("PASS") : QStringLiteral("BLOCK"))
                    .arg(signal.value(QStringLiteral("settlement_mean")).toDouble(), 0, 'f', 2)
                    .arg(signal.value(QStringLiteral("settlement_stddev")).toDouble(), 0, 'f', 2)
                    .arg(signal.value(QStringLiteral("exposure_snapshot_ready")).toBool(true)
                             ? QStringLiteral("CURRENT")
                             : QStringLiteral("BLOCKED: %1").arg(
                                   signal.value(QStringLiteral("exposure_snapshot_reason"))
                                       .toString(QStringLiteral("unavailable"))))
                    .arg(signal.value(QStringLiteral("event_trading_blocked")).toBool()
                             ? QStringLiteral("BLOCKED")
                             : signal.value(QStringLiteral("event_risk_active")).toBool()
                                 ? QStringLiteral("ACTIVE") : QStringLiteral("CLEAR"))
                    .arg(signal.value(QStringLiteral("time_risk_score")).toDouble(), 0, 'f', 3);
            }
            evidence_lines << QStringLiteral("STORED FEATURE SNAPSHOT\n%1")
                                  .arg(QString::fromUtf8(feature_doc.toJson(QJsonDocument::Indented)));
        }
    } else if (stored_rationale.isEmpty()) {
        evidence_lines << QStringLiteral(
            "No local model snapshot is attached. This is expected for a human order created outside OpenTerminal.");
    }
    evidence->setPlainText(evidence_lines.join(QStringLiteral("\n\n")));
    tabs->addTab(evidence, QStringLiteral("DECISION EVIDENCE"));

    auto* timeline = new QTableWidget(0, 4, tabs);
    timeline->setHorizontalHeaderLabels({QStringLiteral("TIME"), QStringLiteral("PHASE"),
                                         QStringLiteral("RESULT"), QStringLiteral("DETAIL")});
    timeline->verticalHeader()->hide();
    timeline->setEditTriggers(QAbstractItemView::NoEditTriggers);
    timeline->setSelectionBehavior(QAbstractItemView::SelectRows);
    timeline->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    timeline->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    timeline->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    timeline->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    auto audit = Database::instance().execute(
        "SELECT ts,phase,decision,reason FROM trade_audit "
        "WHERE json_extract(intent_json,'$.venue')='kalshi' "
        "AND json_extract(intent_json,'$.market_id')=? ORDER BY id DESC LIMIT 100", {market_id});
    if (audit.is_ok()) {
        while (audit.value().next()) {
            const int row = timeline->rowCount();
            timeline->insertRow(row);
            const QDateTime timestamp = QDateTime::fromString(audit.value().value(0).toString(), Qt::ISODate);
            const QString shown_time = timestamp.isValid()
                ? timestamp.toLocalTime().toString(QStringLiteral("MMM d h:mm:ss ap"))
                : audit.value().value(0).toString();
            timeline->setItem(row, 0, new QTableWidgetItem(shown_time));
            timeline->setItem(row, 1, new QTableWidgetItem(audit.value().value(1).toString().toUpper()));
            auto* result = new QTableWidgetItem(audit.value().value(2).toString().toUpper());
            const QString result_text = result->text().toLower();
            result->setForeground(QColor(
                result_text.contains(QStringLiteral("fail")) || result_text.contains(QStringLiteral("reject"))
                    ? colors::NEGATIVE() : colors::POSITIVE()));
            timeline->setItem(row, 2, result);
            timeline->setItem(row, 3, new QTableWidgetItem(audit.value().value(3).toString()));
        }
    }
    if (timeline->rowCount() == 0) {
        timeline->insertRow(0);
        timeline->setSpan(0, 0, 1, 4);
        timeline->setItem(0, 0, new QTableWidgetItem(
            QStringLiteral("No local order events are attached. Human orders placed outside OpenTerminal may only have account settlement data.")));
    }
    timeline->resizeRowsToContents();
    tabs->addTab(timeline, QStringLiteral("ORDER / AUDIT TIMELINE"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    root->addWidget(buttons);
    dialog.exec();
}

void KalshiScreen::cash_out_selected_position() {
    if (!has_selection_ || !adapter() || !adapter()->has_credentials()) return;
    const auto position_it = std::find_if(positions_.cbegin(), positions_.cend(), [this](const auto& position) {
        return position.market_id == selected_.key.market_id && position.size > 0.0;
    });
    if (position_it == positions_.cend()) return;
    const auto book_it = kalshi_books_.constFind(position_it->asset_id);
    if (book_it == kalshi_books_.cend() || book_it->bids.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Cash out unavailable"),
                             QStringLiteral("No executable bid is available for this position yet."));
        return;
    }

    const double bid = book_it->bids.first().price;
    const double fee = kalshi_data::KalshiEvidenceEngine::conservative_taker_fee(
        selected_, bid, position_it->size);
    const double net = qMax(0.0, bid * position_it->size - fee);
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Confirm cash out"),
        QStringLiteral("Sell up to %1 %2 contracts at %3.\n\n"
                       "Estimated proceeds: %4 after estimated fee %5.\n"
                       "Execution is immediate-or-cancel: unfilled contracts cancel.\n"
                       "Reduce-only prevents this order from increasing or reversing your position.")
            .arg(position_it->size, 0, 'f', 2).arg(position_it->outcome)
            .arg(probability(bid)).arg(money(net)).arg(money(fee)),
        QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;

    pred::OrderRequest request;
    request.key = selected_.key;
    request.asset_id = position_it->asset_id;
    request.side = QStringLiteral("SELL");
    request.order_type = QStringLiteral("FAK");
    request.price = bid;
    request.size = position_it->size;
    request.extras.insert(QStringLiteral("reduce_only"), true);
    request.extras.insert(QStringLiteral("post_only"), false);
    request.extras.insert(QStringLiteral("cancel_order_on_pause"), true);
    pending_order_kind_ = QStringLiteral("cashout");
    cashout_button_->setEnabled(false);
    cashout_label_->setText(QStringLiteral("CASH-OUT: submitting reduce-only exit…"));
    adapter()->place_order(request);
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
    if (contract_strip_) {
        contract_strip_->setText(QStringLiteral("KALSHI EXECUTABLE CONTRACT · %1 · %2 · BOOK %3")
                                     .arg(quote_text(0, QStringLiteral("YES")),
                                          quote_text(1, QStringLiteral("NO")), state));
        contract_strip_->setStyleSheet(QStringLiteral(
            "color:%1;background:%2;border:1px solid %3;padding:5px 8px;font-size:10px;font-weight:900;")
                                          .arg(color, colors::BG_RAISED(), colors::BORDER_DIM()));
    }

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
    contract_chart_->append_candle(tick);
    live_chart_seeded_ = true;
    chart_status_->hide();
}

bool KalshiScreen::record_account_fills(const QVariantList& activities) {
    if (!trade_ledger_loaded_) {
        trade_ledger_loaded_ = true;
        QFile existing(evidence_path(QStringLiteral("kalshi-trade-ledger.jsonl")));
        if (existing.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!existing.atEnd()) {
                const QJsonDocument document = QJsonDocument::fromJson(existing.readLine());
                if (!document.isObject()) continue;
                const QJsonObject row = document.object();
                if (row.value(QStringLiteral("event")).toString() != QStringLiteral("kalshi_account_fill"))
                    continue;
                const QString id = row.value(QStringLiteral("fill_id")).toString();
                if (!id.isEmpty()) recorded_fill_ids_.insert(id);
            }
        }
    }
    bool recorded_any = false;
    for (const QVariant& value : activities) {
        const QJsonObject fill = QJsonObject::fromVariantMap(value.toMap());
        const QString fill_id = fill.value(QStringLiteral("fill_id")).toString();
        const QString fallback_id = fill.value(QStringLiteral("order_id")).toString()
            + QLatin1Char(':') + fill.value(QStringLiteral("trade_id")).toString()
            + QLatin1Char(':') + QString::number(qint64(fill.value(QStringLiteral("ts_ms")).toDouble()));
        const QString identity = fill_id.isEmpty() ? fallback_id : fill_id;
        if (identity.isEmpty() || recorded_fill_ids_.contains(identity)) continue;

        const double contracts = fill.value(QStringLiteral("size")).toDouble();
        const double price = fill.value(QStringLiteral("price")).toDouble();
        const double fee = fill.value(QStringLiteral("fee_cost")).toDouble();
        const QString action = fill.value(QStringLiteral("action")).toString().toUpper();
        const double cash_delta = action == QStringLiteral("SELL")
            ? price * contracts - fee : -(price * contracts + fee);
        QJsonObject row = fill;
        row.insert(QStringLiteral("event"), QStringLiteral("kalshi_account_fill"));
        row.insert(QStringLiteral("recorded_ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        row.insert(QStringLiteral("contracts"), contracts);
        row.insert(QStringLiteral("gross_notional"), price * contracts);
        row.insert(QStringLiteral("net_cash_delta"), cash_delta);
        row.insert(QStringLiteral("liquidity"), fill.value(QStringLiteral("is_taker")).toBool()
                       ? QStringLiteral("taker") : QStringLiteral("maker"));
        const auto creds = pred::PredictionCredentialStore::load_kalshi();
        row.insert(QStringLiteral("account_mode"), creds && creds->use_demo
                   ? QStringLiteral("demo") : QStringLiteral("live"));
        if (!kalshi_data::KalshiEvidenceEngine::append_jsonl(
                evidence_path(QStringLiteral("kalshi-trade-ledger.jsonl")), row))
            continue;
        recorded_fill_ids_.insert(identity);
        recorded_any = true;
    }
    return recorded_any;
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
    QJsonObject snapshot = kalshi_data::KalshiEvidenceEngine::ladder_snapshot(
        all_markets_, kalshi_books_, selected_.key.event_id, now);
    services::edge_radar::KalshiAutoContext auto_context;
    const bool official_fresh_for_auto = official_settlement_reference_ > 0.0 &&
                                         official_settlement_reference_ms_ > 0 &&
                                         now - official_settlement_reference_ms_ <= 3'000;
    auto_context.spot = official_fresh_for_auto ? official_settlement_reference_ : reference_spot_;
    auto_context.decision_ts_ms = now;
    auto_context.spot_observed_at_ms = official_fresh_for_auto
        ? official_settlement_reference_ms_ : reference_spot_last_update_ms_;
    const QString volatility_symbol = asset_.trimmed().toUpper();
    refresh_volatility_estimate(volatility_symbol, now);
    auto volatility = volatility_cache_.value(volatility_symbol);
    if (!volatility_cache_.contains(volatility_symbol))
        volatility.reason = QStringLiteral("background volatility refresh pending");
    auto_context.annual_volatility = volatility.annual_volatility;
    auto_context.volatility_ready = volatility.ready;
    auto_context.volatility_time_of_day_multiplier = volatility.time_of_day_multiplier;
    auto_context.volatility_sample_count = volatility.sample_count;
    auto_context.volatility_observed_at_ms = volatility.observed_at_ms;
    auto_context.volatility_source = volatility.source;
    auto_context.volatility_reason = volatility.reason;
    const auto model_outputs = EdgePredictionModelRepository::instance().list_model_outputs(
        asset_.trimmed().toUpper(), now, 40);
    auto effective_model_outputs = model_outputs;
    if (effective_model_outputs.is_ok() && effective_model_outputs.value().isEmpty())
        effective_model_outputs = EdgePredictionModelRepository::instance().list_model_outputs(
            asset_.trimmed().toUpper() + QStringLiteral("-USD"), now, 40);
    if (effective_model_outputs.is_ok()) {
        QSet<QString> seen_horizons;
        for (const auto& output : effective_model_outputs.value()) {
            const QString horizon = output.horizon.trimmed().toLower();
            if (seen_horizons.contains(horizon) ||
                !QStringList{QStringLiteral("5m"), QStringLiteral("15m"),
                             QStringLiteral("1h"), QStringLiteral("1d")}.contains(horizon))
                continue;
            seen_horizons.insert(horizon);
            double up_probability = output.probability;
            if (output.direction.compare(QStringLiteral("down"), Qt::CaseInsensitive) == 0)
                up_probability = 1.0 - up_probability;
            else if (output.direction.compare(QStringLiteral("flat"), Qt::CaseInsensitive) == 0)
                up_probability = 0.5;
            auto_context.horizon_signals.append(services::edge_radar::KalshiHorizonSignal{
                horizon, up_probability, output.confidence, output.calibration_score,
                output.sample_count, output.source, output.as_of});
        }
    }
    const auto auto_surface = services::edge_radar::KalshiAutoEngine::build_surface(
        all_markets_, kalshi_books_, auto_context, selected_.key.event_id);
    services::edge_radar::KalshiPortfolioConstraints auto_limits;
    auto_limits.max_positions = cadence_ == QStringLiteral("hourly") ? 5 : 3;
    auto_limits.max_same_side = cadence_ == QStringLiteral("hourly") ? 4 : 2;
    const auto auto_plan = services::edge_radar::KalshiAutoEngine::optimize(
        auto_surface, auto_context, auto_limits);
    const QJsonObject news_pulse = asset_.trimmed().compare(QStringLiteral("BTC"), Qt::CaseInsensitive) == 0
        ? fresh_btc_news_pulse(now) : QJsonObject{};
    const QJsonObject btc_intelligence = asset_.trimmed().compare(QStringLiteral("BTC"), Qt::CaseInsensitive) == 0
        ? fresh_btc_intelligence(now) : QJsonObject{};
    snapshot.insert(QStringLiteral("reference_spot"), auto_context.spot);
    snapshot.insert(QStringLiteral("spot_observed_at_ms"), QString::number(auto_context.spot_observed_at_ms));
    snapshot.insert(QStringLiteral("spot_source"), official_fresh_for_auto
                        ? QStringLiteral("kalshi-cf-benchmarks") : reference_spot_source_);
    snapshot.insert(QStringLiteral("annual_volatility"), auto_context.annual_volatility);
    snapshot.insert(QStringLiteral("volatility_ready"), auto_context.volatility_ready);
    snapshot.insert(QStringLiteral("volatility_samples"), auto_context.volatility_sample_count);
    snapshot.insert(QStringLiteral("volatility_observed_at_ms"),
                    QString::number(auto_context.volatility_observed_at_ms));
    snapshot.insert(QStringLiteral("volatility_time_of_day_multiplier"),
                    auto_context.volatility_time_of_day_multiplier);
    snapshot.insert(QStringLiteral("volatility_source"), auto_context.volatility_source);
    snapshot.insert(QStringLiteral("volatility_reason"), auto_context.volatility_reason);
    QJsonArray horizon_evidence;
    for (const auto& signal : auto_context.horizon_signals) {
        horizon_evidence.append(QJsonObject{{QStringLiteral("horizon"), signal.horizon},
                                             {QStringLiteral("up_probability"), signal.up_probability},
                                             {QStringLiteral("confidence"), signal.confidence},
                                             {QStringLiteral("calibration_score"), signal.calibration_score},
                                             {QStringLiteral("sample_count"), signal.sample_count},
                                             {QStringLiteral("source"), signal.source},
                                             {QStringLiteral("observed_at_ms"), QString::number(signal.observed_at_ms)}});
    }
    snapshot.insert(QStringLiteral("horizon_signals"), horizon_evidence);
    snapshot.insert(QStringLiteral("probability_surface"),
                    services::edge_radar::KalshiAutoEngine::surface_to_json(auto_surface));
    snapshot.insert(QStringLiteral("portfolio_plan"),
                    services::edge_radar::KalshiAutoEngine::plan_to_json(auto_plan));
    snapshot.insert(QStringLiteral("news_context"), news_pulse);
    snapshot.insert(QStringLiteral("evidence_intelligence"), btc_intelligence);
    if (kalshi_data::KalshiEvidenceEngine::append_jsonl(
            evidence_path(QStringLiteral("kalshi-ladders.jsonl")), snapshot))
        ++ladder_snapshots_recorded_;

    // Mirror the exact coherent Auto Cockpit selections into a dedicated
    // counterfactual stream. One row per contract/side/minute keeps the file
    // useful without duplicating every 5-second repaint.
    for (const auto& leg : auto_plan.legs) {
        const auto point = std::find_if(auto_surface.cbegin(), auto_surface.cend(), [&leg](const auto& row) {
            return row.ticker == leg.ticker;
        });
        if (point == auto_surface.cend()) continue;
        const QString key = QStringLiteral("%1:%2:%3")
            .arg(leg.ticker, leg.side).arg(now / 60'000);
        if (recorded_auto_shadow_keys_.contains(key)) continue;
        QJsonObject row{{QStringLiteral("event"), QStringLiteral("kalshi_auto_shadow_candidate")},
                        {QStringLiteral("shadow_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                        {QStringLiteral("decision_ts_ms"), QString::number(now)},
                        {QStringLiteral("event_ticker"), selected_.key.event_id},
                        {QStringLiteral("market_ticker"), leg.ticker},
                        {QStringLiteral("side"), leg.side},
                        {QStringLiteral("kind"), leg.kind},
                        {QStringLiteral("floor"), leg.floor},
                        {QStringLiteral("cap"), leg.cap},
                        {QStringLiteral("contracts"), leg.contracts},
                        {QStringLiteral("entry_price"), leg.entry_price},
                        {QStringLiteral("entry_fee"), leg.entry_fee},
                        {QStringLiteral("fair_probability"), point->selected_fair},
                        {QStringLiteral("net_edge"), point->net_edge},
                        {QStringLiteral("seconds_left"), point->seconds_left},
                        {QStringLiteral("entry_cohort"), point->seconds_left <= 180
                             ? QStringLiteral("late") : point->seconds_left <= 900
                             ? QStringLiteral("middle") : QStringLiteral("early")},
                        {QStringLiteral("spot"), auto_context.spot},
                        {QStringLiteral("spot_source"), snapshot.value(QStringLiteral("spot_source"))},
                        {QStringLiteral("context_sources"), point->context_sources},
                        {QStringLiteral("context_sample_count"), point->context_sample_count},
                        {QStringLiteral("news_context"), news_pulse},
                        {QStringLiteral("evidence_intelligence"), btc_intelligence},
                        {QStringLiteral("execution"), QStringLiteral("counterfactual_only")},
                        {QStringLiteral("outcome"), QStringLiteral("pending")}};
        if (kalshi_data::KalshiEvidenceEngine::append_jsonl(
                evidence_path(QStringLiteral("kalshi-auto-shadow.jsonl")), row)) {
            recorded_auto_shadow_keys_.insert(key);
            ++auto_shadow_records_;
        }
    }

    if (gate_label_ && family_ == QStringLiteral("Crypto")) {
        services::edge_radar::KalshiUniversalOptions options;
        options.minimum_net_edge = 0.05;
        const bool official_fresh = official_settlement_reference_ > 0.0 &&
                                    official_settlement_reference_ms_ > 0 &&
                                    now - official_settlement_reference_ms_ <= 3'000;
        const double model_reference = official_fresh ? official_settlement_reference_ : reference_spot_;
        const auto decision = services::edge_radar::KalshiUniversalEdgeModel::score_crypto_target(
            selected_, model_reference, options, now,
            official_fresh ? QStringLiteral("kalshi-cf-benchmarks")
                           : QStringLiteral("gui-cross-exchange-spot-proxy"));
        const QString verdict = decision.passes_gate
            ? QStringLiteral("PAPER CANDIDATE")
            : decision.seconds_left >= 0 && decision.seconds_left < options.minimum_seconds_left
                ? QStringLiteral("TOO LATE")
                : decision.is_valid ? QStringLiteral("NO TRADE") : QStringLiteral("NOT ENOUGH DATA");
        const QString tone = decision.passes_gate ? colors::GREEN()
                                                  : decision.is_valid ? colors::WARNING() : colors::RED();
        gate_label_->setText(QStringLiteral("%1 · %2 %3c\nModel %4 · net after cost %5c\nReference: %6%7\n%8\nLIVE ORDERS REQUIRE A BOUNDED ARMED SESSION")
                                 .arg(verdict, decision.side.toUpper())
                                 .arg(decision.executable_price * 100.0, 0, 'f', 1)
                                 .arg(decision.model_probability * 100.0, 0, 'f', 1)
                                 .arg(decision.gate_edge * 100.0, 0, 'f', 1)
                                 .arg(official_fresh ? official_settlement_index_
                                                     : QStringLiteral("SPOT PROXY"))
                                 .arg(official_fresh ? QStringLiteral(" (OFFICIAL)")
                                                     : QStringLiteral(" (FALLBACK)"))
                                 .arg(decision.rejection_reasons.isEmpty()
                                          ? decision.rationale : decision.rejection_reasons));
        gate_label_->setStyleSheet(QStringLiteral("color:%1;border:1px solid %1;padding:9px;font-weight:800;")
                                       .arg(tone));
        QJsonObject row = services::edge_radar::KalshiUniversalEdgeModel::to_json(decision);
        row.insert(QStringLiteral("event"), QStringLiteral("kalshi_crypto_paper_decision"));
        row.insert(QStringLiteral("ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        row.insert(QStringLiteral("live_eligible"), false);
        row.insert(QStringLiteral("shadow_fill_model"), shadow_enabled_ ? QStringLiteral("active")
                                                                         : QStringLiteral("paused"));
        kalshi_data::KalshiEvidenceEngine::append_jsonl(
            evidence_path(QStringLiteral("kalshi-crypto-decisions.jsonl")), row);
    }
    const QJsonArray diagnostics = snapshot.value(QStringLiteral("diagnostics")).toArray();
    int opportunities = 0;
    QString top;
    for (const auto& value : diagnostics) {
        const auto row = value.toObject();
        if (row.value(QStringLiteral("severity")).toString() == QStringLiteral("opportunity")) ++opportunities;
        if (top.isEmpty()) top = row.value(QStringLiteral("detail")).toString();
    }
    const QString news_line = news_pulse.isEmpty() ? QString()
        : QStringLiteral("\nNEWS %1 · score %2 · confidence %3% · ADVISORY ONLY")
              .arg(news_pulse.value(QStringLiteral("verdict")).toString())
              .arg(news_pulse.value(QStringLiteral("score")).toDouble(), 0, 'f', 1)
              .arg(news_pulse.value(QStringLiteral("confidence")).toDouble() * 100.0, 0, 'f', 0);
    const QJsonObject intelligence_calibration = btc_intelligence.value(QStringLiteral("calibration")).toObject();
    const QString intelligence_line = btc_intelligence.isEmpty() ? QString()
        : QStringLiteral("\nEVIDENCE %1 · reaction %2 · regime %3 · %4 · %5 samples · stability %6% · timed confidence %7% · ADVISORY ONLY")
              .arg(btc_intelligence.value(QStringLiteral("verdict")).toString(),
                   btc_intelligence.value(QStringLiteral("market_reaction")).toObject()
                       .value(QStringLiteral("label")).toString(),
                   btc_intelligence.value(QStringLiteral("regime")).toObject()
                       .value(QStringLiteral("label")).toString(),
                   intelligence_calibration.value(QStringLiteral("time_bucket")).toString())
              .arg(intelligence_calibration.value(QStringLiteral("samples")).toInt())
              .arg(intelligence_calibration.value(QStringLiteral("late_market_stability")).toDouble() * 100.0, 0, 'f', 0)
              .arg(intelligence_calibration.value(QStringLiteral("time_conditioned_confidence")).toDouble() * 100.0, 0, 'f', 0);
    ladder_status_->setText(QStringLiteral("AUTO COCKPIT · RESEARCH + BOUNDED LIVE EXECUTION\n%1 contracts · %2 selected · cost $%3 · expected $%4 · worst $%5\n%6%7%8")
                                .arg(snapshot.value(QStringLiteral("contracts")).toArray().size())
                                .arg(auto_plan.legs.size())
                                .arg(auto_plan.total_cost, 0, 'f', 2)
                                .arg(auto_plan.expected_pnl, 0, 'f', 2)
                                .arg(auto_plan.worst_case_pnl, 0, 'f', 2)
                                .arg(auto_plan.verdict)
                                .arg(top.isEmpty() ? QString() : QStringLiteral(" · ") + top)
                                .arg(news_line + intelligence_line));
    ladder_status_->setStyleSheet(QStringLiteral("color:%1;border-top:1px solid %2;padding-top:8px;")
                                      .arg(opportunities > 0 ? colors::GREEN() : colors::TEXT_SECONDARY(),
                                           colors::BORDER_DIM()));
    render_ladder_surface(auto_surface, auto_plan, diagnostics);
}

void KalshiScreen::refresh_volatility_estimate(const QString& symbol, qint64 decision_ts_ms) {
    const QString normalized = symbol.trimmed().toUpper();
    if (normalized.isEmpty() || decision_ts_ms <= 0) return;
    constexpr qint64 kRefreshIntervalMs = 60'000;
    if (decision_ts_ms - volatility_cache_refreshed_ms_.value(normalized) < kRefreshIntervalMs)
        return;
    bool expected = false;
    if (!volatility_fetching_.compare_exchange_strong(expected, true)) return;

    QPointer<KalshiScreen> self(this);
    (void)QtConcurrent::run([self, normalized, decision_ts_ms]() {
        const auto estimate_for = [decision_ts_ms](const QString& series_symbol) {
            QVector<services::edge_radar::KalshiTimedPrice> prices;
            const auto series = EdgePredictionModelRepository::instance().list_recent_price_series_since(
                series_symbol, decision_ts_ms - 24LL * 60 * 60 * 1000, 1'500);
            if (series.is_ok()) {
                prices.reserve(series.value().size());
                for (const auto& tick : series.value())
                    prices.append({tick.exchange_ts, tick.price});
            }
            return services::edge_radar::KalshiAutoEngine::estimate_realized_volatility(
                prices, decision_ts_ms);
        };

        auto estimate = estimate_for(normalized);
        if ((!estimate.ready || estimate.sample_count < 30) &&
            !normalized.endsWith(QStringLiteral("-USD"))) {
            const auto usd_estimate = estimate_for(normalized + QStringLiteral("-USD"));
            if (usd_estimate.ready || usd_estimate.sample_count > estimate.sample_count)
                estimate = usd_estimate;
        }
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, normalized, decision_ts_ms, estimate]() {
            if (!self) return;
            self->volatility_cache_.insert(normalized, estimate);
            self->volatility_cache_refreshed_ms_.insert(normalized, decision_ts_ms);
            self->volatility_fetching_ = false;
        }, Qt::QueuedConnection);
    });
}

void KalshiScreen::refresh_flow_meter() {
    if (!flow_status_) return;
    if (!has_selection_ || selected_.key.market_id.isEmpty()) {
        flow_status_->setText(QStringLiteral("FLOW METER · select a contract to view its public book and trade flow"));
        if (flow_detail_) flow_detail_->setText(QStringLiteral("FLOW CONTEXT · select a contract"));
        return;
    }
    QFile file(evidence_path(QStringLiteral("kalshi-ws-books.json")));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        flow_status_->setText(QStringLiteral("FLOW METER · waiting for the local daemon WebSocket snapshot"));
        if (flow_detail_) flow_detail_->setText(QStringLiteral("FLOW CONTEXT · daemon snapshot unavailable"));
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject root = document.object();
    const QJsonObject snapshot = root.value(QStringLiteral("snapshots")).toObject()
        .value(selected_.key.market_id).toObject();
    const QJsonObject row = snapshot.isEmpty()
        ? root.value(QStringLiteral("flow")).toObject().value(selected_.key.market_id).toObject()
        : snapshot;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 observed = row.value(QStringLiteral("observed_at_ms")).toString().toLongLong();
    if (row.isEmpty() || observed <= 0 || observed > now || now - observed > 30'000) {
        flow_status_->setText(QStringLiteral("FLOW METER · no fresh 30s snapshot for this contract"));
        if (flow_detail_) flow_detail_->setText(QStringLiteral("FLOW CONTEXT · waiting for fresh WebSocket evidence"));
        return;
    }
    const QString signal = row.value(QStringLiteral("signal")).toString(QStringLiteral("MIXED"));
    const QString confidence = row.value(QStringLiteral("confidence")).toString(QStringLiteral("LOW"));
    const double pressure = row.value(QStringLiteral("combined_pressure")).toDouble();
    const QString pressure_text = QStringLiteral("%1%2")
        .arg(pressure >= 0.0 ? QStringLiteral("+") : QString())
        .arg(pressure, 0, 'f', 2);
    const QColor color = signal.startsWith(QStringLiteral("YES")) ? QColor(colors::GREEN())
                        : signal.startsWith(QStringLiteral("NO")) ? QColor(colors::RED())
                                                                     : QColor(colors::TEXT_SECONDARY());
    flow_status_->setText(QStringLiteral(
        "FLOW METER · %1 · %2 · 30s · resting YES %3 / NO %4 · taker YES %5 / NO %6 · pressure %7")
                              .arg(signal, confidence)
                              .arg(row.value(QStringLiteral("yes_bid_depth")).toDouble(), 0, 'f', 1)
                              .arg(row.value(QStringLiteral("no_bid_depth")).toDouble(), 0, 'f', 1)
                              .arg(row.value(QStringLiteral("yes_taker_quantity")).toDouble(), 0, 'f', 1)
                              .arg(row.value(QStringLiteral("no_taker_quantity")).toDouble(), 0, 'f', 1)
                              .arg(pressure_text));
    flow_status_->setStyleSheet(QStringLiteral(
        "color:%1;background:%2;border:1px solid %3;padding:7px;font-weight:800;")
                                    .arg(color.name(), colors::BG_RAISED(), colors::BORDER_DIM()));
    if (!flow_detail_ || snapshot.isEmpty()) return;
    const QJsonObject windows = snapshot.value(QStringLiteral("flow")).toObject()
        .value(QStringLiteral("windows")).toObject();
    const auto compact_window = [&windows](const QString& key) {
        const QJsonObject window = windows.value(key).toObject();
        const double pressure = window.value(QStringLiteral("combined_pressure")).toDouble();
        return QStringLiteral("%1 %2 %3%4")
            .arg(key, window.value(QStringLiteral("signal")).toString(QStringLiteral("MIXED")))
            .arg(pressure >= 0.0 ? QStringLiteral("+") : QString())
            .arg(pressure, 0, 'f', 2);
    };
    const QJsonObject divergence = snapshot.value(QStringLiteral("flow")).toObject()
        .value(QStringLiteral("divergence")).toObject();
    const QJsonObject execution = snapshot.value(QStringLiteral("execution")).toObject();
    const QJsonObject yes = execution.value(QStringLiteral("yes")).toObject();
    const QJsonObject no = execution.value(QStringLiteral("no")).toObject();
    const QJsonObject ladder = snapshot.value(QStringLiteral("ladder")).toObject();
    flow_detail_->setText(QStringLiteral(
        "FLOW CONTEXT · %1 · %2 · %3 · %4 · YES immediate loss %5c · NO %6c · ladder opportunities %7 · advisory only")
        .arg(compact_window(QStringLiteral("2s")), compact_window(QStringLiteral("30s")),
             compact_window(QStringLiteral("5m")), divergence.value(QStringLiteral("label")).toString(QStringLiteral("QUIET")))
        .arg(yes.value(QStringLiteral("immediate_round_trip_loss")).toDouble() * 100.0, 0, 'f', 1)
        .arg(no.value(QStringLiteral("immediate_round_trip_loss")).toDouble() * 100.0, 0, 'f', 1)
        .arg(ladder.value(QStringLiteral("actionable_count")).toInt()));
}

void KalshiScreen::render_ladder_surface(
    const QVector<services::edge_radar::KalshiSurfacePoint>& surface,
    const services::edge_radar::KalshiPortfolioPlan& plan,
    const QJsonArray& diagnostics) {
    if (!ladder_table_) return;
    QHash<QString, QJsonObject> stored_signals;
    const qint64 stored_cutoff = QDateTime::currentMSecsSinceEpoch() - 5 * 60'000;
    auto stored = Database::instance().execute(
        "SELECT market_id,features_json FROM edge_decision_journal "
        "WHERE source='kalshi auto-plan' AND created_at>=? ORDER BY created_at DESC LIMIT 500",
        {stored_cutoff});
    if (stored.is_ok()) {
        while (stored.value().next()) {
            const QString ticker = stored.value().value(0).toString();
            if (ticker.isEmpty() || stored_signals.contains(ticker)) continue;
            const QJsonDocument document = QJsonDocument::fromJson(
                stored.value().value(1).toString().toUtf8());
            const QJsonObject signal = document.object().value(QStringLiteral("signal")).toObject();
            if (!signal.isEmpty()) stored_signals.insert(ticker, signal);
        }
    }
    QSet<QString> selected_tickers;
    for (const auto& leg : plan.legs) selected_tickers.insert(leg.ticker);
    ladder_table_->setRowCount(qMin(15, surface.size()));
    if (surface.isEmpty()) {
        ladder_table_->setRowCount(1);
        ladder_table_->setItem(0, 0, new QTableWidgetItem(QStringLiteral("WAITING")));
        ladder_table_->setItem(0, 7, new QTableWidgetItem(
            QStringLiteral("Waiting for a complete event, fresh spot, and executable books.")));
        return;
    }
    for (int row = 0; row < ladder_table_->rowCount(); ++row) {
        const auto& point = surface[row];
        const bool selected = selected_tickers.contains(point.ticker);
        const QJsonObject stored_signal = stored_signals.value(point.ticker);
        const bool stored_audit = !stored_signal.isEmpty();
        const bool yes_side = point.selected_side.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0;
        const auto selected_probability = [yes_side](double yes_probability) {
            return yes_side ? yes_probability : 1.0 - yes_probability;
        };
        const double raw_probability = stored_audit
            ? selected_probability(stored_signal.value(QStringLiteral("model_probability_raw")).toDouble(0.5))
            : selected_probability(point.model_probability_raw);
        const double calibrated_probability = stored_audit
            ? selected_probability(stored_signal.value(QStringLiteral("calibrated_probability")).toDouble(0.5))
            : selected_probability(point.calibrated_probability);
        const double market_probability = stored_audit
            ? selected_probability(stored_signal.value(QStringLiteral("market_curve_probability")).toDouble(0.5))
            : selected_probability(point.market_curve_probability);
        const double model_weight = stored_audit
            ? stored_signal.value(QStringLiteral("model_weight")).toDouble() : point.model_weight;
        const bool causal = stored_audit
            ? stored_signal.value(QStringLiteral("causal_eligible")).toBool() : point.causal_eligible;
        const bool horizons = stored_audit
            ? stored_signal.value(QStringLiteral("cross_horizon_consistent")).toBool()
            : point.cross_horizon_consistent;
        QString audit = point.context_sample_count > 0
            ? QStringLiteral("%1 samples · %2ms fresh")
                  .arg(point.context_sample_count).arg(point.context_freshness_ms)
            : QStringLiteral("baseline model · collecting calibration");
        QString action = selected ? QStringLiteral("MODEL CANDIDATE - NOT ORDERED")
                                  : point.valid ? QStringLiteral("NO TRADE")
                                                : point.rejection_reason;
        if (!diagnostics.isEmpty() && !selected)
            audit += QStringLiteral(" · ladder checks active");
        action += QStringLiteral(" · %1 · causal %2 · horizons %3 · %4")
                      .arg(stored_audit ? QStringLiteral("DAEMON SNAPSHOT") : QStringLiteral("LOCAL PREVIEW"),
                           causal ? QStringLiteral("PASS") : QStringLiteral("BLOCK"),
                           horizons ? QStringLiteral("PASS") : QStringLiteral("BLOCK"), audit);
        const QStringList values{
            point.ticker,
            point.selected_side.toUpper(),
            point.selected_ask > 0.0 ? QStringLiteral("%1c").arg(point.selected_ask * 100.0, 0, 'f', 1)
                                     : QStringLiteral("—"),
            point.valid ? QStringLiteral("%1%").arg(market_probability * 100.0, 0, 'f', 1)
                        : QStringLiteral("—"),
            point.valid ? QStringLiteral("%1 -> %2%")
                              .arg(raw_probability * 100.0, 0, 'f', 1)
                              .arg(calibrated_probability * 100.0, 0, 'f', 1)
                        : QStringLiteral("—"),
            point.valid ? QStringLiteral("%1%").arg(model_weight * 100.0, 0, 'f', 0)
                        : QStringLiteral("—"),
            point.valid ? QStringLiteral("%1c").arg(point.net_edge * 100.0, 0, 'f', 1)
                        : QStringLiteral("—"),
            action};
        const QColor foreground(selected ? colors::GREEN()
                                : point.valid && point.net_edge > 0.0 ? colors::WARNING()
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
        QStringLiteral("%1 %2\n%3 contracts at %4\nNotional %5\nMaximum fee estimate %6\nMaximum loss %7\nPayout if correct %8\n\nThis review does not submit an order. Use PLACE LIVE MAKER ORDER to submit a post-only, good-til-canceled limit order after confirmation. Automated strategies remain separately locked.")
            .arg(QStringLiteral("BUY"), side_).arg(n).arg(probability(p)).arg(money(p * n)).arg(money(fee))
            .arg(money(p * n + fee)).arg(money(n)));
}

void KalshiScreen::place_live_order() {
    if (!has_selection_) {
        QMessageBox::information(this, QStringLiteral("Kalshi"), QStringLiteral("Select a contract first."));
        return;
    }
    auto* active_adapter = adapter();
    if (!active_adapter || !active_adapter->has_credentials()) {
        QMessageBox::information(this, QStringLiteral("Connect Kalshi account"),
                                 QStringLiteral("Connect and test your Kalshi account before submitting a manual order."));
        show_account_dialog();
        return;
    }
    const auto creds = pred::PredictionCredentialStore::load_kalshi();
    if (!creds) {
        QMessageBox::warning(this, QStringLiteral("Kalshi credentials unavailable"),
                             QStringLiteral("Your configured credentials could not be loaded. Reconnect the account and test it."));
        show_account_dialog();
        return;
    }
    const int outcome_index = side_ == QStringLiteral("NO") ? 1 : 0;
    const QString asset_id = selected_.key.asset_ids.value(outcome_index);
    if (asset_id.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Kalshi order unavailable"),
                             QStringLiteral("This contract does not expose an executable %1 outcome.").arg(side_));
        return;
    }

    const double p = price_->value();
    const int n = contracts_->value();
    const double fee = kalshi_data::KalshiEvidenceEngine::conservative_taker_fee(selected_, p, n);
    const QString destination = creds->use_demo ? QStringLiteral("DEMO") : QStringLiteral("LIVE");
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("Confirm %1 maker order").arg(destination.toLower()),
        QStringLiteral("%1 %2\n\n%3 contracts at %4\nNotional: %5\nMaximum fee estimate: %6\nMaximum loss: %7\nPayout if correct: %8\n\n"
                       "This submits a POST-ONLY, GOOD-TIL-CANCELED limit order to your %9 Kalshi account. "
                       "It may rest unfilled; it will not cross the book as a taker.")
            .arg(QStringLiteral("BUY"), side_)
            .arg(n).arg(probability(p)).arg(money(p * n)).arg(money(fee))
            .arg(money(p * n + fee)).arg(money(n)).arg(destination),
        QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;

    pred::OrderRequest request;
    request.key = selected_.key;
    request.asset_id = asset_id;
    request.side = QStringLiteral("BUY");
    request.order_type = QStringLiteral("LIMIT");
    request.price = p;
    request.size = n;
    request.client_order_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.extras.insert(QStringLiteral("post_only"), true);
    request.extras.insert(QStringLiteral("reduce_only"), false);
    request.extras.insert(QStringLiteral("cancel_order_on_pause"), false);
    pending_order_kind_ = QStringLiteral("manual");
    pending_manual_order_ = QJsonObject{
        {QStringLiteral("event"), QStringLiteral("kalshi_manual_order_requested")},
        {QStringLiteral("requested_ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("account_mode"), creds->use_demo ? QStringLiteral("demo") : QStringLiteral("live")},
        {QStringLiteral("client_order_id"), request.client_order_id},
        {QStringLiteral("market_id"), selected_.key.market_id},
        {QStringLiteral("asset_id"), asset_id},
        {QStringLiteral("outcome"), side_},
        {QStringLiteral("action"), QStringLiteral("BUY")},
        {QStringLiteral("order_type"), QStringLiteral("LIMIT")},
        {QStringLiteral("time_in_force"), QStringLiteral("GTC")},
        {QStringLiteral("post_only"), true},
        {QStringLiteral("contracts"), n},
        {QStringLiteral("limit_price"), p},
        {QStringLiteral("notional"), p * n},
        {QStringLiteral("estimated_max_fee"), fee},
        {QStringLiteral("maximum_loss"), p * n + fee},
        {QStringLiteral("payout_if_correct"), double(n)}};
    kalshi_data::KalshiEvidenceEngine::append_jsonl(
        evidence_path(QStringLiteral("kalshi-trade-ledger.jsonl")), pending_manual_order_);
    if (live_order_button_) {
        live_order_button_->setEnabled(false);
        live_order_button_->setText(QStringLiteral("SUBMITTING…"));
    }
    active_adapter->place_order(request);
}

QString KalshiScreen::cli_path() const {
    using namespace openmarketterminal::screens::kalshi;
    const QString beside = find_cli_beside_app(
        QCoreApplication::applicationDirPath(), cli_exe_suffix());
    if (!beside.isEmpty()) return beside;
    // PATH fallbacks — findExecutable appends the platform suffix itself.
    const QStringList fallbacks{
        QStandardPaths::findExecutable(QStringLiteral("openterminalcli")),
        QStandardPaths::findExecutable(QStringLiteral("ot"))};
    for (const QString& path : fallbacks)
        if (!path.isEmpty() && QFileInfo(path).isExecutable()) return path;
    return {};
}

void KalshiScreen::run_live_cli(
    const QStringList& args,
    const std::function<void(const QJsonObject&, const QString&)>& done) {
    const QString cli = cli_path();
    if (cli.isEmpty()) {
        done({}, QStringLiteral("openterminalcli was not found beside the app"));
        return;
    }
    auto* process = new QProcess(this);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [process, done](int code, QProcess::ExitStatus status) {
                const QByteArray output = process->readAllStandardOutput();
                const QString error = QString::fromUtf8(process->readAllStandardError()).trimmed();
                process->deleteLater();
                const QJsonDocument document = QJsonDocument::fromJson(output);
                if (status != QProcess::NormalExit || code != 0 || !document.isObject()) {
                    done({}, error.isEmpty() ? QStringLiteral("live evidence command failed") : error);
                    return;
                }
                done(document.object(), {});
            });
    QStringList full{QStringLiteral("--json"), QStringLiteral("--profile"),
                     openmarketterminal::ProfileManager::instance().active()};
    full << args;
    process->start(cli, full);
}

void KalshiScreen::show_live_automation_dialog() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Arm bounded Kalshi automation"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* text = new QLabel(QStringLiteral(
        "Choose how long the bot may place real Kalshi orders without individual approval.\n\n"
        "CURRENT HOUR starts immediately and stops at the next :00 clock boundary. "
        "Hard limits: no more than 10 submitted orders in any rolling hour; contract stake <= $2; "
        "fees may bring all-in cost up to $3; total experiment exposure <= $120; one bot order per contract. "
        "Every submission re-checks quote freshness, executable depth, edge after cost, time left, credentials, "
        "session expiry, and the global kill switch. An emergency kill switch must be reset explicitly in "
        "Settings > Security before a new session can be armed."), &dialog);
    text->setWordWrap(true);
    layout->addWidget(text);
    auto* parallel_paper = new QCheckBox(
        QStringLiteral("Run unlimited paper trades in parallel (recommended)"), &dialog);
    parallel_paper->setChecked(true);
    parallel_paper->setToolTip(QStringLiteral(
        "Paper uses the same timestamped candidate, evidence gate, $2 sizing and exit rules as LIVE. "
        "It has no hourly quota and never submits an order."));
    layout->addWidget(parallel_paper);
    auto* paper_note = new QLabel(QStringLiteral(
        "PAPER creates one immutable sample per strategy book and contract. It does not loosen the decision gate; "
        "only the executor and real-money limits differ."), &dialog);
    paper_note->setWordWrap(true);
    paper_note->setStyleSheet(QStringLiteral("color:%1;").arg(colors::TEXT_SECONDARY()));
    layout->addWidget(paper_note);
    auto* choices = new QHBoxLayout;
    for (const QString& duration : {QStringLiteral("1H"), QStringLiteral("6H"),
                                    QStringLiteral("12H"), QStringLiteral("24/7")}) {
        const QString button_text = duration == QStringLiteral("1H")
            ? QStringLiteral("ARM CURRENT HOUR")
            : QStringLiteral("ARM %1").arg(duration);
        auto* button = new QPushButton(button_text, &dialog);
        if (duration == QStringLiteral("1H"))
            button->setToolTip(QStringLiteral("Start now and stop at the next :00 clock boundary."));
        button->setStyleSheet(QStringLiteral("padding:9px 16px;font-weight:900;"));
        connect(button, &QPushButton::clicked, &dialog, [this, &dialog, duration, parallel_paper]() {
            const auto killed = openmarketterminal::SettingsRepository::instance().get(
                QStringLiteral("cli.kill_switch"), QStringLiteral("false"));
            if (killed.is_ok() && killed.value().trimmed().toLower() == QStringLiteral("true")) {
                QMessageBox::warning(this, QStringLiteral("Kill switch engaged"),
                    QStringLiteral("Reset the global kill switch explicitly in Settings > Security before arming."));
                return;
            }
            run_live_cli({QStringLiteral("kalshi"), QStringLiteral("auto"), QStringLiteral("live"),
                          QStringLiteral("session"), duration.toLower(),
                          parallel_paper->isChecked() ? QStringLiteral("--paper") : QStringLiteral("--no-paper")},
                         [this](const QJsonObject&, const QString& error) {
                             if (!error.isEmpty()) QMessageBox::warning(this, QStringLiteral("Session failed"), error);
                             refresh_live_automation_status();
                         });
            dialog.accept();
        });
        choices->addWidget(button);
    }
    layout->addLayout(choices);
    auto* cancel = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    connect(cancel, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(cancel);
    dialog.exec();
}

void KalshiScreen::kill_live_automation() {
    if (QMessageBox::warning(this, QStringLiteral("Kill automated trading"),
                             QStringLiteral("Engage the global trading kill switch and stop this Kalshi session now?"),
                             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    openmarketterminal::SettingsRepository::instance().set(
        QStringLiteral("cli.kill_switch"), QStringLiteral("true"), QStringLiteral("cli"));
    openmarketterminal::SettingsRepository::instance().set(
        QStringLiteral("cli.kill_switch_latched"), QStringLiteral("true"), QStringLiteral("cli"));
    openmarketterminal::EventBus::instance().publish(
        QStringLiteral("settings.changed"), QVariantMap{{QStringLiteral("key"),
                                                          QStringLiteral("cli.kill_switch")}});
    run_live_cli({QStringLiteral("kalshi"), QStringLiteral("auto"), QStringLiteral("live"),
                  QStringLiteral("session"), QStringLiteral("stop")},
                 [this](const QJsonObject&, const QString&) { refresh_live_automation_status(); });
}

void KalshiScreen::refresh_live_automation_status() {
    if (!live_automation_status_ || live_status_fetching_) return;
    live_status_fetching_ = true;
    run_live_cli({QStringLiteral("kalshi"), QStringLiteral("auto"), QStringLiteral("live"),
                  QStringLiteral("status")},
                 [this](const QJsonObject& status, const QString& error) {
        live_status_fetching_ = false;
        if (!error.isEmpty()) {
            live_automation_status_->setText(QStringLiteral("LIVE EVIDENCE: unavailable"));
            return;
        }
        const bool active = status.value(QStringLiteral("session_active")).toBool();
        latest_legacy_live_status_ = status;
        const bool killed = status.value(QStringLiteral("kill_switch")).toBool();
        const bool autonomous = status.value(QStringLiteral("autonomous")).toBool();
        const bool paper = status.value(QStringLiteral("parallel_paper_enabled")).toBool();
        const bool engine_operational = status.value(
            QStringLiteral("decision_engine_operational")).toBool(!active);
        const QString engine_fault = status.value(QStringLiteral("decision_engine_fault")).toString();
        if (live_automation_button_) {
            const QJsonObject session = status.value(QStringLiteral("session")).toObject();
            const QString session_duration = session.value(QStringLiteral("duration")).toString();
            QString armed_text = QStringLiteral("ARMED");
            const QDateTime ends_at = QDateTime::fromString(
                session.value(QStringLiteral("ends_at")).toString(), Qt::ISODateWithMs);
            if (active && ends_at.isValid()) {
                armed_text += QStringLiteral(" · %1").arg(
                    duration_text(qMax<qint64>(0, QDateTime::currentDateTimeUtc().secsTo(ends_at.toUTC()))));
            } else if (active && session_duration == QStringLiteral("24/7")) {
                armed_text += QStringLiteral(" · 24/7");
            }
            if (active && !engine_operational)
                armed_text += QStringLiteral(" · ENGINE RECOVERING");
            live_automation_button_->setText(active ? armed_text : QStringLiteral("ARM AUTOMATION"));
            live_automation_button_->setEnabled(!active);
            live_automation_button_->setStyleSheet(QStringLiteral(
                "QPushButton{background:%1;color:%2;border:1px solid %1;padding:7px 12px;font-weight:900;}"
                "QPushButton:disabled{background:%1;color:%2;border:1px solid %1;padding:7px 12px;font-weight:900;}")
                .arg(active && !engine_operational ? colors::RED()
                                                   : active ? colors::GREEN()
                                                            : killed ? colors::RED() : colors::CYAN(),
                     colors::BG_BASE()));
            if (active) {
                live_automation_button_->setToolTip(!engine_operational
                    ? QStringLiteral("Session is armed but the decision engine is recovering: %1")
                          .arg(engine_fault.isEmpty() ? QStringLiteral("event stream unavailable")
                                                      : engine_fault)
                    : QStringLiteral(
                          "Automated Kalshi trading is armed for %1. Use KILL AUTOMATED TRADING to stop it.")
                          .arg(session_duration.isEmpty() ? QStringLiteral("THIS SESSION")
                                                          : session_duration.toUpper()));
            } else if (killed) {
                live_automation_button_->setToolTip(QStringLiteral(
                    "The global trading kill switch is engaged. Reset it explicitly in Settings > Security before arming."));
            } else {
                live_automation_button_->setToolTip(QStringLiteral(
                    "Arm bounded autonomous Kalshi execution for 1H, 6H, 12H, or 24/7."));
            }
        }
        live_automation_status_->setText(QStringLiteral(
            "%1 · %2/10 LIVE orders this rolling hour · $%3 of $120 used · stake <=$2 + fee, all-in <=$3%4%5")
            .arg(active ? (autonomous ? QStringLiteral("AUTO ACTIVE")
                                      : QStringLiteral("REVIEW SESSION ACTIVE"))
                        : QStringLiteral("SESSION STOPPED"))
            .arg(status.value(QStringLiteral("orders_last_hour")).toInt())
            .arg(status.value(QStringLiteral("worst_case_exposure_used")).toDouble(), 0, 'f', 2)
            .arg(paper ? QStringLiteral(" · PAPER ON: %1 open / %2 closed, no hourly limit")
                             .arg(status.value(QStringLiteral("paper_open_positions")).toInt())
                             .arg(status.value(QStringLiteral("paper_closed_positions")).toInt())
                       : QStringLiteral(" · PAPER OFF"))
            .arg(killed ? QStringLiteral(" · KILL SWITCH ON")
                        : active && !engine_operational
                            ? QStringLiteral(" · ENGINE OFFLINE: %1").arg(
                                  engine_fault.isEmpty() ? QStringLiteral("restart daemon") : engine_fault)
                            : QString()));
        live_automation_status_->setStyleSheet(QStringLiteral("color:%1;font-weight:800;")
            .arg(killed || (active && !engine_operational)
                     ? colors::RED() : active ? colors::GREEN() : colors::TEXT_SECONDARY()));
    });
}

void KalshiScreen::refresh_advisor_canary_status() {
    if (!legacy_live_badge_ || !canary_badge_ || !advisor_system_status_ ||
        !advisor_qualification_status_ || !advisor_safety_status_ || !advisor_activity_status_)
        return;
    const QString root = ProfileManager::instance().profile_root() + QStringLiteral("/daemon/");
    const QJsonObject loop = read_json_object(root + QStringLiteral("advisor_loop_state.json"));
    const QJsonObject qualification = read_json_object(root + QStringLiteral("advisor_qualification_report.json"));
    const QJsonObject promotion = read_json_object(root + QStringLiteral("advisor_promotion_state.json"));
    const QJsonObject safety = read_json_object(root + QStringLiteral("advisor_safety_state.json"));
    const QJsonObject canary = read_json_object(root + QStringLiteral("advisor_canary_config.json"));
    const QJsonObject latest = read_last_jsonl_object(root + QStringLiteral("advisor_opportunities.jsonl"));
    const AdvisorCanaryView view = present_advisor_canary(loop, qualification, promotion, safety,
        canary, latest, latest_legacy_live_status_, QDateTime::currentMSecsSinceEpoch());

    legacy_live_badge_->setText(view.legacy_badge);
    legacy_live_badge_->setStyleSheet(QStringLiteral(
        "color:%1;background:%2;border:2px solid %1;padding:9px;font-weight:900;")
        .arg(view.legacy_live ? colors::RED() : latest_legacy_live_status_.isEmpty()
                  ? colors::WARNING() : colors::TEXT_SECONDARY(), colors::BG_RAISED()));
    canary_badge_->setText(view.canary_badge);
    canary_badge_->setStyleSheet(QStringLiteral(
        "color:%1;background:%2;border:2px solid %1;padding:9px;font-weight:900;")
        .arg(view.canary_live ? colors::RED() : view.canary_badge.contains(QStringLiteral("UNKNOWN"))
                  ? colors::WARNING() : colors::CYAN(), colors::BG_RAISED()));
    if (advisor_separation_status_) {
        advisor_separation_status_->setText(view.legacy_badge + QStringLiteral("  |  ") + view.canary_badge);
        advisor_separation_status_->setStyleSheet(QStringLiteral(
            "color:%1;background:%2;border:2px solid %1;padding:8px;font-weight:900;")
            .arg(view.legacy_live || view.canary_live ? colors::RED() : view.critical
                      ? colors::WARNING() : colors::CYAN(), colors::BG_RAISED()));
    }
    advisor_system_status_->setText(view.system);
    advisor_qualification_status_->setText(view.qualification);
    advisor_safety_status_->setText(view.safety);
    advisor_activity_status_->setText(view.activity);
    advisor_system_status_->setStyleSheet(QStringLiteral(
        "color:%1;background:%2;border:1px solid %3;padding:9px;font-weight:700;")
        .arg(view.critical ? colors::WARNING() : colors::GREEN(), colors::BG_RAISED(),
             view.critical ? colors::WARNING() : colors::BORDER_DIM()));
}

void KalshiScreen::refresh_daemon_status() {
    if (!daemon_badge_ || !daemon_restart_button_ || daemon_status_fetching_ || daemon_restarting_)
        return;
    // Evidence freshness is judged in-process: a missing CLI binary must never
    // misreport a daemon that is demonstrably writing evidence as OFF.
    namespace cli = openmarketterminal::cli;
    QJsonObject books;
    QFile books_file(cli::kalshi_evidence_path(QStringLiteral("kalshi-ws-books.json")));
    if (books_file.open(QIODevice::ReadOnly | QIODevice::Text))
        books = QJsonDocument::fromJson(books_file.readAll()).object();
    const auto evidence = cli::kalshi_daemon_evidence_freshness(
        cli::kalshi_ws_books_heartbeat_ms(books), QDateTime::currentMSecsSinceEpoch());
    const auto set_badge = [this](const QString& text, const QString& color, const QString& tip) {
        daemon_badge_->setText(text);
        daemon_badge_->setStyleSheet(
            QStringLiteral("color:%1;font-weight:900;padding:6px;").arg(color));
        daemon_badge_->setToolTip(tip);
    };
    const QString age_text = evidence.age_ms >= 120'000
        ? QStringLiteral("%1m").arg(evidence.age_ms / 60'000)
        : QStringLiteral("%1s").arg(evidence.age_ms / 1'000);
    if (evidence.state == QStringLiteral("fresh")) {
        set_badge(QStringLiteral("DAEMON: ACTIVE"), colors::GREEN(), QStringLiteral(
            "Local daemon is active: kalshi-ws-books.json heartbeat %1 ago "
            "(source: in-process evidence freshness). It owns Kalshi WebSocket "
            "monitoring, paper evidence, and armed automation.").arg(age_text));
        daemon_restart_button_->setEnabled(true);
        return;
    }
    if (evidence.state == QStringLiteral("stale"))
        set_badge(QStringLiteral("DAEMON: STALE"), colors::WARNING(), QStringLiteral(
            "Daemon evidence is stale: last kalshi-ws-books.json heartbeat %1 ago "
            "(source: in-process evidence freshness). The daemon may be down or its "
            "WebSocket feed idle.").arg(age_text));
    else
        set_badge(QStringLiteral("DAEMON: NO EVIDENCE"), colors::TEXT_SECONDARY(), QStringLiteral(
            "No daemon evidence found: kalshi-ws-books.json has no heartbeat "
            "(source: in-process evidence freshness). Start or restart the daemon "
            "to begin monitoring."));
    daemon_restart_button_->setEnabled(true);
    // CLI refinement: a daemon can be alive without a WebSocket heartbeat (for
    // example, no Kalshi credentials). A definitive CLI answer upgrades or
    // settles the badge; a missing/failing CLI leaves the evidence verdict.
    daemon_status_fetching_ = true;
    run_live_cli({QStringLiteral("daemon"), QStringLiteral("status")},
                 [this, set_badge](const QJsonObject& status, const QString& error) {
        daemon_status_fetching_ = false;
        daemon_restart_button_->setEnabled(true);
        if (!error.isEmpty()) return;
        const bool running = status.value(QStringLiteral("scheduler_running")).toBool() ||
                             status.value(QStringLiteral("running")).toBool();
        if (running) {
            const qint64 pid = static_cast<qint64>(
                status.value(QStringLiteral("daemon_process_pid")).toDouble(
                    status.value(QStringLiteral("pid")).toDouble()));
            set_badge(QStringLiteral("DAEMON: ACTIVE"), colors::GREEN(), QStringLiteral(
                "Local daemon is active%1 (source: openterminalcli daemon status). "
                "It owns Kalshi WebSocket monitoring, paper evidence, and armed automation.")
                .arg(pid > 0 ? QStringLiteral(" (PID %1)").arg(pid) : QString()));
        } else {
            set_badge(QStringLiteral("DAEMON: OFF"), colors::TEXT_SECONDARY(), QStringLiteral(
                "The local daemon is not running (source: openterminalcli daemon status)."));
        }
    });
}

void KalshiScreen::restart_daemon() {
    if (!daemon_badge_ || !daemon_restart_button_ || daemon_restarting_) return;
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("Restart local daemon"),
        QStringLiteral(
            "Restart the local OpenTerminal daemon now?\n\n"
            "Market monitoring pauses briefly. If a bounded LIVE automation session is armed, "
            "it remains armed and may resume submitting approved-size orders after the daemon reconnects."),
        QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;

    daemon_restarting_ = true;
    daemon_restart_button_->setEnabled(false);
    daemon_restart_button_->setText(QStringLiteral("RESTARTING..."));
    daemon_badge_->setText(QStringLiteral("DAEMON: RESTARTING"));
    daemon_badge_->setStyleSheet(QStringLiteral("color:%1;font-weight:900;padding:6px;")
                                     .arg(colors::WARNING()));
    run_live_cli({QStringLiteral("daemon"), QStringLiteral("restart")},
                 [this](const QJsonObject&, const QString& error) {
        daemon_restarting_ = false;
        daemon_restart_button_->setText(QStringLiteral("RESTART"));
        daemon_restart_button_->setEnabled(true);
        if (!error.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Daemon restart failed"), error);
            // A failed restart command is not proof the daemon is off; let the
            // in-process evidence check re-derive the badge.
            refresh_daemon_status();
            return;
        }
        QTimer::singleShot(1'000, this, &KalshiScreen::refresh_daemon_status);
    });
}

void KalshiScreen::toggle_shadow_collector() {
    shadow_enabled_ = !shadow_enabled_;
    shadow_button_->setText(shadow_enabled_ ? QStringLiteral("PAUSE SHADOW") : QStringLiteral("RESUME SHADOW"));
    shadow_status_->setText(shadow_enabled_
        ? QStringLiteral("BOT SHADOW ACTIVE\n%1 auto-plan records · %2 maker candidates · %3 confirmed trade-throughs\nCounterfactual evidence only. No orders are submitted.")
              .arg(auto_shadow_records_).arg(shadow_candidates_).arg(shadow_confirmed_)
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
    shadow_status_->setText(QStringLiteral("BOT SHADOW ACTIVE\n%1 auto-plan records · %2 maker candidates · %3 confirmed trade-throughs\nCounterfactual evidence only. No orders are submitted.")
                                .arg(auto_shadow_records_).arg(shadow_candidates_).arg(shadow_confirmed_));
}

void KalshiScreen::append_shadow_event(const QString& event, const QString& asset_id,
                                       double quote_price, double queue, const QString& confirmation) {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QJsonObject row{{QStringLiteral("event"), event},
                    {QStringLiteral("schema_version"), 2},
                    {QStringLiteral("execution_style"), QStringLiteral("shadow_maker")},
                    {QStringLiteral("asset_id"), asset_id},
                    {QStringLiteral("quote_price"), quote_price},
                    {QStringLiteral("initial_queue"), queue},
                    {QStringLiteral("ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                    {QStringLiteral("live_eligible"), false}};
    if (!confirmation.isEmpty()) row.insert(QStringLiteral("confirmation"), confirmation);
    kalshi_data::KalshiEvidenceEngine::append_jsonl(
        directory + QStringLiteral("/kalshi-shadow-maker.jsonl"), row);
}

void KalshiScreen::set_spot_symbol(const QString& asset) {
    spot_symbol_ = asset.trimmed().toUpper() + QStringLiteral("/USD");
    if (dom_title_) dom_title_->setText(
        QStringLiteral("%1 DOM · %2 · 24 L2").arg(reference_dom_venue_.toUpper(), spot_symbol_));
    if (spot_dom_) spot_dom_->clear();
    if (reference_chart_) {
        reference_chart_->clear();
        reference_chart_->set_probability_mode(false);
        reference_chart_->set_symbol(spot_symbol_);
    }
    if (contract_chart_) contract_chart_->clear();
    if (chart_status_) { chart_status_->setText(QStringLiteral("LOADING KALSHI CONTRACT HISTORY")); chart_status_->show(); }
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
    reference_spot_history_.clear();
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
            // Advanced→legacy fallback: a coinbase connection that dropped
            // without ever delivering a book retries on the legacy Exchange
            // feed (while it still answers) instead of looping on Advanced.
            if (reference_dom_venue_ == QStringLiteral("coinbase") &&
                !reference_dom_use_legacy_coinbase_ &&
                reference_dom_bids_.isEmpty() && reference_dom_asks_.isEmpty()) {
                reference_dom_use_legacy_coinbase_ = true;
            }
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
    const QUrl endpoint(
        reference_dom_venue_ == QStringLiteral("coinbase")
            ? QString::fromLatin1(reference_dom_use_legacy_coinbase_
                                      ? openmarketterminal::services::crypto::kLegacyExchangeWsUrl
                                      : openmarketterminal::services::crypto::kAdvancedTradeWsUrl)
            : QStringLiteral("wss://ws.kraken.com/v2"));
    reference_dom_socket_->open(endpoint);
}

void KalshiScreen::subscribe_reference_dom() {
    if (!reference_dom_socket_ || reference_dom_socket_->state() != QAbstractSocket::ConnectedState) return;
    QJsonObject request;
    if (reference_dom_venue_ == QStringLiteral("coinbase")) {
        const QString product = QString(reference_dom_symbol_).replace(QLatin1Char('/'), QLatin1Char('-'));
        if (!reference_dom_use_legacy_coinbase_) {
            // Advanced Trade: one channel per subscribe frame (single
            // "channel" string); heartbeats keeps quiet books alive.
            for (const QString& channel : {QStringLiteral("level2"), QStringLiteral("heartbeats")}) {
                const QJsonObject frame =
                    openmarketterminal::services::crypto::advanced_ws_subscribe({product}, channel);
                reference_dom_socket_->sendTextMessage(QJsonDocument(frame).toJson(QJsonDocument::Compact));
            }
            if (dom_status_) {
                dom_status_->setText(QStringLiteral("%1 WS CONNECTED · AWAITING BOOK")
                                         .arg(reference_dom_venue_.toUpper()));
                dom_status_->setStyleSheet(QStringLiteral("color:%1;font-size:10px;font-weight:800;")
                                               .arg(colors::WARNING()));
            }
            return;
        }
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
        // Advanced Trade level2 envelope: {"channel":"l2_data","events":[{"type":
        // "snapshot"|"update","product_id":..,"updates":[{"side":"bid"|"offer",
        // "price_level":"..","new_quantity":".."}]}]} — quantity 0 removes a level.
        if (root.value(QStringLiteral("channel")).toString() == QStringLiteral("l2_data")) {
            bool touched = false;
            for (const auto& ev : root.value(QStringLiteral("events")).toArray()) {
                const QJsonObject event = ev.toObject();
                if (event.value(QStringLiteral("product_id")).toString() != product) continue;
                if (event.value(QStringLiteral("type")).toString() == QStringLiteral("snapshot")) {
                    reference_dom_bids_.clear();
                    reference_dom_asks_.clear();
                }
                for (const auto& uv : event.value(QStringLiteral("updates")).toArray()) {
                    const QJsonObject u = uv.toObject();
                    QMap<double, double>& side = u.value(QStringLiteral("side")).toString() == QStringLiteral("bid")
                        ? reference_dom_bids_ : reference_dom_asks_;
                    const double price = u.value(QStringLiteral("price_level")).toVariant().toDouble();
                    const double qty = u.value(QStringLiteral("new_quantity")).toVariant().toDouble();
                    if (price <= 0.0) continue;
                    if (qty <= 0.0) side.remove(price); else side.insert(price, qty);
                    touched = true;
                }
            }
            if (!touched) return;
        } else if (root.value(QStringLiteral("channel")).toString() == QStringLiteral("heartbeats") ||
                   root.value(QStringLiteral("channel")).toString() == QStringLiteral("subscriptions")) {
            return; // keep-alive / ack frames carry no book data
        } else {
        // Legacy Exchange feed shapes (fallback path) below.
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
        } // legacy Exchange-feed branch
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
        openmarketterminal::trading::Candle spot_tick;
        spot_tick.timestamp = tick_now;
        spot_tick.open = spot_tick.high = spot_tick.low = spot_tick.close = midpoint;
        reference_spot_history_.append(spot_tick);
        if (reference_spot_history_.size() > 2'400)
            reference_spot_history_.remove(0, reference_spot_history_.size() - 2'400);
        if (reference_chart_ && chart_timeframe_ == QStringLiteral("live"))
            reference_chart_->set_candles(reference_spot_history_);
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
                openmarketterminal::trading::Candle spot_tick;
                spot_tick.timestamp = now;
                spot_tick.open = spot_tick.high = spot_tick.low = spot_tick.close = mean_mid;
                self->reference_spot_history_.append(spot_tick);
                if (self->reference_spot_history_.size() > 2'400)
                    self->reference_spot_history_.remove(
                        0, self->reference_spot_history_.size() - 2'400);
                if (self->reference_chart_ && self->chart_timeframe_ == QStringLiteral("live"))
                    self->reference_chart_->set_candles(self->reference_spot_history_);
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
    if (contract_chart_) contract_chart_->set_timeframe(chart_timeframe_);
    refresh_spot_history();
    if (chart_status_) {
        chart_status_->setText(QStringLiteral("FETCHING %1 KALSHI CONTRACT HISTORY")
                                   .arg(chart_timeframe_.toUpper()));
        chart_status_->show();
    }
    refresh_reference_chart();
}

void KalshiScreen::refresh_spot_history() {
    if (!reference_chart_) return;
    if (chart_timeframe_ == QStringLiteral("live")) {
        reference_chart_->set_candles(reference_spot_history_);
        return;
    }
    if (spot_chart_fetching_.exchange(true)) return;

    const QString timeframe = chart_timeframe_;
    const QString symbol = spot_symbol_;
    const QString venue = reference_dom_venue_;
    const int limit = timeframe == QStringLiteral("1d") ? 120
        : timeframe == QStringLiteral("1h") ? 240 : 300;
    auto* session = openmarketterminal::trading::ExchangeSessionManager::instance().session(venue);
    QPointer<KalshiScreen> self(this);
    (void)QtConcurrent::run([self, session, symbol, timeframe, limit]() {
        QVector<openmarketterminal::trading::Candle> candles;
        if (session) {
            try {
                candles = session->fetch_ohlcv(symbol, timeframe, limit);
                if (candles.isEmpty() && symbol.endsWith(QStringLiteral("/USD")))
                    candles = session->fetch_ohlcv(symbol + QStringLiteral("T"), timeframe, limit);
            } catch (...) {
                // The live DOM continues to work if this optional public-history request fails.
            }
        }
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, symbol, timeframe, candles]() {
            if (!self) return;
            self->spot_chart_fetching_ = false;
            if (self->spot_symbol_ != symbol || self->chart_timeframe_ != timeframe) {
                self->refresh_spot_history();
                return;
            }
            if (!candles.isEmpty()) self->reference_chart_->set_candles(candles);
        }, Qt::QueuedConnection);
    });
}

void KalshiScreen::refresh_reference_chart() {
    if (!contract_chart_ || !has_selection_ || chart_asset_id_.isEmpty() ||
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
    reference_chart_->set_targets(has_selection_ ? contract_bounds(selected_) : QVector<double>{});
}

void KalshiScreen::update_observation_strip() {
    if (!close_countdown_) return;
    const QDateTime close = has_selection_ ? QDateTime::fromString(selected_.end_date_iso, Qt::ISODate) : QDateTime{};
    const qint64 seconds = close.isValid()
        ? QDateTime::currentDateTimeUtc().secsTo(close.toUTC()) : -1;
    close_countdown_->setText(seconds >= 0
        ? QStringLiteral("CLOSE IN %1").arg(duration_text(seconds))
        : QStringLiteral("CLOSE —"));
    close_countdown_->setToolTip(close.isValid()
        ? QStringLiteral("Closes at %1").arg(close.toLocalTime().toString(QStringLiteral("h:mm ap")))
        : QStringLiteral("Close time unavailable for this contract."));
}

void KalshiScreen::update_calibrator_readout() {
    if (!calibrator_readout_) return;
    if (!has_selection_) {
        calibrator_readout_->setText(QStringLiteral("CALIBRATOR · select a contract"));
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - calibrator_report_read_ms_ >= 5'000) {
        calibrator_report_read_ms_ = now;
        calibrator_report_ = QJsonObject();
        QFile file(cli::kalshi_evidence_path(QStringLiteral("calibrator.json")));
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
            if (document.isObject()) calibrator_report_ = document.object();
        }
    }
    const QJsonObject readout = kalshi_data::KalshiEvidenceEngine::calibrator_readout(
        calibrator_report_, selected_.key.market_id, now);
    const QString record = readout.value(QStringLiteral("record")).toString();
    calibrator_readout_->setText(record.isEmpty()
        ? readout.value(QStringLiteral("headline")).toString()
        : readout.value(QStringLiteral("headline")).toString() + QLatin1Char('\n') + record);
    const QString state = readout.value(QStringLiteral("state")).toString();
    // Deliberately muted styling either way: a calibrated probability is
    // evidence with a stated record, never an implied edge.
    const QString color = state == QStringLiteral("ok") ? colors::TEXT_SECONDARY()
                                                        : colors::WARNING();
    calibrator_readout_->setStyleSheet(QStringLiteral(
        "color:%1;background:%2;border:1px solid %3;padding:4px 8px;font-size:10px;font-weight:800;")
                                           .arg(color, colors::BG_RAISED(), colors::BORDER_DIM()));
}

} // namespace openmarketterminal::screens::kalshi
