#include "services/edge_radar/EdgeProofStats.h"

#include <QRegularExpression>

#include <algorithm>

namespace openmarketterminal::services::edge_radar {

double edge_proof_rate(int wins, int resolved) {
    return resolved > 0 ? static_cast<double>(wins) / static_cast<double>(resolved) : 0.0;
}

QString edge_proof_sample_status(int resolved) {
    if (resolved >= 1000)
        return QStringLiteral("institutional-sample");
    if (resolved >= 500)
        return QStringLiteral("strong-sample");
    if (resolved >= 100)
        return QStringLiteral("useful-sample");
    if (resolved >= 30)
        return QStringLiteral("early-sample");
    return QStringLiteral("warmup");
}

int edge_proof_next_milestone(int resolved) {
    if (resolved < 30)
        return 30;
    if (resolved < 100)
        return 100;
    if (resolved < 500)
        return 500;
    if (resolved < 1000)
        return 1000;
    return 0;
}

QString edge_proof_verdict(const EdgeProofStats& s) {
    const double buy_rate = edge_proof_rate(s.buy_wins, s.buy_resolved);
    const double no_trade_rate = edge_proof_rate(s.no_trade_correct, s.no_trade_resolved);
    if (s.resolved < 30)
        return QStringLiteral("WARMUP");
    if (s.buy_resolved >= 10 && s.paper_pnl > 0.0 && buy_rate >= 0.55)
        return QStringLiteral("BUY EDGE PROVING");
    if (s.buy_resolved >= 10 && (s.paper_pnl <= 0.0 || buy_rate < 0.50))
        return QStringLiteral("BUY WEAK");
    if (s.no_trade_resolved >= 20 && no_trade_rate >= 0.65 && s.buy_resolved < 5)
        return QStringLiteral("AVOID WEAK TRADES");
    if (s.no_trade_resolved >= 20 && no_trade_rate >= 0.65)
        return QStringLiteral("NO-TRADE EDGE");
    return QStringLiteral("MIXED");
}

bool edge_proof_is_buy_call(const QString& call, const QString& side) {
    Q_UNUSED(side);
    return call == QLatin1String("BUY CANDIDATE");
}

double edge_proof_accumulate(EdgeProofStats& s, bool buy_call,
                             const EdgeProofRowOutcome& row, double amount_usd) {
    ++s.signal_count;
    if (buy_call)
        ++s.buy_signals;
    else
        ++s.no_trade_signals;
    if (!row.resolved) {
        ++s.waiting;
        return 0.0;
    }
    ++s.resolved;
    if (row.outcome == 1)
        ++s.wins;
    double pnl = 0.0;
    if (buy_call) {
        ++s.buy_resolved;
        pnl = amount_usd * (row.move - row.breakeven);
        s.paper_pnl += pnl;
        if (pnl > 0.0)
            ++s.buy_wins;
    } else {
        ++s.no_trade_resolved;
        if (row.outcome == 1) {
            ++s.no_trade_correct;
            s.avoided_value += amount_usd * std::max(0.0, row.breakeven - row.move);
        }
    }
    return pnl;
}

bool edge_proof_parse_scored_move(const QString& reasons, double* move_out) {
    static const QRegularExpression pattern(
        QStringLiteral("scored:[^|]*\\bmove=(-?[0-9]+(?:\\.[0-9]+)?)"));
    QRegularExpressionMatch last;
    auto it = pattern.globalMatch(reasons);
    while (it.hasNext())
        last = it.next();
    if (!last.hasMatch())
        return false;
    bool ok = false;
    const double value = last.captured(1).toDouble(&ok);
    if (!ok)
        return false;
    if (move_out)
        *move_out = value;
    return true;
}

} // namespace openmarketterminal::services::edge_radar
