#include "screens/observer/ObserverJournalView.h"
#include "services/observer/ObserverJournalService.h"

namespace openmarketterminal::screens::observer {

QString markdown_for(JournalView view, const services::ObserverJournalService& svc) {
    if (!svc.available()) {
        return "## Observer journal not found\n\nNo journal at `" + svc.journalDir() +
               "`.\n\nSet `OPENTERMINAL_OBSERVER_DIR` to the trading-mcp-server directory, "
               "or let the daily observer run.";
    }
    switch (view) {
        case JournalView::Latest: {
            const auto b = svc.latestDaily();
            return b ? b->markdown : QStringLiteral("_No daily observations yet._");
        }
        case JournalView::Weekly: {
            const auto b = svc.latestWeekly();
            return b ? b->markdown : QStringLiteral("_No weekly review yet._");
        }
        case JournalView::Alerts: {
            const auto alerts = svc.recentAlerts(20);
            if (alerts.isEmpty()) return QStringLiteral("_No alerts recorded._");
            QString md = QStringLiteral("# Recent alerts\n\n");
            for (const auto& a : alerts)
                md += "- **" + a.title + "** — " + a.alert + "\n";
            return md;
        }
    }
    return QString();
}

} // namespace openmarketterminal::screens::observer
