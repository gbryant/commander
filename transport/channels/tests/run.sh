#!/bin/bash
# Host tests for the channel bus (transport/channels/). Pure C++, no hardware.
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CXX="${CXX:-g++}"
TMP="$(mktemp -d)"

echo "== ChannelCodec =="
$CXX -std=c++17 -Wall -Wextra -I"$ROOT" \
    "$ROOT/transport/channels/tests/test_codec.cpp" -o "$TMP/test_codec"
"$TMP/test_codec"

echo
echo "== ChannelTransport =="
$CXX -std=c++17 -Wall -Wextra -DMAX_COMMANDS=16 -I"$ROOT" -I"$ROOT/include" \
    "$ROOT/transport/channels/tests/test_transport.cpp" \
    "$ROOT/core/CommandRegistry.cpp" -o "$TMP/test_transport"
"$TMP/test_transport"

echo
echo "== ChannelBusRunner =="
$CXX -std=c++17 -Wall -Wextra -DMAX_COMMANDS=16 -I"$ROOT" -I"$ROOT/include" \
    "$ROOT/transport/channels/tests/test_runner.cpp" \
    "$ROOT/transport/channels/ChannelBusRunner.cpp" \
    "$ROOT/core/CommandRegistry.cpp" -o "$TMP/test_runner"
"$TMP/test_runner"

rm -rf "$TMP"
