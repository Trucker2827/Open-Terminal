#pragma once
// Key-less fallback for the FRED panel: flagship FRED series mapped to their
// ORIGINAL sources' DBnomics mirrors (BEA / BLS / Federal Reserve releases),
// served by the app's native key-less DBnomicsService. Only mappings that
// were LIVE-VERIFIED against api.db.nomics.world (2026-07-22) are listed —
// e.g. BEA/NIPA-T10105/A191RC-Q returned Q1-2026 GDP matching FRED exactly.
// Unmapped series keep the add-a-FRED-key guidance. Pure header: testable.

#include <QString>

#include <optional>

namespace openmarketterminal::screens {

struct FredDbnFallback {
    QString provider;
    QString dataset;
    QString series;
    QString source_label; // e.g. "BEA NIPA mirror"
};

inline std::optional<FredDbnFallback> fred_dbnomics_fallback(const QString& fred_id) {
    const QString id = fred_id.trimmed().toUpper();
    if (id == QLatin1String("GDP"))
        return FredDbnFallback{"BEA", "NIPA-T10105", "A191RC-Q", "BEA NIPA mirror"};
    if (id == QLatin1String("GDPC1"))
        return FredDbnFallback{"BEA", "NIPA-T10106", "A191RX-Q", "BEA NIPA mirror"};
    if (id == QLatin1String("CPIAUCSL"))
        return FredDbnFallback{"BLS", "cu", "CUSR0000SA0", "BLS CPI mirror"};
    if (id == QLatin1String("CPILFESL"))
        return FredDbnFallback{"BLS", "cu", "CUSR0000SA0L1E", "BLS CPI mirror"};
    if (id == QLatin1String("UNRATE"))
        return FredDbnFallback{"BLS", "ln", "LNS14000000", "BLS labor-force mirror"};
    if (id == QLatin1String("FEDFUNDS"))
        return FredDbnFallback{"FED", "H15", "RIFSPFF_N.M", "Fed H.15 mirror"};
    if (id == QLatin1String("DGS10"))
        return FredDbnFallback{"FED", "H15", "RIFLGFCY10_N.B", "Fed H.15 mirror"};
    if (id == QLatin1String("DGS2"))
        return FredDbnFallback{"FED", "H15", "RIFLGFCY02_N.B", "Fed H.15 mirror"};
    return std::nullopt;
}

} // namespace openmarketterminal::screens
