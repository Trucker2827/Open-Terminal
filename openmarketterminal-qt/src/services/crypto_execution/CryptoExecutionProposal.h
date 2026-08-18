#pragma once

// Canonical, immutable hand-off between crypto signal producers and the
// deterministic executor.  Producers may publish this object, but they never
// receive order-placement authority from it.  The executor must re-check every
// limit, expiry, balance, inventory and live gate at submission time.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace openmarketterminal::services::crypto_execution {

inline constexpr auto kProposalSchema = "crypto-execution-proposal-v1";

struct CryptoExecutionProposal {
    QString proposal_id;
    QString strategy_id;
    QString policy_hash;
    QString ledger;
    QString lane; // spot | scalp
    QString symbol;
    QString venue;
    QString side; // BUY | SELL
    QString liquidity;
    QString time_in_force;
    QString execution_authority{QStringLiteral("shadow_only")};
    qint64 created_at_ms = 0;
    qint64 expires_at_ms = 0;
    double limit_price = 0.0;
    double notional_usd = 0.0;
    double expected_move_bps = 0.0;
    double round_trip_cost_bps = 0.0;
    double net_edge_bps = 0.0;
    bool inventory_backed = false;
    bool executable = false;
    QStringList blockers;
};

inline QJsonObject proposal_json(const CryptoExecutionProposal& p) {
    return QJsonObject{
        {QStringLiteral("schema"), QString::fromLatin1(kProposalSchema)},
        {QStringLiteral("proposal_id"), p.proposal_id},
        {QStringLiteral("strategy_id"), p.strategy_id},
        {QStringLiteral("policy_hash"), p.policy_hash},
        {QStringLiteral("ledger"), p.ledger},
        {QStringLiteral("lane"), p.lane},
        {QStringLiteral("symbol"), p.symbol},
        {QStringLiteral("venue"), p.venue},
        {QStringLiteral("side"), p.side},
        {QStringLiteral("liquidity"), p.liquidity},
        {QStringLiteral("time_in_force"), p.time_in_force},
        {QStringLiteral("execution_authority"), p.execution_authority},
        {QStringLiteral("created_at_ms"), QString::number(p.created_at_ms)},
        {QStringLiteral("expires_at_ms"), QString::number(p.expires_at_ms)},
        {QStringLiteral("limit_price"), p.limit_price},
        {QStringLiteral("notional_usd"), p.notional_usd},
        {QStringLiteral("expected_move_bps"), p.expected_move_bps},
        {QStringLiteral("round_trip_cost_bps"), p.round_trip_cost_bps},
        {QStringLiteral("net_edge_bps"), p.net_edge_bps},
        {QStringLiteral("inventory_backed"), p.inventory_backed},
        {QStringLiteral("executable"), p.executable},
        {QStringLiteral("blockers"), QJsonArray::fromStringList(p.blockers)}};
}

inline QStringList validate_proposal(const CryptoExecutionProposal& p, qint64 now_ms) {
    QStringList errors;
    if (p.proposal_id.trimmed().isEmpty()) errors << QStringLiteral("MISSING_PROPOSAL_ID");
    if (p.strategy_id.trimmed().isEmpty()) errors << QStringLiteral("MISSING_STRATEGY_ID");
    if (p.policy_hash.trimmed().isEmpty()) errors << QStringLiteral("MISSING_POLICY_HASH");
    if (p.ledger.trimmed().isEmpty()) errors << QStringLiteral("MISSING_LEDGER");
    if (p.lane != QLatin1String("spot") && p.lane != QLatin1String("scalp"))
        errors << QStringLiteral("INVALID_LANE");
    if (p.symbol.trimmed().isEmpty()) errors << QStringLiteral("MISSING_SYMBOL");
    if (p.venue.trimmed().isEmpty()) errors << QStringLiteral("MISSING_VENUE");
    if (p.side != QLatin1String("BUY") && p.side != QLatin1String("SELL"))
        errors << QStringLiteral("INVALID_SIDE");
    if (p.limit_price <= 0.0) errors << QStringLiteral("INVALID_LIMIT_PRICE");
    if (p.notional_usd <= 0.0) errors << QStringLiteral("INVALID_NOTIONAL");
    if (p.expires_at_ms <= p.created_at_ms || p.expires_at_ms <= now_ms)
        errors << QStringLiteral("EXPIRED_PROPOSAL");
    if (p.execution_authority != QLatin1String("shadow_only") &&
        p.execution_authority != QLatin1String("paper_only") &&
        p.execution_authority != QLatin1String("canary_eligible"))
        errors << QStringLiteral("INVALID_AUTHORITY");
    // A long-only spot SELL must be backed by reconciled inventory.  This flag
    // is only evidence from the producer; the executor still rechecks balance.
    if (p.lane == QLatin1String("spot") && p.side == QLatin1String("SELL") &&
        !p.inventory_backed)
        errors << QStringLiteral("SPOT_SELL_NOT_INVENTORY_BACKED");
    return errors;
}

} // namespace openmarketterminal::services::crypto_execution
