// tst_local_essentials.cpp — Unit test for mcp::local_essentials_tool_names().
//
// Verifies that the curated local-model toolset contains all key live-data
// tools and stays within the "curated, not the full 600+" size bounds.
// TDD step 2: this test FAILS before the set is implemented (undefined symbol).
// TDD step 4: neuter by removing web_search → RED; revert → GREEN.

#include <QtTest>

#include "mcp/McpProvider.h"

class TstLocalEssentials : public QObject {
    Q_OBJECT
private slots:
    void contains_key_live_tools() {
        const auto& s = openmarketterminal::mcp::local_essentials_tool_names();
        for (const char* n : {"get_quote", "get_equity_news", "web_search", "web_fetch", "tool_list"})
            QVERIFY2(s.contains(n), n);
        QVERIFY(s.size() >= 12 && s.size() <= 30); // curated, not all 605
    }
};

QTEST_MAIN(TstLocalEssentials)
#include "tst_local_essentials.moc"
