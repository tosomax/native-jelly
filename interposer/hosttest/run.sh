#!/bin/sh
# Host test of the interposer MECHANISM (x86_64): the caller and the stock definition are in
# separate TUs, so `mock_query` resolves `isPlatformSupportSoftwareAudioDecoder` through the PLT,
# as libplayerAPIs does. It does NOT prove the ARM build or the TV's libstdc++ ABI.
set -eu
cd "$(dirname "$0")"

CXX=${CXX:-g++}
ABI=-D_GLIBCXX_USE_CXX11_ABI=1
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/platformfix-hosttest.XXXXXX")
trap 'rm -rf "$SCRATCH"' EXIT HUP INT TERM

$CXX -O2 -g -Wall -Wextra -fPIC $ABI -shared -o "$SCRATCH/libmock.so" mock_stock.cpp mock_caller.cpp
$CXX -O2 -g -Wall -Wextra -fPIC $ABI -shared -o "$SCRATCH/libplatformfix.so" ../platformfix.cpp -ldl
$CXX -O2 -g -Wall -Wextra $ABI -o "$SCRATCH/test" test.cpp -L"$SCRATCH" -lmock -Wl,-rpath,'$ORIGIN'

echo "== call site goes through the PLT (mechanism check)"
objdump -d "$SCRATCH/libmock.so" > "$SCRATCH/libmock.dis"
if ! PLT=$(awk '
  /<_Z10mock_queryPKc>:/,/^$/ {
    if ($0 ~ /PlatformSupportSoftwareAudioDecoder@plt/) { print; found=1 }
  }
  END { exit !found }
' "$SCRATCH/libmock.dis"); then
  echo "FAIL: mock_query has no interposable PLT call" >&2
  exit 1
fi
printf '%s\n' "$PLT"

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
SYMBOL='_ZN3smp4util37isPlatformSupportSoftwareAudioDecoderENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE'
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
