// tst_mcp_init_split.cpp — verifies the McpInit core/gui registration split.
// After register_core_tools(), a data tool (get_quote) must be registered and a
// GUI-only tool (navigate_to_tab) must NOT be — it only appears once
// register_gui_tools() runs (called separately by the GUI app).

#include <QtTest>

#include "mcp/McpInit.h"
#include "mcp/McpProvider.h"
#include "mcp/McpService.h"
#include "mcp/McpTypes.h"

using namespace openmarketterminal::mcp;

class TstMcpInitSplit : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { register_core_tools(); }

    void core_registers_data_tools_not_gui() {
        auto& p = McpProvider::instance();
        QVERIFY(p.has_tool("get_quote"));         // data tool present in core set
        QVERIFY(!p.has_tool("navigate_to_tab"));  // GUI tool absent until register_gui_tools()
    }

    // The default filter applies the kHardMaxTools=50 safety cap (right for a
    // token-limited LLM). ToolFilter::no_cap must bypass it so an external client
    // (the Claude Code MCP adapter via GET /tools?all=1) sees the full catalogue.
    void no_cap_filter_returns_full_catalog() {
        ToolFilter capped;                 // max_tools=0 → kHardMaxTools safety cap
        ToolFilter uncapped;
        uncapped.no_cap = true;
        const int n_capped = static_cast<int>(McpService::instance().get_all_tools(capped).size());
        const int n_uncapped = static_cast<int>(McpService::instance().get_all_tools(uncapped).size());
        QCOMPARE(n_capped, 50);            // safety cap holds for the default path
        QVERIFY2(n_uncapped > 50,          // no_cap returns the whole registry
                 qPrintable(QString("expected full catalog > 50, got %1").arg(n_uncapped)));
    }
};

QTEST_MAIN(TstMcpInitSplit)
#include "tst_mcp_init_split.moc"
