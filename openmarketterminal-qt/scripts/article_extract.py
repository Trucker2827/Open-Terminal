"""
Article Extractor
Fetches and extracts the full text of a news article from its URL.
Uses three strategies in order: JSON-LD articleBody, trafilatura, readability heuristic.
Returns the longest result >= 300 chars.

Returns JSON to stdout, exits 0 on both success and handled errors.
"""

import sys
import json
import re
import html


def _fetch_url(url):
    """Fetch URL with browser User-Agent. Returns (response, error_dict)."""
    import urllib.request

    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": (
                "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/120 Safari/537.36"
            ),
            "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
            "Accept-Language": "en-US,en;q=0.9",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            charset = "utf-8"
            ct = resp.headers.get_content_charset()
            if ct:
                charset = ct
            body = resp.read().decode(charset, errors="replace")
            return body, None
    except urllib.error.HTTPError as e:
        return None, {"success": False, "error": str(e), "error_code": f"HTTP_{e.code}"}
    except Exception as e:
        return None, {"success": False, "error": str(e), "error_code": "FETCH_FAILED"}


# ---------------------------------------------------------------------------
# Strategy 1: JSON-LD articleBody
# ---------------------------------------------------------------------------

def _extract_json_ld(html_text):
    """
    Find <script type="application/ld+json"> blocks, parse each,
    flatten lists and '@graph' arrays, find dicts with 'articleBody'.
    Returns (title, article_body_text) or ("", "").
    """
    _ARTICLE_TYPES = {
        "article", "newsarticle", "reportage", "backgroundnewsarticle",
        "reviewarticle", "opinion", "opinion article",
    }

    pattern = re.compile(
        r'<script[^>]+type=["\']application/ld\+json["\'][^>]*>(.*?)</script>',
        re.S | re.I,
    )

    candidates = []

    for m in pattern.finditer(html_text):
        raw = m.group(1).strip()
        try:
            data = json.loads(raw)
        except Exception:
            continue

        # Normalise to a list of dicts
        items = []
        if isinstance(data, list):
            items = data
        elif isinstance(data, dict):
            items = [data]

        # Expand @graph arrays
        expanded = []
        for item in items:
            if isinstance(item, dict) and "@graph" in item:
                g = item["@graph"]
                if isinstance(g, list):
                    expanded.extend(g)
                else:
                    expanded.append(item)
            else:
                expanded.append(item)

        for item in expanded:
            if not isinstance(item, dict):
                continue
            body = item.get("articleBody", "")
            if not body or not isinstance(body, str):
                continue
            body = html.unescape(body).strip()
            if len(body) < 300:
                continue
            type_val = item.get("@type", "")
            if isinstance(type_val, list):
                type_val = " ".join(type_val)
            type_lower = type_val.lower()
            is_article_type = any(t in type_lower for t in _ARTICLE_TYPES)
            title = item.get("headline", item.get("name", ""))
            if isinstance(title, str):
                title = html.unescape(title).strip()
            candidates.append((len(body), is_article_type, title, body))

    if not candidates:
        return "", ""

    # Prefer article-typed candidates; among ties, prefer longer
    candidates.sort(key=lambda x: (x[1], x[0]), reverse=True)
    _, _, title, body = candidates[0]
    return title, body


# ---------------------------------------------------------------------------
# Strategy 2: trafilatura
# ---------------------------------------------------------------------------

def _extract_trafilatura(html_text):
    """Returns extracted text or empty string."""
    try:
        import trafilatura  # type: ignore
    except ImportError:
        return ""
    try:
        result = trafilatura.extract(
            html_text,
            include_comments=False,
            include_tables=False,
        )
        return result or ""
    except Exception:
        return ""


# ---------------------------------------------------------------------------
# Strategy 3: Readability heuristic (no extra deps)
# ---------------------------------------------------------------------------

def _strip_tags(s):
    """Remove HTML tags from a string."""
    return re.sub(r"<[^>]+>", "", s)


def _extract_readability(html_text):
    """
    Prefer <article> content; else collect <p> blocks.
    Strip tags, drop short fragments (< 40 chars), join with blank lines.
    """
    # Try <article> first
    article_m = re.search(r"<article[^>]*>(.*?)</article>", html_text, re.S | re.I)
    if article_m:
        text = _strip_tags(article_m.group(1))
        # collapse whitespace
        text = re.sub(r"\s+", " ", text).strip()
        if len(text) >= 300:
            return text

    # Collect all <p> blocks
    fragments = []
    for m in re.finditer(r"<p[^>]*>(.*?)</p>", html_text, re.S | re.I):
        t = _strip_tags(m.group(1))
        t = re.sub(r"\s+", " ", t).strip()
        if len(t) >= 40:
            fragments.append(t)

    return "\n\n".join(fragments)


# ---------------------------------------------------------------------------
# Title extraction helper
# ---------------------------------------------------------------------------

def _extract_title(html_text):
    """Extract best available page title."""
    # og:title
    m = re.search(r'<meta[^>]+property=["\']og:title["\'][^>]+content=["\']([^"\']+)["\']',
                  html_text, re.I)
    if m:
        return html.unescape(m.group(1).strip())
    # <title>
    m = re.search(r"<title[^>]*>(.*?)</title>", html_text, re.S | re.I)
    if m:
        return html.unescape(_strip_tags(m.group(1)).strip())
    return ""


# ---------------------------------------------------------------------------
# Main extraction
# ---------------------------------------------------------------------------

def extract_article(url):
    """
    Fetch url and run three strategies, keeping the longest result >= 300 chars.
    Returns a dict matching the expected JSON schema.
    """
    html_text, fetch_err = _fetch_url(url)
    if fetch_err:
        return fetch_err

    candidates = []  # list of (length, method, title, text)

    # Strategy 1: JSON-LD
    jld_title, jld_body = _extract_json_ld(html_text)
    if len(jld_body) >= 300:
        candidates.append((len(jld_body), "json_ld", jld_title, jld_body))

    # Strategy 2: trafilatura
    traf_text = _extract_trafilatura(html_text)
    if len(traf_text) >= 300:
        page_title = jld_title or _extract_title(html_text)
        candidates.append((len(traf_text), "trafilatura", page_title, traf_text))

    # Strategy 3: readability heuristic
    read_text = _extract_readability(html_text)
    if len(read_text) >= 300:
        page_title = jld_title or _extract_title(html_text)
        candidates.append((len(read_text), "readability", page_title, read_text))

    if not candidates:
        return {
            "success": False,
            "error": "Could not extract article text (all strategies returned < 300 chars)",
            "error_code": "NO_CONTENT",
        }

    # Keep longest result
    candidates.sort(key=lambda x: x[0], reverse=True)
    _, method, title, text = candidates[0]

    word_count = len(text.split())
    return {
        "success": True,
        "title": title,
        "text": text,
        "word_count": word_count,
        "method": method,
    }


def main():
    if len(sys.argv) < 2:
        result = {
            "success": False,
            "error": "Usage: article_extract.py <article_url>",
            "error_code": "FETCH_FAILED",
        }
        print(json.dumps(result))
        sys.exit(0)

    url = sys.argv[1]

    try:
        result = extract_article(url)
    except Exception as exc:
        result = {
            "success": False,
            "error": str(exc),
            "error_code": "FETCH_FAILED",
        }

    # Only write to stdout; debug/progress goes to stderr.
    print(json.dumps(result))
    sys.exit(0)


if __name__ == "__main__":
    main()
