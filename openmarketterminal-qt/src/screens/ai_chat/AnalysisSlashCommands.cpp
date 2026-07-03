#include "screens/ai_chat/AnalysisSlashCommands.h"

#include <QList>
#include <QRegularExpression>

namespace openmarketterminal::ai_chat {

namespace {
struct Cmd {
    const char* name;
    const char* usage;
    const char* tmpl;  // %1 = ticker/argument (upper-cased)
};

const QList<Cmd>& commands() {
    static const QList<Cmd> kCmds = {
        {"comps", "/comps TICKER",
         "Run the comps playbook: build a comparable-company analysis for %1. Identify 4-6 true "
         "peers, pull operating metrics and valuation multiples with the edgar tools, compute "
         "median/quartile multiples, derive the implied value vs. the current price, and build the "
         "peer table + read in the Report Builder."},
        {"dcf", "/dcf TICKER",
         "Run the DCF playbook: build a discounted-cash-flow intrinsic valuation for %1 using the "
         "edgar tools — history, revenue projections, FCF build, WACC, terminal value, equity "
         "bridge to an implied price, and a WACC×terminal-g sensitivity table — in the Report Builder."},
        {"earnings", "/earnings TICKER",
         "Run the earnings/filing-note playbook for %1: anchor the latest financials with YoY "
         "deltas, read the MD&A and risk sections, synthesize what drove the result plus guidance "
         "and the key risks, and write the note in the Report Builder."},
        {"brief", "/brief TARGET",
         "Run the AI market brief playbook for %1: synthesize the latest price action, relevant "
         "news, filings or macro context, technical setup, open risks, and next-watch catalysts. "
         "Use app tools first, cite sources and timestamps, and produce a concise decision-ready "
         "brief in the Report Builder."},
        {"risk", "/risk PORTFOLIO_OR_SYMBOL",
         "Run the AI risk review playbook for %1: inspect exposures, concentration, drawdown, "
         "volatility, correlation, news/event risks, liquidity, and rule violations. Produce a "
         "risk dashboard summary with concrete mitigations and cite every data source."},
        {"thesis", "/thesis TICKER",
         "Run the thesis monitor playbook for %1: extract or draft the current bull/base/bear "
         "thesis, identify key assumptions, look for confirming and disconfirming evidence in "
         "filings, news, price action, and fundamentals, then write a sourced thesis update."},
        {"radar", "/radar WATCHLIST_OR_TOPIC",
         "Run the AI news radar playbook for %1: build a personalized monitoring brief across "
         "news, filings, macro data, portfolio/watchlist exposure, and catalysts. Rank what matters "
         "by probable market impact, novelty, and relevance to the user's holdings or thesis."},
        {"lbo", "/lbo TICKER",
         "Run the LBO playbook for %1: set entry EV from an EV/EBITDA multiple, build Sources & "
         "Uses (debt vs sponsor equity), a 5-yr operating model and debt schedule with cash sweep, "
         "an exit, and sponsor returns (IRR/MOIC) with a sensitivity grid — in the Report Builder."},
        {"3statement", "/3statement TICKER",
         "Run the 3-statement playbook for %1: pull historicals with the edgar tools, then build "
         "linked Income Statement, Balance Sheet, and Cash Flow projections with the balance check "
         "(A=L+E) and cash tie-out holding every period, plus credit metrics — in the Report Builder."},
    };
    return kCmds;
}
} // namespace

QString expand_analysis_slash_command(const QString& raw, QString* usage_out) {
    const QString trimmed = raw.trimmed();
    if (!trimmed.startsWith('/'))
        return {};
    const QStringList parts =
        trimmed.mid(1).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return {};
    const QString cmd = parts.first().toLower();
    const QString arg = parts.size() > 1 ? parts.mid(1).join(' ').trimmed().toUpper() : QString();

    for (const auto& c : commands()) {
        if (cmd != QLatin1String(c.name))
            continue;
        if (arg.isEmpty()) {
            if (usage_out)
                *usage_out = QStringLiteral("Usage: %1").arg(QLatin1String(c.usage));
            return {};
        }
        return QString(QLatin1String(c.tmpl)).arg(arg);
    }
    return {};  // unrecognized slash command — caller sends the raw text unchanged
}

} // namespace openmarketterminal::ai_chat
