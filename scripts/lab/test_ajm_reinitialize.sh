#!/bin/bash
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
test_dir="$(mktemp -d "${TMPDIR:-/tmp}/gow3-ajm-reinitialize.XXXXXX")"
trap 'python3 -c '\''import shutil,sys; shutil.rmtree(sys.argv[1], ignore_errors=True)'\'' "$test_dir"' EXIT

clang -std=c11 -O2 -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections \
    -I"$REPO/3rdparty/libatrac9/C/src" \
    -I"$REPO/3rdparty/minimp3" \
    -I"$REPO/runtime/core" \
    -I"$REPO/runtime/modules/libSceAjm" \
    "$REPO/tests/test_ajm_reinitialize.c" \
    -Wl,-dead_strip -lpthread -lm \
    -o "$test_dir/ajm-reinitialize-test"

"$test_dir/ajm-reinitialize-test"
