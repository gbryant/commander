#!/bin/bash
# Host tests for the portable IR decoder (modules/ir/). Pure C++, no hardware.
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CXX="${CXX:-g++}"
TMP="$(mktemp -d)"

echo "== NecDecoder =="
$CXX -std=c++17 -Wall -Wextra -I"$ROOT" \
    "$ROOT/modules/ir/tests/test_nec.cpp" -o "$TMP/test_nec"
"$TMP/test_nec"

rm -rf "$TMP"
