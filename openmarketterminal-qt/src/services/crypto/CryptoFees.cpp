#include "services/crypto/CryptoFees.h"

#include "storage/repositories/SettingsRepository.h"

#include <algorithm>

namespace openmarketterminal::services::crypto {

namespace {

using openmarketterminal::SettingsRepository;

QString settings_string_value(const QString& key, const QString& fallback = {}) {
    auto r = SettingsRepository::instance().get(key, fallback);
    if (r.is_err())
        return fallback;
    const QString value = r.value().trimmed();
    return value.isEmpty() ? fallback : value;
}

double settings_double_value(const QString& key, double fallback) {
    bool ok = false;
    const double value = settings_string_value(key, QString::number(fallback, 'g', 12)).toDouble(&ok);
    return ok ? value : fallback;
}

bool settings_bool_value(const QString& key, bool fallback) {
    const QString raw = settings_string_value(key, fallback ? QStringLiteral("true") : QStringLiteral("false")).toLower();
    if (raw == QLatin1String("true") || raw == QLatin1String("yes") || raw == QLatin1String("1") ||
        raw == QLatin1String("on"))
        return true;
    if (raw == QLatin1String("false") || raw == QLatin1String("no") || raw == QLatin1String("0") ||
        raw == QLatin1String("off"))
        return false;
    return fallback;
}

} // namespace

const QVector<CoinbaseFeeTier>& coinbase_fee_tiers() {
    static const QVector<CoinbaseFeeTier> tiers = {
        {"coinbase_advanced", "coinbase-advanced-tier1", "$0K-$10K", 40.0, 60.0},
        {"coinbase_tier2", "coinbase-advanced-tier2", "$10K-$50K", 25.0, 40.0},
        {"coinbase_tier3", "coinbase-advanced-tier3", "$50K-$100K", 15.0, 25.0},
        {"coinbase_tier4", "coinbase-advanced-tier4", "$100K-$1M", 10.0, 20.0},
        {"coinbase_tier5", "coinbase-advanced-tier5", "$1M-$15M", 8.0, 18.0},
        {"coinbase_tier6", "coinbase-advanced-tier6", "$15M-$75M", 6.0, 16.0},
        {"coinbase_tier7", "coinbase-advanced-tier7", "$75M-$250M", 3.0, 10.0},
        {"coinbase_tier8", "coinbase-advanced-tier8", "$250M-$400M", 0.0, 6.0},
        {"coinbase_tier9", "coinbase-advanced-tier9", "$400M+", 0.0, 4.0},
    };
    return tiers;
}

std::optional<CoinbaseFeeTier> coinbase_fee_tier_by_key(const QString& key) {
    for (const CoinbaseFeeTier& tier : coinbase_fee_tiers()) {
        if (key == QLatin1String(tier.key))
            return tier;
    }
    return std::nullopt;
}

QString crypto_fee_venue_key(const QString& venue) {
    const QString v = venue.trimmed().toLower();
    QString compact = v;
    compact.replace(QLatin1Char('-'), QLatin1Char('_'));
    compact.replace(QLatin1Char(' '), QLatin1Char('_'));
    if (compact == QLatin1String("coinbase_tier1") || compact == QLatin1String("coinbase_tier_1") ||
        compact == QLatin1String("coinbase_base") || compact == QLatin1String("coinbase_advanced"))
        return QStringLiteral("coinbase_advanced");
    if (compact == QLatin1String("coinbase_tier2") || compact == QLatin1String("coinbase_tier_2") ||
        compact == QLatin1String("coinbase_10k") || compact == QLatin1String("coinbase_10k_50k"))
        return QStringLiteral("coinbase_tier2");
    if (compact == QLatin1String("coinbase_tier3") || compact == QLatin1String("coinbase_tier_3") ||
        compact == QLatin1String("coinbase_50k") || compact == QLatin1String("coinbase_50k_100k"))
        return QStringLiteral("coinbase_tier3");
    if (compact == QLatin1String("coinbase_tier4") || compact == QLatin1String("coinbase_tier_4") ||
        compact == QLatin1String("coinbase_100k") || compact == QLatin1String("coinbase_100k_1m"))
        return QStringLiteral("coinbase_tier4");
    if (compact == QLatin1String("coinbase_tier5") || compact == QLatin1String("coinbase_tier_5") ||
        compact == QLatin1String("coinbase_1m") || compact == QLatin1String("coinbase_1m_15m"))
        return QStringLiteral("coinbase_tier5");
    if (compact == QLatin1String("coinbase_tier6") || compact == QLatin1String("coinbase_tier_6") ||
        compact == QLatin1String("coinbase_15m") || compact == QLatin1String("coinbase_15m_75m"))
        return QStringLiteral("coinbase_tier6");
    if (compact == QLatin1String("coinbase_tier7") || compact == QLatin1String("coinbase_tier_7") ||
        compact == QLatin1String("coinbase_75m") || compact == QLatin1String("coinbase_75m_250m"))
        return QStringLiteral("coinbase_tier7");
    if (compact == QLatin1String("coinbase_tier8") || compact == QLatin1String("coinbase_tier_8") ||
        compact == QLatin1String("coinbase_250m") || compact == QLatin1String("coinbase_250m_400m"))
        return QStringLiteral("coinbase_tier8");
    if (compact == QLatin1String("coinbase_tier9") || compact == QLatin1String("coinbase_tier_9") ||
        compact == QLatin1String("coinbase_400m") || compact == QLatin1String("coinbase_400m_plus"))
        return QStringLiteral("coinbase_tier9");
    if (v.contains("coinbase"))
        return QStringLiteral("coinbase_advanced");
    if (v.contains("alpaca"))
        return QStringLiteral("alpaca_crypto");
    if (v.contains("kraken"))
        return QStringLiteral("kraken_pro");
    if (v.contains("binance"))
        return QStringLiteral("binanceus");
    return v.isEmpty() || v == QLatin1String("unknown") ? QStringLiteral("default") : v;
}

QString crypto_fee_key(const QString& venue_key, const QString& field) {
    return QStringLiteral("crypto.fee.%1.%2").arg(venue_key, field);
}

CryptoFeeProfile crypto_default_fee_profile(const QString& venue_key) {
    CryptoFeeProfile p;
    p.venue_key = venue_key;
    p.profile = QStringLiteral("built-in-default");
    p.source = QStringLiteral("built-in conservative estimate");
    p.note = QStringLiteral("Configure this locally with `crypto fees set`; exact exchange/account tier may differ.");
    if (const auto coinbase_tier = coinbase_fee_tier_by_key(venue_key)) {
        p.profile = QString::fromLatin1(coinbase_tier->profile);
        p.maker_bps = coinbase_tier->maker_bps;
        p.taker_bps = coinbase_tier->taker_bps;
        p.note = QStringLiteral("Coinbase Advanced %1 estimate: %2 trailing 30-day volume, %3bps maker / %4bps taker. Coinbase One simple-trade fee perks are not assumed for Advanced API orders.")
                     .arg(QString::fromLatin1(coinbase_tier->profile).section(QLatin1Char('-'), -1),
                          QString::fromLatin1(coinbase_tier->volume))
                     .arg(coinbase_tier->maker_bps)
                     .arg(coinbase_tier->taker_bps);
    } else if (venue_key == QLatin1String("alpaca_crypto")) {
        p.profile = QStringLiteral("alpaca-crypto-tier1");
        p.maker_bps = 15.0;
        p.taker_bps = 25.0;
        p.note = QStringLiteral("Alpaca crypto tier 1 is volume/account dependent; set your account-specific bps here before live trading.");
    } else if (venue_key == QLatin1String("kraken_pro")) {
        p.profile = QStringLiteral("kraken-pro-base");
        p.maker_bps = 25.0;
        p.taker_bps = 40.0;
    } else if (venue_key == QLatin1String("binanceus")) {
        p.profile = QStringLiteral("binanceus-spot");
        p.maker_bps = 0.0;
        p.taker_bps = 1.0;
        p.note = QStringLiteral("Binance.US public fee page lists 0bps maker / 1bp taker for Tier 0 pairs; verify pair tier, account availability, and withdrawal costs before live trading.");
    } else {
        p.maker_bps = 10.0;
        p.taker_bps = 20.0;
    }
    return p;
}

CryptoFeeProfile crypto_fee_profile_for_venue(const QString& venue) {
    CryptoFeeProfile p = crypto_default_fee_profile(crypto_fee_venue_key(venue));
    const QString prefix = p.venue_key;
    const QString configured_profile = settings_string_value(crypto_fee_key(prefix, QStringLiteral("profile")));
    if (!configured_profile.isEmpty()) {
        p.profile = configured_profile;
        p.source = QStringLiteral("local settings");
    }
    p.maker_bps = settings_double_value(crypto_fee_key(prefix, QStringLiteral("maker_bps")), p.maker_bps);
    p.taker_bps = settings_double_value(crypto_fee_key(prefix, QStringLiteral("taker_bps")), p.taker_bps);
    p.rebate_pct = std::clamp(settings_double_value(crypto_fee_key(prefix, QStringLiteral("rebate_pct")), p.rebate_pct),
                              0.0, 100.0);
    p.free_remaining_usd = std::max(0.0, settings_double_value(crypto_fee_key(prefix, QStringLiteral("free_remaining_usd")),
                                                               p.free_remaining_usd));
    p.free_applies_to_advanced = settings_bool_value(crypto_fee_key(prefix, QStringLiteral("free_applies_to_advanced")),
                                                     p.free_applies_to_advanced);
    p.slippage_bps = std::max(0.0, settings_double_value(crypto_fee_key(prefix, QStringLiteral("slippage_bps")),
                                                         p.slippage_bps));
    const QString note = settings_string_value(crypto_fee_key(prefix, QStringLiteral("note")));
    if (!note.isEmpty())
        p.note = note;
    return p;
}

} // namespace openmarketterminal::services::crypto
