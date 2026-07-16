#pragma once
// Screener.h — ai screen shortlist Task 1: screen(), the reusable, PURE-READ
// core that gathers distinct recent (symbol, venue) pairs from
// edge_decision_journal per market, runs each through DecisionContext::
// assess (REUSE — see DecisionContext.h; this piece never re-derives the
// gate logic), keeps only rows whose recommendation_hint is "all gates
// pass", ranks the survivors by edge_after_cost descending, and returns the
// top-N as ScreenRows.
//
// READ-ONLY INVARIANT (binding, same as DecisionContext.h): screen() issues
// only SELECTs against edge_decision_journal (directly, for the universe
// query) and via assess() (per-symbol packet). It never INSERTs/UPDATEs/
// DELETEs, places an order, or mutates a gate/setting.

#include <QJsonArray>
#include <QString>
#include <QVector>

namespace openmarketterminal::ai_decision {

// One shortlisted symbol: the subset of its DecisionPacket fields needed to
// present a ranked, gate-vetted candidate list.
struct ScreenRow {
    QString symbol;
    QString market;
    double edge_after_cost = 0.0;
    QString side;
    QString horizon;
    QString freshness;
    QString recommendation_hint;
};

// Maps a market name to a SQL WHERE fragment over `venue`
// (" AND venue IN ('a','b',...)"). Returns "" for an empty market (no
// filter — all venues). An unrecognized, non-empty market returns a
// fragment that matches nothing (" AND 0"), so screen() on a bogus market
// name comes back empty rather than silently falling through to "all
// venues".
QString market_venue_filter(const QString& market);

// Reverse of the market->venue mapping used by market_venue_filter: maps a
// single venue back to its market name ("prediction"|"equity"|"crypto"), or
// "" if the venue is not recognized.
QString market_for_venue(const QString& venue);

// Gathers the distinct (symbol, venue) pairs seen in edge_decision_journal
// within the last 24h relative to the most recent matching row (a
// test-stable, "recent activity" window rather than a wall-clock one), runs
// each through DecisionContext::assess, keeps only the rows whose
// recommendation_hint is "all gates pass", ranks the survivors by
// edge_after_cost descending, and returns the top `limit` (default 5;
// non-positive treated as the default). `market` filters the universe to
// that market's venues; empty means all markets. PURE READ — see file
// header.
QVector<ScreenRow> screen(const QString& market = {}, int limit = 5);

// Serializes each ScreenRow to {symbol, market, edge_after_cost, side,
// horizon, freshness, recommendation_hint}.
QJsonArray screen_to_json(const QVector<ScreenRow>& rows);

} // namespace openmarketterminal::ai_decision
