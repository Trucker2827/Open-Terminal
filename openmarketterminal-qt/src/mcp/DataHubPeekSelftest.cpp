#include "mcp/DataHubPeekSelftest.h"

#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "mcp/tools/DataHubPeekHelpers.h"
#include "services/markets/MarketDataService.h"

#include <QCoreApplication>
#include <QVariant>

#include <cstdio>
#include <memory>

namespace openmarketterminal::mcp {

namespace {

bool check(const char* label, bool ok, int& failures) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (!ok)
        ++failures;
    return ok;
}

} // namespace

int run_datahub_peek_selftest() {
    int failures = 0;
    std::unique_ptr<QCoreApplication> owned_app;
    if (!QCoreApplication::instance()) {
        int argc = 0;
        char* argv[] = {nullptr};
        owned_app = std::make_unique<QCoreApplication>(argc, argv);
    }
    openmarketterminal::datahub::register_metatypes();

    auto& hub = openmarketterminal::datahub::DataHub::instance();

    openmarketterminal::services::QuoteData q;
    q.symbol = QStringLiteral("AAPL");
    q.name = QStringLiteral("Apple Inc.");
    q.price = 190.25;
    q.change = 1.5;
    q.change_pct = 0.79;
    q.high = 191.0;
    q.low = 188.5;
    q.volume = 42'000'000;

    hub.publish(QStringLiteral("market:quote:AAPL"), QVariant::fromValue(q));

    const auto peeked = openmarketterminal::mcp::tools::detail::peek_quote(QStringLiteral("AAPL"));
    check("peek_quote returns published AAPL", peeked.has_value() && peeked->price == 190.25, failures);

    const QJsonObject json = openmarketterminal::mcp::tools::detail::quote_to_json(*peeked);
    check("quote_to_json symbol", json.value(QStringLiteral("symbol")).toString() == QStringLiteral("AAPL"),
          failures);

    const qint64 age = openmarketterminal::mcp::tools::detail::topic_age_ms(QStringLiteral("market:quote:AAPL"));
    check("topic_age_ms fresh", age >= 0 && age < 5000, failures);

    const QString brief = openmarketterminal::mcp::tools::detail::build_screen_context_brief();
    check("build_screen_context_brief empty without subscribers", brief.isEmpty(), failures);

    const QJsonObject ctx = openmarketterminal::mcp::tools::detail::build_terminal_context_json();
    check("build_terminal_context_json has active_topics",
          ctx.value(QStringLiteral("active_topics")).toArray().size() >= 1, failures);

    check("peek_quote miss for unknown symbol",
          !openmarketterminal::mcp::tools::detail::peek_quote(QStringLiteral("ZZZZZZ")).has_value(), failures);

    return failures == 0 ? 0 : 1;
}

} // namespace openmarketterminal::mcp
