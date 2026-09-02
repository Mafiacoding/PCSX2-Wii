#!/bin/sh
# Round 782 (task #807): replaces tests/README.md's 134 individually
# hand-maintained "gcc ... -o test_x tests/test_x.c ../source/hw/a.c
# ../source/hw/b.c ..." compile commands, which this project's own
# STATUS.md has now documented going stale FOUR separate times (Round
# 605, Round 776b, Round 781, Round 782) as source/ grew and files moved
# out of the old flat source/hw/ layout into source/core/ee/,
# source/core/iop/, source/hw/, etc. Every prior "fix" just re-typed a
# fresh snapshot of the correct file list, which is exactly why it kept
# going stale again - a hand-copied list is a fork of the truth, not a
# derivation from it.
#
# This script instead derives the correct link line at run time, every
# time, from the live source/ tree - the same glob-based approach this
# project's own scratch tooling (tools/round729-gt3-discboot/*, this
# round's r806_* builds, etc.) has used successfully for many rounds:
#
#   find source -name '*.c' ! -path '*recompiler*' ! -name 'main.c'
#
# ...with one refinement: some test_*.c files #include their target
# core/*.c file DIRECTLY (e.g. test_ee_core.c has
# `#include "core/ee/ee_core.c"`), so linking that same .c file's .o
# again would duplicate-symbol-error. Rather than hand-maintaining WHICH
# tests do this (the exact staleness trap this script exists to avoid),
# this script grep's each test file for that pattern itself and excludes
# whatever it finds, live.
#
# Usage:
#   tests/run_test.sh test_ee_core            # builds + runs
#   tests/run_test.sh test_ee_core --build-only
#   tests/run_test.sh --all                    # builds + runs every test_*.c
#
# Run from the repository root (the directory containing include/ and
# source/).

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

build_and_run() {
    name="$1"
    build_only="$2"
    src="tests/${name}.c"
    if [ ! -f "$src" ]; then
        echo "run_test.sh: no such test source: $src" >&2
        return 1
    fi

    # Live-detect any source/*.c this test #includes directly (either the
    # "core/..." or "hw/..." prefix - both are real subtrees under
    # source/), and exclude every exact file it names from the glob
    # (duplicate-symbol guard). A test can self-include more than one
    # file (e.g. test_dma_gif_demo.c pulls in hw/dma.c, hw/gif.c, and
    # hw/gs_mem.c all directly), so collect all matches, not just the
    # first.
    self_incs=$(grep -oE '#include "(core|hw)/[a-zA-Z0-9_/]+\.c"' "$src" | sed -E 's/#include "(.*)"/\1/' | sort -u)

    exclude_args="! -path '*recompiler*' ! -name 'main.c'"
    if [ -n "$self_incs" ]; then
        for inc in $self_incs; do
            exclude_args="$exclude_args ! -name '$(basename "$inc")'"
        done
    fi

    out="/tmp/${name}"
    # shellcheck disable=SC2086
    sources=$(eval find source -name "'*.c'" $exclude_args)

    self_incs_display=$(echo "$self_incs" | tr '\n' ' ')
    echo "[run_test.sh] compiling $name (self-include exclude: ${self_incs_display:-none})"
    gcc -O2 -w -Iinclude -Isource "$src" $sources -o "$out" -lm

    if [ "$build_only" != "--build-only" ]; then
        echo "[run_test.sh] running $name"
        "$out"
    fi
}

if [ "$1" = "--all" ]; then
    fail=0
    for f in tests/test_*.c; do
        name=$(basename "$f" .c)
        if ! build_and_run "$name" ""; then
            fail=1
        fi
    done
    exit $fail
fi

if [ -z "$1" ]; then
    echo "usage: $0 <test_name|--all> [--build-only]" >&2
    exit 1
fi

build_and_run "$1" "$2"
