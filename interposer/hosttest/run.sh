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
# Parse each row from its right edge: name, Ndx, Vis, Bind, Type. Column numbers before those are
# not portable across readelf versions/architectures. An undefined call target may be recorded as
# NOTYPE (or a weak reference) on some hosts; STT does not affect preemption — the JUMP_SLOT
# relocation below is the proof. UND and JUMP_SLOT stay strict.
# mock_caller.cpp defines only mock_query, so this is its call site.
CALLER_DYNSYMS=$(readelf -W --dyn-syms "$SCRATCH/libmockcaller.so")
ROWS=$(printf '%s\n' "$CALLER_DYNSYMS" | awk -v symbol="$SYMBOL" '$NF == symbol { print }')
[ "$(printf '%s\n' "$ROWS" | wc -l | tr -d ' ')" -eq 1 ] || {
  echo "FAIL: interposed symbol is not present exactly once in the caller's .dynsym" >&2
  printf '  rows read:\n%s\n' "$ROWS" >&2
  exit 1
}
if ! printf '%s\n' "$ROWS" | awk '
  {
    type = $(NF-4); bind = $(NF-3); vis = $(NF-2); ndx = $(NF-1)
    if ((type == "FUNC" || type == "IFUNC" || type == "NOTYPE") &&
        (bind == "GLOBAL" || bind == "WEAK") &&
        vis == "DEFAULT" && ndx == "UND") ok = 1
    else printf "  parsed: type=%s bind=%s vis=%s ndx=%s\n", type, bind, vis, ndx > "/dev/stderr"
  }
  END { exit !ok }
'; then
  echo "FAIL: interposed symbol is not an UND/default-visibility function in the caller's .dynsym" >&2
  printf '  row read: %s\n' "$ROWS" >&2
  exit 1
fi
printf '%s\n' "$ROWS"

CALLER_RELOCS=$(readelf -W --relocs "$SCRATCH/libmockcaller.so")
# Find the relocation type anywhere on the line, then the symbol name anywhere in that line's
# fields (some readelf versions append a version suffix). Both named types are PLT slots; the
# requirement itself is JUMP_SLOT, not a particular disassembly spelling.
if ! JUMP=$(printf '%s\n' "$CALLER_RELOCS" | awk -v symbol="$SYMBOL" '
  {
    slot = 0
    for (i = 1; i <= NF; i++)
      if ($i == "R_X86_64_JUMP_SLOT" || $i == "R_AARCH64_JUMP_SLOT") slot = 1
    if (slot) for (i = 1; i <= NF; i++)
      if ($i == symbol || index($i, symbol "@") == 1) { print; found = 1 }
  }
  END { exit !found }
'); then
  echo "FAIL: mock_query has no JUMP_SLOT relocation for the interposed symbol" >&2
  printf '  relocations read:\n%s\n' "$CALLER_RELOCS" >&2
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
if ! printf '%s\n' "$ROWS" | awk '
  {
    type = $(NF-4); bind = $(NF-3); vis = $(NF-2); ndx = $(NF-1)
    if (type == "FUNC" && bind == "GLOBAL" && vis == "DEFAULT" && ndx != "UND") ok = 1
    else printf "  parsed: type=%s bind=%s vis=%s ndx=%s\n", type, bind, vis, ndx > "/dev/stderr"
  }
  END { exit !ok }
'; then
  echo "FAIL: interposer symbol is not a defined GLOBAL/DEFAULT FUNC" >&2
  printf '  row read: %s\n' "$ROWS" >&2
  exit 1
fi
printf '%s\n' "$ROWS"
