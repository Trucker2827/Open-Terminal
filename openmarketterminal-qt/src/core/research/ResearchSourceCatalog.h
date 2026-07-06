#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace openmarketterminal::research_sources {

struct Entry {
    const char* id;
    const char* title;
    const char* panel;
    const char* source_type;
    const char* status;
    const char* config_key;
    const char* credential_key;
    const char* provider_id;
    const char* endpoint;
    bool keyless;
    bool gui;
    bool cli;
    bool mcp;
    bool daemon_candidate;
    const char* verify_command;
    const char* notes;
};

inline const QList<Entry>& entries() {
    static const QList<Entry> rows = {
        {"yfinance", "Yahoo / yfinance", "Equity Research", "public-market-data", "CONFIRMED",
         "", "", "yahoo-finance", "https://query2.finance.yahoo.com", true, true, true, true, true,
         "research quote AAPL; mcp call get_equity_quote symbol=AAPL",
         "Managed Python venv should contain yfinance. Used for quote, company info, history, financials, and fallback news."},
        {"sec_edgar", "SEC EDGAR", "Equity Research", "public-filings", "CONFIRMED",
         "", "", "sec-edgar", "https://data.sec.gov", true, true, true, true, true,
         "research filings AAPL --form 10-K; research metrics AAPL",
         "Keyless SEC company facts, submissions, filings, insiders, and 13F helpers."},
        {"google_news", "Google News company search", "Equity Research", "public-news", "CONFIRMED",
         "", "", "", "fetch_company_news.py", true, true, true, true, true,
         "research news AAPL --limit 5",
         "Primary company-news path before Yahoo/yfinance fallback."},
        {"newsapi", "NewsAPI.org", "Equity Research", "optional-news", "OPTIONAL_KEY",
         "", "NEWSAPI_KEY", "newsapi", "https://newsapi.org/v2/everything", false, true, true, false, true,
         "data connections; research news AAPL --limit 5",
         "Optional Data Sources connector. Equity code currently reads enabled Data Source config, not only the env-style key."},
        {"adanos_sentiment", "Adanos Market Sentiment", "Equity Research", "optional-alt-data", "OPTIONAL_KEY",
         "", "", "adanos-market-sentiment", "https://api.adanos.org", false, true, false, false, true,
         "data connections; research sentiment AAPL",
         "Optional sentiment overlay. Requires enabled Data Source connection with apiKey."},
        {"ainvest", "AInvest politician disclosures", "Equity Research", "optional-gov-data", "OPTIONAL_KEY",
         "", "AINVEST_API_KEY", "", "ainvest_data.py", false, true, true, false, true,
         "research politicians AAPL --limit 10",
         "Local key only. Used for congress/politician trade disclosures."},
        {"gdelt_doc", "GDELT DOC 2.0", "Geopolitics", "public-news-events", "CONFIRMED_RATE_LIMITED",
         "", "", "", "https://api.gdeltproject.org/api/v2/doc/doc", true, true, true, true, true,
         "mcp call fetch_geopolitics_events limit=5",
         "Real keyless feed, but strict rate limit around one request every five seconds."},
        {"gdelt_events", "GDELT event exports", "Geopolitics", "public-event-network", "CONFIRMED",
         "", "", "", "http://data.gdeltproject.org/gdeltv2/lastupdate.txt", true, true, true, false, true,
         "python scripts/geopolitics/gdelt_events_network.py --files 1 --top 5",
         "Builds relationship network from CAMEO-coded event exports."},
        {"hdx", "HDX Humanitarian Data Exchange", "Geopolitics", "public-humanitarian-data", "CONFIRMED",
         "", "", "", "https://data.humdata.org/api/3", true, true, true, true, true,
         "mcp call search_hdx_humanitarian '{}'",
         "Public CKAN API. Used by HDX data panel and geopolitics MCP tools."},
        {"trade_geopolitics", "Local trade-geopolitics model", "Geopolitics", "local-python-model", "LOCAL",
         "", "", "", "scripts/Analytics/economics/trade_geopolitics.py", true, true, true, true, false,
         "mcp call analyze_trade_benefits '{...}'",
         "Local analysis model, not a remote feed."},
        {"maritime_proxy", "Self-hosted maritime/AIS proxy", "Maritime", "self-hosted-feed", "OPTIONAL_CONFIG",
         "connectors.maritime_url", "", "", "connectors.maritime_url", false, true, false, false, true,
         "settings get connectors.maritime_url",
         "Local-first source. Empty means no vendor cloud call is made."},
        {"aisstream", "AISStream.io live vessel feed", "Maritime", "optional-websocket", "OPTIONAL_KEY",
         "connectors.aisstream_key", "", "", "wss://stream.aisstream.io/v0/stream", false, true, false, false, true,
         "settings get connectors.aisstream_key",
         "Free live AIS websocket with user key. Key is passed by environment to the subprocess, not argv."},
        {"wikidata_ports", "Wikidata ports", "Maritime", "public-reference-data", "CONFIRMED",
         "", "", "", "https://query.wikidata.org/sparql", true, true, false, false, false,
         "open maritime; search ports",
         "Primary keyless port catalog. Polite-use rate limits apply."},
        {"marineregions_ports", "MarineRegions ports", "Maritime", "public-reference-data", "CONFIRMED",
         "", "", "", "https://www.marineregions.org/rest", true, true, false, false, false,
         "open maritime; search ports",
         "Fallback port name-search source."},
        {"overpass_ports", "OSM Overpass ports", "Maritime", "public-reference-data", "CONFIRMED",
         "", "", "", "https://overpass-api.de/api/interpreter", true, true, false, false, false,
         "open maritime; bbox port search",
         "Fallback/supplemental bbox source when Wikidata is sparse."},
        {"databento", "Databento", "Surface Analytics", "paid-market-data", "OPTIONAL_KEY",
         "", "DATABENTO_API_KEY", "", "https://databento.com", false, true, true, true, true,
         "mcp call test_databento_connection '{}'",
         "Surface Analytics provider. Python package may be installed even when the API key is absent."},
        {"surface_demo", "Synthetic surface generator", "Surface Analytics", "local-demo-data", "LOCAL_DEMO",
         "", "", "", "SurfaceCapabilities DEMO", true, true, false, false, false,
         "open surface_analytics",
         "Useful for UI demos only; must stay visibly labeled as synthetic."},
        {"ma_python", "Corporate finance Python models", "M&A Analytics", "local-python-model", "LOCAL",
         "", "", "", "scripts/Analytics/corporateFinance", true, true, false, true, false,
         "mcp search ma_",
         "DCF, LBO, merger model, fairness, deal comparison. Mostly user-input/local computation."},
        {"alt_python", "Alternative-investment Python models", "Alt Investments", "local-python-model", "LOCAL",
         "", "", "", "scripts/Analytics/alternateInvestment/cli.py", true, true, false, true, false,
         "mcp search alt_",
         "Private equity, REITs, structured notes, risk/performance calculators. Mostly user-input/local computation."},
    };
    return rows;
}

inline QJsonObject entry_to_json(const Entry& e) {
    return QJsonObject{{"id", QString::fromLatin1(e.id)},
                       {"title", QString::fromLatin1(e.title)},
                       {"panel", QString::fromLatin1(e.panel)},
                       {"source_type", QString::fromLatin1(e.source_type)},
                       {"status", QString::fromLatin1(e.status)},
                       {"config_key", QString::fromLatin1(e.config_key)},
                       {"credential_key", QString::fromLatin1(e.credential_key)},
                       {"provider_id", QString::fromLatin1(e.provider_id)},
                       {"endpoint", QString::fromLatin1(e.endpoint)},
                       {"keyless", e.keyless},
                       {"gui", e.gui},
                       {"cli", e.cli},
                       {"mcp", e.mcp},
                       {"daemon_candidate", e.daemon_candidate},
                       {"verify_command", QString::fromLatin1(e.verify_command)},
                       {"notes", QString::fromLatin1(e.notes)}};
}

inline QJsonArray entries_to_json() {
    QJsonArray arr;
    for (const auto& e : entries())
        arr.append(entry_to_json(e));
    return arr;
}

} // namespace openmarketterminal::research_sources
