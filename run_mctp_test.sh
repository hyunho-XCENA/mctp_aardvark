#!/bin/bash
# Run the MCTP-over-I2C Aardvark test with the aardvark.so on the dlopen path.
# Pass any program options through, e.g.:  ./run_mctp_test.sh -d 0x1d -E 9 -v
ROOT="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$ROOT/aardvark-api-linux-x86_64-v6.00/c:$LD_LIBRARY_PATH"
exec "$ROOT/mctp_aardvark_test" "$@"
