// v048_scrub_hosted_llm_proxy — neutralizes the dead hosted "openmarketterminal"
// LLM proxy that migration v002 seeded as the active provider
// (base_url = https://api.example.com/research/llm, is_active = 1). That row is
// a metered/billed cloud-proxy artifact from the upstream product. At runtime
// LlmService already coerces provider=="openmarketterminal" to local Ollama, so
// no prompt can reach that endpoint — but the stale billed URL still sits in the
// DB and would route prompts if that coercion ever regressed. This forward
// migration blanks the URL and deactivates the row so the artifact can't be
// re-enabled, on existing installs as well as fresh ones.
//
// Scope: touches ONLY the 'openmarketterminal' hosted-proxy row. Any provider
// the user configured themselves (Ollama / OpenAI / Anthropic / …) is untouched.
// If this was the only active provider, the user simply picks one via the
// "Configure LLM" button (the assistant stays idle until then). Idempotent:
// the UPDATE is a no-op when the row is absent or already scrubbed.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace openmarketterminal {
namespace {

Result<void> apply_v048(QSqlDatabase& db) {
    QSqlQuery q(db);
    if (!q.exec("UPDATE llm_configs SET base_url = '', is_active = 0 "
                "WHERE provider = 'openmarketterminal'")) {
        const QString err = q.lastError().text();
        // A missing llm_configs table (shouldn't happen — v002 creates it) is
        // not fatal for this cleanup; only surface real SQL errors.
        if (!err.contains("no such table", Qt::CaseInsensitive))
            return Result<void>::err(err.toStdString());
    }
    return Result<void>::ok();
}

} // anonymous namespace

void register_migration_v048() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({48, "scrub_hosted_llm_proxy", apply_v048});
}

} // namespace openmarketterminal
