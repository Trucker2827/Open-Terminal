// RegisterAllMigrations.cpp — single source of truth for the ordered list of
// schema migrations.
//
// Both the GUI (main.cpp) and the headless host (HeadlessRuntime) must register
// the exact same migrations, in the exact same order, BEFORE Database::open()
// runs them — otherwise the headless DB comes up with no app schema (the bug
// this consolidation fixes). Keeping the list in one function makes drift
// impossible: there is only one place to edit when a vNNN migration is added.

#include "storage/sqlite/migrations/MigrationRunner.h"

namespace openmarketterminal {

// Register migrations explicitly (avoids MSVC /OPT:REF stripping static-init TUs).
void register_all_migrations() {
    register_migration_v001();
    register_migration_v002();
    register_migration_v003();
    register_migration_v004();
    register_migration_v005();
    register_migration_v006();
    register_migration_v007();
    register_migration_v008();
    register_migration_v009();
    register_migration_v010();
    register_migration_v011();
    register_migration_v012();
    register_migration_v013();
    register_migration_v014();
    register_migration_v015();
    register_migration_v016();
    register_migration_v017();
    register_migration_v018();
    register_migration_v019();
    register_migration_v020();
    register_migration_v021();
    register_migration_v022();
    register_migration_v023();
    register_migration_v024();
    register_migration_v025();
    register_migration_v026();
    register_migration_v028();
    register_migration_v029();
    register_migration_v030();
    register_migration_v031();
    register_migration_v032();
    register_migration_v033();
    register_migration_v034();
    register_migration_v035();
    register_migration_v036();
    register_migration_v038();
    register_migration_v039();
    register_migration_v040();
    register_migration_v041();
    register_migration_v042();
    register_migration_v043();
    register_migration_v044();
    register_migration_v045();
    register_migration_v046();
    register_migration_v047();
    register_migration_v048();
    register_migration_v049();
    register_migration_v050();
    register_migration_v051();
    register_migration_v052();
    register_migration_v053();
    register_migration_v054();
    register_migration_v055();
    register_migration_v056();
    register_migration_v057();
    register_migration_v058();
    register_migration_v059();
    register_migration_v060();
    register_migration_v061();
}

} // namespace openmarketterminal
