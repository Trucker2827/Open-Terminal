#include "services/crypto_execution/CryptoExecutionProposal.h"

#include <QtTest>

using namespace openmarketterminal::services::crypto_execution;

class CryptoExecutionProposalTest final : public QObject {
    Q_OBJECT
  private slots:
    void canonical_proposal_serializes_authority_and_ledger() {
        CryptoExecutionProposal p;
        p.proposal_id = QStringLiteral("btc1h-1");
        p.strategy_id = QStringLiteral("coinbase-btc1h-spot-v1");
        p.policy_hash = QStringLiteral("abc123");
        p.ledger = QStringLiteral("btc1h_spot_decisions.jsonl");
        p.lane = QStringLiteral("spot");
        p.symbol = QStringLiteral("BTC-USD");
        p.venue = QStringLiteral("coinbase_advanced");
        p.side = QStringLiteral("BUY");
        p.liquidity = QStringLiteral("maker");
        p.time_in_force = QStringLiteral("GTC");
        p.created_at_ms = 1'000;
        p.expires_at_ms = 2'000;
        p.limit_price = 64'000.0;
        p.notional_usd = 10.0;
        p.net_edge_bps = 20.0;
        const auto json = proposal_json(p);
        QCOMPARE(json.value(QStringLiteral("schema")).toString(),
                 QStringLiteral("crypto-execution-proposal-v1"));
        QCOMPARE(json.value(QStringLiteral("execution_authority")).toString(),
                 QStringLiteral("shadow_only"));
        QCOMPARE(json.value(QStringLiteral("ledger")).toString(), p.ledger);
        QVERIFY(validate_proposal(p, 1'500).isEmpty());
    }

    void unbacked_spot_sell_fails_closed() {
        CryptoExecutionProposal p;
        p.proposal_id = QStringLiteral("btc1h-2");
        p.strategy_id = QStringLiteral("coinbase-btc1h-spot-v1");
        p.policy_hash = QStringLiteral("abc123");
        p.ledger = QStringLiteral("btc1h_spot_decisions.jsonl");
        p.lane = QStringLiteral("spot");
        p.symbol = QStringLiteral("BTC-USD");
        p.venue = QStringLiteral("coinbase_advanced");
        p.side = QStringLiteral("SELL");
        p.created_at_ms = 1'000;
        p.expires_at_ms = 2'000;
        p.limit_price = 64'000.0;
        p.notional_usd = 10.0;
        const auto errors = validate_proposal(p, 1'500);
        QVERIFY(errors.contains(QStringLiteral("SPOT_SELL_NOT_INVENTORY_BACKED")));
    }
};

QTEST_GUILESS_MAIN(CryptoExecutionProposalTest)
#include "tst_crypto_execution_proposal.moc"
