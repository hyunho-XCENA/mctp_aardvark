#!/bin/bash
# Build the libmctp and libpldm submodules used by the test bench.
set -e

sudo apt install build-essential ninja-build pipx cmake  -y
pipx install meson
pipx ensurepath

ROOT="$(cd "$(dirname "$0")" && pwd)"

# libmctp — i2c_mtu=254 (max) so oversized single packets from the DUT (e.g. a
# 76-byte FRU table) are accepted instead of dropped for exceeding the 64 BTU.
cd "$ROOT/libmctp"
meson setup build-meson -Di2c_mtu=254 || meson configure build-meson -Di2c_mtu=254
meson compile -C build-meson

# libpldm — PLDM message encode/decode library (same one OpenBMC's pldmd uses).
cd "$ROOT/libpldm"
meson setup build-meson || true
meson compile -C build-meson
