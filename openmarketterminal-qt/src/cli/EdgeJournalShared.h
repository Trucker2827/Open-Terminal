#pragma once
// Shared surface for edge-journal CLI commands compiled OUTSIDE the huge
// CommandDispatch.cpp translation unit. CommandDispatch.cpp (~27k lines)
// deterministically crashes the MSVC FRONT-END (C1001, msc1.cpp:1589) while
// parsing near edge_journal_rare_alerts_command — position-dependent, not
// content-dependent (survived brace-init removal and optimisation pragmas;
// recurred across v0.3.25/26/29). Splitting the function into its own small
// TU (EdgeJournalRareAlerts.cpp) is the structural fix; these declarations
// give the split TU access to CommandDispatch's helpers without duplicating
// them. Definitions stay in CommandDispatch.cpp (now external linkage).

#include "cli/CommandDispatch.h"
#include "services/notifications/NotificationService.h"

#include <QString>
#include <QStringList>

namespace openmarketterminal::cli {

struct EdgeCryptoTrust {
    QString symbol;
    QString horizon;
    int decisions = 0;
    int resolved = 0;
    int wins = 0;
    int buy_resolved = 0;
    int buy_wins = 0;
    int no_buy_resolved = 0;
    int no_buy_wins = 0;
    double avg_edge = 0.0;
    double avg_confidence = 0.0;
    double trust = 0.0;
    QString status;
};

bool take_bool_flag(QStringList& args, const QString& flag);
bool take_string_option(QStringList& args, const QString& flag, QString& out);
bool require_yes(QStringList& args, const char* usage_line);
const char* edge_journal_cols();
EdgeCryptoTrust edge_crypto_trust_for_symbol(const QString& symbol, const QString& horizon,
                                             int max_age_hours);
QString edge_time_text(qint64 ts_ms);
QString edge_pct(double v);
bool init_headless_for_cli(const GlobalOpts& opts, int& exit_code);
bool notify_wait_send(notifications::INotificationProvider* provider,
                      const notifications::NotificationRequest& req, int timeout_ms, bool& ok,
                      QString& error);

int edge_journal_rare_alerts_command(const GlobalOpts& opts, QStringList args);

} // namespace openmarketterminal::cli
