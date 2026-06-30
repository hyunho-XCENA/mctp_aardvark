#!/bin/bash
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
AA="$ROOT/aardvark-api-linux-x86_64-v6.00/c"
MCTP="$ROOT/libmctp"
MCTP_BUILD="$MCTP/build-meson"
PLDM="$ROOT/libpldm"
PLDM_BUILD="$PLDM/build-meson"

# aardvark.c is a shim that dlopen()s aardvark.so at runtime (looking for it in
# the working dir / LD path), exposing the aa_* API. Compile it in and link -ldl.
# I2C_BTU must match the value libmctp was built with (meson -Di2c_mtu), so the
# struct mctp_binding_i2c layout agrees. 254 is the max (bytecount is one byte).
gcc -Wall -Wextra -O2 -DI2C_BTU=254 \
    "$ROOT/mctp_aardvark_test.c" \
    "$ROOT/pldm_test.c" \
    "$ROOT/pldm_platform_test.c" \
    "$AA/aardvark.c" \
    -I"$AA" -I"$MCTP" -I"$MCTP_BUILD" \
    -I"$PLDM/include" -I"$PLDM_BUILD/include" \
    "$MCTP_BUILD/libmctp.so" \
    "$PLDM_BUILD/src/libpldm.so" \
    -ldl \
    -Wl,-rpath,"$MCTP_BUILD" -Wl,-rpath,"$PLDM_BUILD/src" \
    -o "$ROOT/mctp_aardvark_test"

echo "Built: $ROOT/mctp_aardvark_test"
