#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

// Pure reader/formatter for kalshi-strategy-grid-latest.json (the strategy-grid
// engine's compact verdict file). No I/O of its own — the caller reads the file
// and passes the bytes. Fails CLOSED and never promotes a candidate to a winner,
// mirroring BotCockpitPresentation's issue-#145 discipline.
namespace openmarketterminal::services::prediction::kalshi_ns {

inline constexpr qint64 kGridStaleMs = 6LL * 3600LL * 1000LL;   // 6h

struct GridRow {
    QString variant_id, side, band, gate, exit, trust, blocked_by;
    double delta_vs_hold = 0.0, delta_vs_market = 0.0, effective_n = 0.0;
};

struct GridLatest {
    bool available = false;
    int schema_version = 0;
    QString headline;
    qint64 age_ms = -1;
    bool stale = false;
    QVector<GridRow> survivors;
    QVector<GridRow> candidates;
};

/// Parse kalshi-strategy-grid-latest.json. Returns available=false on empty,
/// unparseable, or schema-version-mismatched input. `now_ms` sets age_ms/stale.
GridLatest parse_grid_latest(const QByteArray& json, qint64 now_ms);

/// One advisory line for the bot cockpit.
QString grid_cockpit_line(const GridLatest&);

/// CLI report: headline + survivors + closest candidates (or `raw` when as_json).
QString grid_cli_report(const GridLatest&, bool as_json, const QByteArray& raw);

}  // namespace openmarketterminal::services::prediction::kalshi_ns
