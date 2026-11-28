#!/bin/sh
# Host test of the interposer MECHANISM (x86_64): the caller and the stock definition are in
# separate TUs, so `mock_query` resolves `isPlatformSupportSoftwareAudioDecoder` through the PLT,
# as libplayerAPIs does. It does NOT prove the ARM build or the TV's libstdc++ ABI.
set -eu
cd "$(dirname "$0")"

CXX=${CXX:-g++}
ABI=-D_GLIBCXX_USE_CXX11_ABI=1

$CXX -O2 -g -Wall -Wextra -fPIC $ABI -shared -o libmock.so mock_stock.cpp mock_caller.cpp
$CXX -O2 -g -Wall -Wextra -fPIC $ABI -shared -o libplatformfix.so ../platformfix.cpp -ldl
$CXX -O2 -g -Wall -Wextra $ABI -o test test.cpp -L. -lmock -Wl,-rpath,'$ORIGIN'

echo "== call site goes through the PLT (mechanism check)"
objdump -d libmock.so | awk '/<_Z10mock_queryPKc>:/,/^$/' | grep -E 'PlatformSupportSoftwareAudioDecoder@plt' || true

echo
echo "== stock: no preload (the bug: o24n refused)"
./test

echo
echo "== preload WITHOUT the re-exec marker (must stay inert)"
LD_PRELOAD=$PWD/libplatformfix.so ./test

echo
echo "== preload WITH the re-exec marker (the app's path)"
NJ_PLATFORMFIX_ACTIVE=1 LD_PRELOAD=$PWD/libplatformfix.so ./test

echo
echo "== preload + marker + kill switch (must delegate stock)"
NJ_PLATFORMFIX_ACTIVE=1 NJ_PLATFORMFIX=0 LD_PRELOAD=$PWD/libplatformfix.so ./test

echo
echo "== exported symbol"
readelf -W --dyn-syms libplatformfix.so | grep isPlatformSupportSoftwareAudioDecoder
