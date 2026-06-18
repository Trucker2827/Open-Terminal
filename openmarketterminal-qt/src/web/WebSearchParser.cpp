#include "web/WebSearchParser.h"
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

namespace openmarketterminal::web {

static QString strip_tags(QString s) {
    s.remove(QRegularExpression("<[^>]*>"));
    s = s.toHtmlEscaped();                       // normalize, then unescape entities
    s.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">")
     .replace("&quot;", "\"").replace("&#x27;", "'").replace("&#39;", "'").replace("&nbsp;", " ");
    return s.simplified();
}

static QString decode_ddg_url(const QString& href) {
    // DDG wraps results as //duckduckgo.com/l/?uddg=<urlencoded real url>&...
    // or /l/?uddg=...
    QUrl u(href);
    if (u.path().contains("/l/")) {
        const QString uddg = QUrlQuery(u).queryItemValue("uddg", QUrl::FullyDecoded);
        if (!uddg.isEmpty()) return uddg;
    }
    if (href.startsWith("//")) return "https:" + href;
    return href;
}

QVector<WebResult> parse_ddg_results(const QString& html, int max_results) {
    QVector<WebResult> out;
    if (html.isEmpty() || max_results <= 0) return out;
    // Each result anchor: <a class="result__a" href="...">title</a>
    static const QRegularExpression kAnchor(
        "<a[^>]*class=\"[^\"]*result__a[^\"]*\"[^>]*href=\"([^\"]+)\"[^>]*>(.*?)</a>",
        QRegularExpression::DotMatchesEverythingOption);
    // Snippet: <a class="result__snippet" ...>snippet</a>  (may be absent)
    static const QRegularExpression kSnippet(
        "<a[^>]*class=\"[^\"]*result__snippet[^\"]*\"[^>]*>(.*?)</a>",
        QRegularExpression::DotMatchesEverythingOption);

    auto anchors = kAnchor.globalMatch(html);
    auto snippets = kSnippet.globalMatch(html);
    while (anchors.hasNext() && out.size() < max_results) {
        const auto m = anchors.next();
        WebResult r;
        r.url = decode_ddg_url(m.captured(1));
        r.title = strip_tags(m.captured(2));
        if (snippets.hasNext()) r.snippet = strip_tags(snippets.next().captured(1));
        if (!r.title.isEmpty() && r.url.startsWith("http")) out.push_back(r);
    }
    return out;
}
}
