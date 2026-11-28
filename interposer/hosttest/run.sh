#!/bin/sh
# Host test of the interposer MECHANISM (any host architecture): the caller and the stock definition
# are in separate shared objects, so `mock_query` reaches `isPlatformSupportSoftwareAudioDecoder`
# through an interposable JUMP_SLOT relocation, as libplayerAPIs does. It does NOT prove the ARM
# build or the TV's libstdc++ ABI.
set -eu
cd "$(dirname "$0")"

CXX=${CXX:-g++}
ABI=-D_GLIBCXX_USE_CXX11_ABI=1
SYMBOL='_ZN3smp4util37isPlatformSupportSoftwareAudioDecoderENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE'
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/platformfix-hosttest.XXXXXX")
trap 'rm -rf "$SCRATCH"' EXIT HUP INT TERM

# Separate DSOs are deliberate: the caller's reference to the stock symbol must stay undefined so
# that a preloaded definition can win. One DSO holding both would bind the call locally.
$CXX -O2 -g -Wall -Wextra -fPIC $ABI -shared -o "$SCRATCH/libmockcaller.so" mock_caller.cpp
$CXX -O2 -g -Wall -Wextra -fPIC $ABI -shared -o "$SCRATCH/libmockstock.so" mock_stock.cpp
$CXX -O2 -g -Wall -Wextra -fPIC $ABI -shared -o "$SCRATCH/libplatformfix.so" ../platformfix.cpp -ldl
# Keep both DT_NEEDED entries even under the linker's default --as-needed: the stock DSO is needed
# by the caller DSO, not directly by this executable.
$CXX -O2 -g -Wall -Wextra $ABI -o "$SCRATCH/test" test.cpp -L"$SCRATCH" \
  -Wl,--no-as-needed -lmockcaller -lmockstock -Wl,--as-needed -Wl,-rpath,'$ORIGIN'

echo
echo "== call site is interposable (readelf mechanism check)"
# AArch64 names it R_AARCH64_JUMP_SLOT, x86-64 R_X86_64_JUMP_SLOT; both mean a PLT slot the loader
# can bind to a preloaded definition. Disassembly suffixes are not portable across host toolchains.
# mock_caller.cpp defines only mock_query, so this is its call site.
CALLER_DYNSYMS=$(readelf -W --dyn-syms "$SCRATCH/libmockcaller.so")
ROWS=$(printf '%s\n' "$CALLER_DYNSYMS" | awk -v symbol="$SYMBOL" '$NF == symbol { print }')
[ "$(printf '%s\n' "$ROWS" | wc -l | tr -d ' ')" -eq 1 ] || {
  echo "FAIL: interposed symbol is not present exactly once in the caller's .dynsym" >&2
  exit 1
}
printf '%s\n' "$ROWS" | awk '$4 == "FUNC" && $5 == "GLOBAL" && $6 == "DEFAULT" && $7 == "UND" { ok=1 } END { exit !ok }' || {
  echo "FAIL: interposed symbol is not an undefined GLOBAL/DEFAULT FUNC in the caller's .dynsym" >&2
  exit 1
}
printf '%s\n' "$ROWS"

CALLER_RELOCS=$(readelf -W --relocs "$SCRATCH/libmockcaller.so")
if ! JUMP=$(printf '%s\n' "$CALLER_RELOCS" | awk -v symbol="$SYMBOL" '
  $3 == "R_X86_64_JUMP_SLOT" || $3 == "R_AARCH64_JUMP_SLOT" {
    for (i = 4; i <= NF; i++) if ($i == symbol) { print; found=1 }
  }
  END { exit !found }
'); then
  echo "FAIL: mock_query has no JUMP_SLOT relocation for the interposed symbol" >&2
  exit 1
fi
printf '%s\n' "$JUMP"

echo
echo "== stock: no preload (the bug: o24n refused)"
(
  unset NJ_PLATFORMFIX_ACTIVE NJ_PLATFORMFIX
  LD_PRELOAD=
  export LD_PRELOAD
  "$SCRATCH/test"
)

echo
echo "== preload WITHOUT the re-exec marker (must stay inert)"
(
  unset NJ_PLATFORMFIX_ACTIVE NJ_PLATFORMFIX
  LD_PRELOAD="$SCRATCH/libplatformfix.so"
  export LD_PRELOAD
  "$SCRATCH/test"
)

# Rust and C++ both define the sentinel as the exact value `1`, not merely a present variable.
for marker in '' 0 1; do
  echo
  echo "== preload with NJ_PLATFORMFIX_ACTIVE='$marker' (only 1 may override)"
  (
    unset NJ_PLATFORMFIX
    NJ_PLATFORMFIX_ACTIVE="$marker"
    LD_PRELOAD="$SCRATCH/libplatformfix.so"
    export NJ_PLATFORMFIX_ACTIVE LD_PRELOAD
    "$SCRATCH/test"
  )
done

echo
echo "== preload + marker + kill switch (must delegate stock)"
(
  NJ_PLATFORMFIX_ACTIVE=1
  NJ_PLATFORMFIX=0
  LD_PRELOAD="$SCRATCH/libplatformfix.so"
  export NJ_PLATFORMFIX_ACTIVE NJ_PLATFORMFIX LD_PRELOAD
  "$SCRATCH/test"
)

echo
echo "== exported symbol"
DYNSYMS=$(readelf -W --dyn-syms "$SCRATCH/libplatformfix.so")
ROWS=$(printf '%s\n' "$DYNSYMS" | awk -v symbol="$SYMBOL" '$NF == symbol { print }')
[ "$(printf '%s\n' "$ROWS" | wc -l | tr -d ' ')" -eq 1 ] || {
  echo "FAIL: exact interposer symbol is not present once in .dynsym" >&2
  exit 1
}
printf '%s\n' "$ROWS" | awk '$4 == "FUNC" && $5 == "GLOBAL" && $6 == "DEFAULT" && $7 != "UND" { ok=1 } END { exit !ok }' || {
  echo "FAIL: interposer symbol is not a defined GLOBAL/DEFAULT FUNC" >&2
  exit 1
}
printf '%s\n' "$ROWS"
