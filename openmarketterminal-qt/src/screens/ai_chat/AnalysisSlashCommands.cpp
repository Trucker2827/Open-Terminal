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
