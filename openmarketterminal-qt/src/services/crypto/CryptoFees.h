#pragma once
// Single source of truth for crypto venue fee economics. Every consumer —
// CLI scalp gate, `crypto fees` commands, the serve-loop scalp report, and
// the GUI microstructure widget — reads these constants from here so a fee
// tier is never duplicated (the roadmap's net-of-round-trip hurdle depends
// on GUI and CLI agreeing on the same numbers). Local overrides configured
// with `crypto fees set` are applied by crypto_fee_profile_for_venue().

#include <QString>
#include <QVector>

#include <optional>

namespace openmarketterminal::services::crypto {

struct CryptoFeeProfile {
    QString venue_key;
    QString profile;
    QString source;
    QString note;
    double maker_bps = 0.0;
    double taker_bps = 0.0;
    double rebate_pct = 0.0;
    double free_remaining_usd = 0.0;
    bool free_applies_to_advanced = false;
    double slippage_bps = 0.0;
};

struct CoinbaseFeeTier {
    const char* key;
    const char* profile;
    const char* volume;
    double maker_bps;
    double taker_bps;
};

// Built-in Coinbase Advanced/Exchange fee schedule, tier 1 ($0-$10K) first.
const QVector<CoinbaseFeeTier>& coinbase_fee_tiers();
std::optional<CoinbaseFeeTier> coinbase_fee_tier_by_key(const QString& key);

// Canonical venue key ("coinbase tier 2" -> "coinbase_tier2", unknown -> "default").
QString crypto_fee_venue_key(const QString& venue);

// Settings key for a locally configured fee field (crypto.fee.<venue>.<field>).
QString crypto_fee_key(const QString& venue_key, const QString& field);

// Built-in conservative defaults for a canonical venue key (no local overrides).
CryptoFeeProfile crypto_default_fee_profile(const QString& venue_key);

// Defaults plus any local `crypto fees set` overrides from SettingsRepository.
CryptoFeeProfile crypto_fee_profile_for_venue(const QString& venue);

} // namespace openmarketterminal::services::crypto
