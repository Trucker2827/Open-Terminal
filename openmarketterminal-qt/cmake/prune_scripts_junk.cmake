# Prune files/directories that accidentally live under scripts/ but must
# never ship inside the built bundle. Runs under `cmake -P` — portable
# across Windows / macOS / Linux.
#
# Rationale:
#   - *.deleted.* / *.bak / *.orig  — leftover backups from refactors
#   - .pytest_cache / .benchmarks   — pytest caches committed by mistake
#   - __pycache__                   — Python bytecode caches
#   - *.db / *.sqlite*              — local SQLite DBs (e.g. memories_alice.db)
#
# On macOS anything unexpected inside an .app bundle trips codesign with
# "bundle format unrecognized, invalid, or unsuitable". On Linux/Windows
# these just bloat the installer; removing them shrinks it meaningfully.

if(NOT DEFINED SCRIPTS_DIR)
    message(FATAL_ERROR "prune_scripts_junk.cmake: SCRIPTS_DIR must be set")
endif()

if(NOT IS_DIRECTORY "${SCRIPTS_DIR}")
    # Nothing to do — scripts dir isn't there.
    return()
endif()

# Glob directories to nuke (recursively under scripts/).
file(GLOB_RECURSE _junk_dirs
    LIST_DIRECTORIES true
    "${SCRIPTS_DIR}/*.deleted.*"
    "${SCRIPTS_DIR}/*.deleted"
    "${SCRIPTS_DIR}/*/.pytest_cache"
    "${SCRIPTS_DIR}/*/.benchmarks"
    "${SCRIPTS_DIR}/*/__pycache__"
    "${SCRIPTS_DIR}/.pytest_cache"
    "${SCRIPTS_DIR}/.benchmarks"
    "${SCRIPTS_DIR}/__pycache__"
)

# Glob stray files (local DBs + editor/backup artifacts).
file(GLOB_RECURSE _junk_files
    "${SCRIPTS_DIR}/*.db"
    "${SCRIPTS_DIR}/*.sqlite"
    "${SCRIPTS_DIR}/*.sqlite3"
    "${SCRIPTS_DIR}/*.bak"
    "${SCRIPTS_DIR}/*.orig"
    "${SCRIPTS_DIR}/*.pyc"
)

set(_removed 0)
foreach(_path IN LISTS _junk_dirs _junk_files)
    get_filename_component(_name "${_path}" NAME)
    if(IS_DIRECTORY "${_path}")
        if(_name STREQUAL "__pycache__" OR
           _name STREQUAL ".pytest_cache" OR
           _name STREQUAL ".benchmarks" OR
           _name MATCHES "\\.deleted(\\.|$)")
            file(REMOVE_RECURSE "${_path}")
            math(EXPR _removed "${_removed} + 1")
        endif()
    elseif(EXISTS "${_path}")
        if(_name MATCHES "\\.(db|sqlite|sqlite3|bak|orig|pyc)$")
            file(REMOVE "${_path}")
            math(EXPR _removed "${_removed} + 1")
        endif()
    endif()
endforeach()

if(_removed GREATER 0)
    message(STATUS "prune_scripts_junk: removed ${_removed} junk path(s) under ${SCRIPTS_DIR}")
endif()
