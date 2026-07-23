#pragma once
#include <QString>
#include <QVector>
namespace openmarketterminal::web {
struct WebResult { QString title; QString url; QString snippet; };
// Parse the DuckDuckGo html-endpoint (html.duckduckgo.com/html) result list.
// Returns up to max_results rows; decodes DDG /l/?uddg= redirect links to the
// real URL. Tolerant of missing snippets. Pure (no network, no Qt GUI).
QVector<WebResult> parse_ddg_results(const QString& html, int max_results);

// Parse a Mojeek (mojeek.com/search) result list — the keyless fallback
// engine when DuckDuckGo serves its anti-bot challenge. Pure.
QVector<WebResult> parse_mojeek_results(const QString& html, int max_results);

// True when a page is an anti-bot interstitial (DDG "anomaly" challenge,
// generic captcha walls) rather than a result list — callers should fall
// through to the next engine instead of reporting "no results".
bool looks_like_bot_challenge(const QString& html);
}
