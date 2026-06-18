#pragma once
#include <QString>
#include <QVector>
namespace openmarketterminal::web {
struct WebResult { QString title; QString url; QString snippet; };
// Parse the DuckDuckGo html-endpoint (html.duckduckgo.com/html) result list.
// Returns up to max_results rows; decodes DDG /l/?uddg= redirect links to the
// real URL. Tolerant of missing snippets. Pure (no network, no Qt GUI).
QVector<WebResult> parse_ddg_results(const QString& html, int max_results);
}
