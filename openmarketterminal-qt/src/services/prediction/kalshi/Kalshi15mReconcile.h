#pragma once
#include <QStringList>
#include <QSet>
#include <QVector>
#include "services/prediction/PredictionTypes.h"

namespace kalshi15m {

namespace pred = openmarketterminal::services::prediction;

/// market_ids whose family (prefix before the first '-') is in `families`,
/// de-duplicated and order-stable, truncated to `cap` (qWarning if exceeded).
QStringList desired_subscriptions(const QVector<pred::PredictionMarket>& markets,
                                  const QStringList& families, int cap);

struct Delta { QStringList to_subscribe; QStringList to_unsubscribe; };

/// to_subscribe = desired \ held ; to_unsubscribe = held \ desired.
Delta reconcile(const QStringList& desired, const QSet<QString>& held);

}  // namespace kalshi15m
