#!/bin/bash
# sandbox_boundary_check.sh — Strategy Sandbox Task 11 power-boundary check.
#
# REPORT-ONLY INVARIANT (see SandboxEligibility.h's file header, and the
# Task 11 brief): the Strategy Sandbox (PaperExecutor, PaperFillModel,
# SandboxResolver, SandboxScorer, SandboxEligibility, SandboxRegistry,
# TickTail) must never link against — must never even reference — the live
# order-placement surface. This script proves that at the OBJECT-FILE level,
# after the fact, rather than trusting a #include audit: it finds each
# compiled .o for the sandbox service sources and greps their UNDEFINED
# symbol table (nm -u, demangled via c++filt) for ExchangeService,
# place_exchange_order, crypto_submit_order, or place_order. Any hit means
# some sandbox source pulled in a live execution path and this must FAIL
# LOUDLY — the sandbox is a read/paper-only surface, never a route to a real
# order.
#
# Deliberately fails loudly (exit 1) if ZERO object files are found, rather
# than silently passing — a build-layout change that moves/renames these
# objects must not make this check quietly vacuous.
#
# Invoked by ctest (tests/CMakeLists.txt) with BUILD_DIR set via
# `cmake -E env` to the build tree root, since ctest's working directory is
# not guaranteed to be the build dir.

set -uo pipefail

if [ -z "${BUILD_DIR:-}" ]; then
    echo "FAIL: BUILD_DIR is not set (expected the CMake build tree root)"
    exit 1
fi

OBJ_DIR="$BUILD_DIR/CMakeFiles/openterminal_core.dir/src/services/sandbox"

if [ ! -d "$OBJ_DIR" ]; then
    echo "FAIL: sandbox object directory not found: $OBJ_DIR"
    exit 1
fi

command -v nm >/dev/null 2>&1 || { echo "FAIL: nm not found on PATH"; exit 1; }
command -v c++filt >/dev/null 2>&1 || { echo "FAIL: c++filt not found on PATH"; exit 1; }

# Sandbox service sources whose compiled objects must never reference the
# live order-placement surface (see file header).
SOURCES=(PaperExecutor.cpp PaperFillModel.cpp SandboxResolver.cpp SandboxScorer.cpp
         SandboxEligibility.cpp SandboxRegistry.cpp TickTail.cpp)

FORBIDDEN_RE='ExchangeService|place_exchange_order|crypto_submit_order|place_order'

checked=0
violated=0

for src in "${SOURCES[@]}"; do
    # find, not a hardcoded path suffix: tolerates minor CMake object-layout
    # differences (e.g. a unity-build or generator change) while still
    # anchoring to this source file's own object.
    while IFS= read -r obj; do
        [ -z "$obj" ] && continue
        checked=$((checked + 1))

        undefined_syms="$(nm -u "$obj" 2>/dev/null | c++filt || true)"
        hit="$(printf '%s\n' "$undefined_syms" | grep -E "$FORBIDDEN_RE" || true)"
        if [ -n "$hit" ]; then
            echo "FAIL: power-boundary violation — $obj references forbidden symbol(s):"
            printf '%s\n' "$hit"
            violated=1
        fi
    done < <(find "$OBJ_DIR" -type f -name "${src}.o")
done

if [ "$checked" -eq 0 ]; then
    echo "FAIL: zero sandbox object files found under $OBJ_DIR for: ${SOURCES[*]}"
    echo "      (this check must never pass vacuously — rebuild openterminal_core first)"
    exit 1
fi

if [ "$violated" -ne 0 ]; then
    exit 1
fi

echo "PASS: checked $checked sandbox object file(s) under $OBJ_DIR — no forbidden symbol references"
exit 0
