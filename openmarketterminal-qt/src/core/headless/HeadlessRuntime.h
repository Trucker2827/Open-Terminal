#pragma once

// HeadlessRuntime — in-process bring-up of the terminal "brain" with no GUI.
//
// Lives in openterminal_core (Qt6::Core/Network/Sql only — no QtWidgets). It
// replicates the headless-relevant subset of the GUI's main.cpp startup
// sequence (DB + migrations → SecureStorage → cache DB → DataHub default hook →
// register_core_tools) under an already-running QCoreApplication, then lets the
// caller dispatch core MCP tools synchronously. Used by openterminalcli
// --headless (Task 5) and any other non-GUI host.

#include "mcp/McpTypes.h"

#include <QJsonObject>
#include <QString>

namespace openmarketterminal::headless {

struct InitResult {
    bool ok = false;
    QString error;
};

class HeadlessRuntime {
  public:
    /// Brings up DB (+migrations) → SecureStorage → cache DB → DataHub default
    /// owner-active hook → register_core_tools() → deny-all gate presenter.
    /// `profile` selects the datadir (same rule as the GUI). A QCoreApplication
    /// must already exist. Idempotent: a second call is a no-op success.
    InitResult init(const QString& profile);

    /// Dispatch one core tool synchronously (pumps the event loop until the
    /// async future completes).
    mcp::ToolResult call_tool(const QString& name, const QJsonObject& args);

    /// One-shot teardown. Singletons own their own lifetime; nothing to do.
    void shutdown();

  private:
    bool initialized_ = false;
};

}  // namespace openmarketterminal::headless
