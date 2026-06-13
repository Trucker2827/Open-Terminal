#include "RateLimiter.h"

namespace openmarketterminal::trading {

OrderRateLimiter& OrderRateLimiter::instance()
{
    static OrderRateLimiter limiter;
    return limiter;
}

OrderRateLimiter::OrderRateLimiter()
{
    set_limit(BrokerId::Alpaca, 200);
    set_limit(BrokerId::IBKR, 50);
    set_limit(BrokerId::Tradier, 10);
    set_limit(BrokerId::SaxoBank, 10);
    set_limit(BrokerId::MetaTrader4, 10);
}

OrderRateLimiter::BrokerLimit& OrderRateLimiter::get_or_create(BrokerId broker)
{
    QMutexLocker locker(&registry_mutex_);
    int key = static_cast<int>(broker);
    auto it = limits_.find(key);
    if (it == limits_.end())
        it = limits_.emplace(key, std::make_unique<BrokerLimit>()).first;
    return *it->second;
}

void OrderRateLimiter::acquire(BrokerId broker)
{
    auto& limit = get_or_create(broker);
    QMutexLocker locker(&limit.mutex);

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 min_interval_ms = 1000 / limit.orders_per_second;
    qint64 elapsed = now - limit.last_order_ms;

    if (elapsed < min_interval_ms) {
        qint64 wait = min_interval_ms - elapsed;
        locker.unlock();
        QThread::msleep(static_cast<unsigned long>(wait));
        locker.relock();
    }

    limit.last_order_ms = QDateTime::currentMSecsSinceEpoch();
}

void OrderRateLimiter::set_limit(BrokerId broker, int orders_per_second)
{
    auto& limit = get_or_create(broker);
    QMutexLocker locker(&limit.mutex);
    limit.orders_per_second = (orders_per_second > 0) ? orders_per_second : DEFAULT_LIMIT;
}

int OrderRateLimiter::get_limit(BrokerId broker) const
{
    QMutexLocker locker(&registry_mutex_);
    int key = static_cast<int>(broker);
    auto it = limits_.find(key);
    if (it == limits_.end()) return DEFAULT_LIMIT;
    return it->second->orders_per_second;
}

} // namespace openmarketterminal::trading
