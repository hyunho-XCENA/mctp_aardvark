#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
AA="$ROOT/aardvark-api-linux-x86_64-v6.00/c"
MCTP="$ROOT/libmctp"
MCTP_BUILD="$MCTP/build-meson"

# aardvark.c is a shim that dlopen()s aardvark.so at runtime (looking for it in
# the working dir / LD path), exposing the aa_* API. Compile it in and link -ldl.
gcc -Wall -Wextra -O2 \
    "$ROOT/mctp_aardvark_test.c" \
    "$AA/aardvark.c" \
    -I"$AA" -I"$MCTP" -I"$MCTP_BUILD" \
    "$MCTP_BUILD/libmctp.so" \
    -ldl \
    -Wl,-rpath,"$MCTP_BUILD" \
    -o "$ROOT/mctp_aardvark_test"

echo "Built: $ROOT/mctp_aardvark_test"
