#!/usr/bin/env bash
# Compile and run the self-contained host C regression harness. No GStreamer SDK is required.
# This lane intentionally did NOT execute it; the command is recorded for the next host gate.
set -euo pipefail
cd "$(dirname "$0")/.."
BIN="${TMPDIR:-/tmp/opencode}/nj-pcmhd-sink-test"
if [ "$(uname -s)" != Linux ]; then
  echo "this link-wrap harness currently requires Linux ld" >&2
  exit 2
fi
cc -std=c11 -O2 -Wall -Wextra -Werror \
  -Wl,--wrap=dlopen -Wl,--wrap=dlsym \
  ci/pcmhd-sink-test.c -lpthread -ldl -o "$BIN"
"$BIN"
