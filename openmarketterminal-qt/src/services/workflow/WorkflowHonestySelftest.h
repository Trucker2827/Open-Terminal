// src/services/workflow/WorkflowHonestySelftest.h
#pragma once
namespace openmarketterminal::workflow {
// Returns 0 on pass, non-zero on failure. Headless (needs a QCoreApplication).
int run_workflow_honesty_selftest();
}
