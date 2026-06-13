// src/services/workflow/WorkflowHonestySelftest.cpp
#include "services/workflow/WorkflowHonestySelftest.h"

#include "services/workflow/NodeRegistry.h"
#include "services/workflow/WorkflowExecutor.h"
#include "screens/node_editor/NodeEditorTypes.h"
#include "core/result/Outcome.h"

#include <cstdio>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

namespace openmarketterminal::workflow {

namespace {

// Run a single-node workflow of the given registered type; return its result.
// success_out=false if the node never reported (timeout).
NodeExecutionResult run_one(const QString& type_id, bool& reported) {
    WorkflowDef wf;
    wf.id = "honesty-wf";
    NodeDef n;
    n.id = "n1";
    n.type = type_id;
    n.name = "n1";
    wf.nodes.push_back(n);

    WorkflowExecutor exec;
    NodeExecutionResult captured;
    reported = false;

    QObject::connect(&exec, &WorkflowExecutor::node_completed,
                     [&](const QString& id, const NodeExecutionResult& r) {
                         if (id == "n1") { captured = r; reported = true; }
                     });

    QEventLoop loop;
    QObject::connect(&exec, &WorkflowExecutor::execution_finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit); // safety timeout
    exec.execute(wf);
    loop.exec();
    return captured;
}

int fail(const char* msg) { std::fprintf(stderr, "[honesty-selftest] FAIL: %s\n", msg); return 1; }

} // namespace

int run_workflow_honesty_selftest() {
    auto& reg = NodeRegistry::instance();

    // (a) Unwired node type: execute is null.
    NodeTypeDef unwired;
    unwired.type_id = "test.unwired";
    unwired.display_name = "Test Unwired";
    unwired.category = "Test";
    // execute intentionally left null; available defaults true.
    reg.register_type(unwired);

    // (b) Explicitly unavailable node type.
    NodeTypeDef unavail;
    unavail.type_id = "test.unavailable";
    unavail.display_name = "Test Unavailable";
    unavail.category = "Test";
    unavail.available = false;
    unavail.unavailable_reason = "test.unavailable is not implemented yet. No work was performed.";
    unavail.execute = [](const QJsonObject&, const QVector<QJsonValue>&,
                         std::function<void(bool, QJsonValue, QString)> cb) {
        cb(true, QJsonObject{{"ran", true}}, {}); // MUST never be called (available=false)
    };
    reg.register_type(unavail);

    bool reported = false;

    auto r1 = run_one("test.unwired", reported);
    if (!reported) return fail("unwired node never reported a result");
    if (r1.success) return fail("unwired node reported success=true (silent passthrough still present)");
    if (r1.kind != OutcomeKind::Failed) return fail("unwired node kind != Failed");

    auto r2 = run_one("test.unavailable", reported);
    if (!reported) return fail("unavailable node never reported a result");
    if (r2.success) return fail("unavailable node reported success=true");
    if (r2.kind != OutcomeKind::Unavailable) return fail("unavailable node kind != Unavailable");
    if (!r2.output.toObject().isEmpty()) return fail("unavailable node produced output (execute was called)");

    std::fprintf(stdout, "[honesty-selftest] PASS\n");
    return 0;
}

} // namespace openmarketterminal::workflow
