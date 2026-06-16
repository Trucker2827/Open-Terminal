#include "cli/ObserveCommand.h"
#include "services/observer/ObserverJournalService.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <cstdio>

namespace openmarketterminal::cli {

using services::ObserverJournalService;

static void print_doc(const QJsonDocument& doc, bool compact) {
    std::printf("%s\n", doc.toJson(compact ? QJsonDocument::Compact : QJsonDocument::Indented).constData());
}

static int not_available(const ObserverJournalService& svc) {
    std::fprintf(stderr,
                 "observer journal not found at %s\n"
                 "set OPENTERMINAL_OBSERVER_DIR to the trading-mcp-server directory\n",
                 qUtf8Printable(svc.journalDir()));
    return 6;
}

static int cmd_latest(const GlobalOpts& opts, const ObserverJournalService& svc) {
    const auto b = svc.latestDaily();
    if (opts.json) {
        QJsonObject o;
        o["title"]    = b ? QJsonValue(b->title) : QJsonValue();
        o["markdown"] = b ? QJsonValue(b->markdown) : QJsonValue();
        o["alert"]    = (b && !b->alert.isEmpty()) ? QJsonValue(b->alert) : QJsonValue();
        const QJsonArray h = svc.history(1);
        o["history"]  = h.isEmpty() ? QJsonValue() : h.last();   // matching machine-readable record
        print_doc(QJsonDocument(o), true);
        return 0;
    }
    std::printf("%s\n", b ? qUtf8Printable(b->markdown) : "no daily observations yet");
    return 0;
}

static int cmd_week(const GlobalOpts& opts, const ObserverJournalService& svc) {
    const auto b = svc.latestWeekly();
    if (opts.json) {
        QJsonObject o;
        o["title"]    = b ? QJsonValue(b->title) : QJsonValue();
        o["markdown"] = b ? QJsonValue(b->markdown) : QJsonValue();
        print_doc(QJsonDocument(o), true);
        return 0;
    }
    std::printf("%s\n", b ? qUtf8Printable(b->markdown) : "no weekly review yet");
    return 0;
}

static int cmd_alerts(const GlobalOpts& opts, const ObserverJournalService& svc, int limit) {
    const auto alerts = svc.recentAlerts(limit);
    if (opts.json) {
        QJsonArray arr;
        for (const auto& b : alerts)
            arr.append(QJsonObject{{"title", b.title}, {"alert", b.alert}, {"markdown", b.markdown}});
        print_doc(QJsonDocument(arr), true);
        return 0;
    }
    if (alerts.isEmpty()) { std::printf("no alerts recorded\n"); return 0; }
    for (const auto& b : alerts)
        std::printf("%s — %s\n", qUtf8Printable(b.title), qUtf8Printable(b.alert));
    return 0;
}

int observe_command(const GlobalOpts& opts, QStringList args) {
    const QString sub = args.isEmpty() ? QString() : args.takeFirst();
    if (sub.isEmpty()) { std::fprintf(stderr, "usage: observe latest|week|alerts [N]\n"); return 2; }

    auto& svc = ObserverJournalService::instance();
    if (!svc.available()) return not_available(svc);

    if (sub == "latest") return cmd_latest(opts, svc);
    if (sub == "week")   return cmd_week(opts, svc);
    if (sub == "alerts") {
        int limit = 10;
        if (!args.isEmpty()) {
            bool ok = false;
            const int n = args.first().toInt(&ok);
            if (ok && n > 0) limit = n;
        }
        return cmd_alerts(opts, svc, limit);
    }
    std::fprintf(stderr, "usage: observe latest|week|alerts [N]\n");
    return 2;
}

} // namespace openmarketterminal::cli
