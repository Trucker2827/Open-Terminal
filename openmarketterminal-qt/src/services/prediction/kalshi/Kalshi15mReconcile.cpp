#include "services/prediction/kalshi/Kalshi15mReconcile.h"
#include <QLoggingCategory>

namespace kalshi15m {

QStringList desired_subscriptions(const QVector<pred::PredictionMarket>& markets,
                                  const QStringList& families, int cap) {
    QStringList out;
    QSet<QString> seen;
    for (const auto& m : markets) {
        const QString id = m.key.market_id;
        const int dash = id.indexOf('-');
        const QString family = dash < 0 ? id : id.left(dash);
        if (!families.contains(family) || seen.contains(id) || id.isEmpty())
            continue;
        seen.insert(id);
        out.append(id);
    }
    if (cap > 0 && out.size() > cap) {
        qWarning("kalshi15m: %d open markets exceeds cap %d; capping",
                 out.size(), cap);
        out = out.mid(0, cap);
    }
    return out;
}

Delta reconcile(const QStringList& desired, const QSet<QString>& held) {
    Delta d;
    const QSet<QString> desired_set(desired.begin(), desired.end());
    for (const QString& t : desired)
        if (!held.contains(t)) d.to_subscribe.append(t);
    for (const QString& t : held)
        if (!desired_set.contains(t)) d.to_unsubscribe.append(t);
    d.to_unsubscribe.sort();   // deterministic for tests (held is unordered)
    return d;
}

}  // namespace kalshi15m
