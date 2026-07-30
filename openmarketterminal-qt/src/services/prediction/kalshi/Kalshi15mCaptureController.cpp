#include "services/prediction/kalshi/Kalshi15mCaptureController.h"
#include "services/prediction/kalshi/KalshiRestClient.h"
#include "services/prediction/kalshi/Kalshi15mReconcile.h"
#include "services/prediction/PredictionExchangeAdapter.h"

namespace pred = openmarketterminal::services::prediction;
namespace kalshi_ns = openmarketterminal::services::prediction::kalshi_ns;

namespace {
/// KalshiAdapter::subscribe_market/unsubscribe_market take asset_ids in
/// "<ticker>:yes|no" form (it splits on the last ':' to recover the ticker
/// for the WS channel — see KalshiAdapter::split_asset_id). The reconcile
/// policy in Kalshi15mReconcile operates on bare market_id tickers (no
/// colon), so we must re-attach a side suffix before calling the adapter or
/// split_asset_id silently drops the (colon-less) ticker.
QStringList to_asset_ids(const QStringList& tickers) {
    QStringList out;
    out.reserve(tickers.size());
    for (const QString& t : tickers) out.append(t + QStringLiteral(":yes"));
    return out;
}
}  // namespace

Kalshi15mCaptureController::Kalshi15mCaptureController(
    pred::PredictionExchangeAdapter* adapter, QObject* parent)
    : QObject(parent), adapter_(adapter),
      rest_(std::make_unique<kalshi_ns::KalshiRestClient>(this)) {
    connect(rest_.get(), &kalshi_ns::KalshiRestClient::markets_ready,
            this, &Kalshi15mCaptureController::on_markets_ready);
    connect(rest_.get(), &kalshi_ns::KalshiRestClient::request_error,
            this, &Kalshi15mCaptureController::on_request_error);
    poll_timer_.setInterval(poll_interval_ms_);
    connect(&poll_timer_, &QTimer::timeout, this, &Kalshi15mCaptureController::poll);
}

Kalshi15mCaptureController::~Kalshi15mCaptureController() = default;

void Kalshi15mCaptureController::start() { poll(); poll_timer_.start(); }
void Kalshi15mCaptureController::stop()  { poll_timer_.stop(); in_flight_ = false; }

void Kalshi15mCaptureController::poll() {
    if (in_flight_) return;   // a previous discovery cycle is still paginating
    in_flight_ = true;
    page_accum_.clear();
    // Discover open markets for the (single, MVP) configured 15-minute series.
    rest_->fetch_markets(QStringLiteral("open"), QString(), families_.first(),
                         QString(), 100, QString());
}

void Kalshi15mCaptureController::on_markets_ready(
    const QVector<pred::PredictionMarket>& markets, const QString& next_cursor) {
    page_accum_ += markets;
    if (!next_cursor.isEmpty()) {
        rest_->fetch_markets(QStringLiteral("open"), QString(), families_.first(),
                             QString(), 100, next_cursor);
        return;
    }
    in_flight_ = false;
    reconcile_and_apply();
}

void Kalshi15mCaptureController::on_request_error(const QString& context,
                                                 const QString& message) {
    // A failed discovery fetch: abandon this cycle without reconciling, so a
    // transient error never triggers spurious unsubscribes. Next timer tick retries.
    qWarning("kalshi15m: discovery fetch failed (%s): %s",
             qUtf8Printable(context), qUtf8Printable(message));
    in_flight_ = false;
    page_accum_.clear();
}

void Kalshi15mCaptureController::reconcile_and_apply() {
    const QStringList desired =
        kalshi15m::desired_subscriptions(page_accum_, families_, cap_);
    const kalshi15m::Delta d = kalshi15m::reconcile(desired, held_);
    // Re-assert the FULL desired set every cycle, not just the delta: the UI
    // shares one non-ref-counted WS subscription set, so if it unsubscribed a
    // ticker we still want, re-subscribing here re-adds it within one poll
    // (<=30s) instead of leaving a silent capture gap. This is NOT free at the
    // wire level: KalshiWsClient::subscribe re-sends a subscribe frame for the
    // whole list each call (the local set only dedupes membership), so this is
    // bounded periodic WS traffic (<= cap_ tickers / poll) that the canary
    // measures. Only tickers that have rolled off (held \ desired) are
    // unsubscribed. (d.to_subscribe is intentionally superseded by this full
    // re-assert; d.to_unsubscribe is still the correct roll-off set.)
    if (!desired.isEmpty())          adapter_->subscribe_market(to_asset_ids(desired));
    if (!d.to_unsubscribe.isEmpty()) adapter_->unsubscribe_market(to_asset_ids(d.to_unsubscribe));
    held_ = QSet<QString>(desired.begin(), desired.end());
}
