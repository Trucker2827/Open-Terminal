#pragma once

#include <QMutex>
#include <QString>

#include <functional>

namespace openmarketterminal::mcp {

/// Per-action approval gate for destructive AI/MCP tool calls (live orders,
/// code execution, config writes). The MCP auth checker calls request() — on a
/// worker thread — which marshals to the main thread, shows the installed
/// modal presenter, and blocks until the user answers.
///
/// FAIL-CLOSED is the contract: request() returns true ONLY on an explicit user
/// approval. It returns false (deny) on ANYTHING else — no presenter installed,
/// no QApplication / can't marshal to the main thread, the dialog dismissed via
/// Esc / window-close / focus-loss, or any exception. This keeps the deny-by-
/// default floor in AgentService intact: if this gate is absent or broken in the
/// field, the AI simply cannot perform the action — it can never act UN-approved.
///
/// Requests are serialized (one prompt at a time) so concurrent tool calls queue
/// instead of racing two modals.
class ToolConfirmationGate {
  public:
    static ToolConfirmationGate& instance();

    /// The presenter is invoked on the MAIN thread and must return true ONLY for
    /// an explicit approval (false for deny / dismiss / close). Installed once by
    /// the UI layer at startup. Until then, request() denies (fail-closed).
    using Presenter = std::function<bool(const QString& title, const QString& detail)>;
    void set_presenter(Presenter presenter);

    /// Ask the user to approve an action. Safe to call from any thread; blocks
    /// the caller until the user answers (or denies immediately on any failure).
    bool request(const QString& title, const QString& detail);

  private:
    ToolConfirmationGate() = default;

    QMutex serialize_mutex_;        // one prompt at a time
    QMutex presenter_mutex_;        // guards presenter_
    Presenter presenter_;
};

}  // namespace openmarketterminal::mcp
