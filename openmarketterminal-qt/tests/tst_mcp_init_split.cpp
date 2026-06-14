// tst_mcp_init_split.cpp — verifies the McpInit core/gui registration split.
// After register_core_tools(), a data tool (get_quote) must be registered and a
// GUI-only tool (navigate_to_tab) must NOT be — it only appears once
// register_gui_tools() runs (called separately by the GUI app).

#include <QtTest>

#include "mcp/McpInit.h"
#include "mcp/McpProvider.h"

using namespace openmarketterminal::mcp;

class TstMcpInitSplit : public QObject {
    Q_OBJECT
  private slots:
    void core_registers_data_tools_not_gui() {
        register_core_tools();
        auto& p = McpProvider::instance();
        QVERIFY(p.has_tool("get_quote"));         // data tool present in core set
        QVERIFY(!p.has_tool("navigate_to_tab"));  // GUI tool absent until register_gui_tools()
    }
};

QTEST_MAIN(TstMcpInitSplit)
#include "tst_mcp_init_split.moc"
