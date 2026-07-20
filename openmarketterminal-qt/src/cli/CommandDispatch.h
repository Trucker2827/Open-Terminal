#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace openmarketterminal::cli {

struct GlobalOpts {
    bool json = false;
    bool headless = false; // run commands in-process via HeadlessRuntime (no GUI)
    bool help = false;
    QString profile = "default";
};

// Parse [--json] [--profile X] off the FRONT of args; mutates args to the
// remaining <group> <command> [params]. Returns false on a bad --profile.
bool parse_global_opts(QStringList& args, GlobalOpts& out);

// Entry: returns a process exit code (see spec §4). Prints data to stdout,
// diagnostics to stderr.
int dispatch(QStringList args);

// Pure (no DB) construction of `kalshi auto advise open`'s response, given an
// already-fetched decision snapshot (kalshi_auto_current_snapshot()'s
// shape). Exposed here (non-static, declared) so it can be unit-tested
// directly for the blind-context firewall invariant, without process/stdout
// capture. On success returns {ticker, context, context_hash,
// prediction_ttl_ms, execution_relevance_ms, ts_opened, PRICE_WITHHELD:true}
// where `context` is exactly openmarketterminal::adv::build_blind_packet()'s
// output -- it must never contain any of adv::kBlindForbiddenKeys(). On a
// too-close-to-settlement rejection (adv::ttl_for().may_open == false),
// returns {"error": "settlement too near — do not open"}.
QJsonObject build_advise_open(const QJsonObject& snapshot, const QString& ticker, qint64 now_ms);

// Release headless resources before QCoreApplication is destroyed.
void shutdown();

} // namespace openmarketterminal::cli
