#!/bin/bash
# Run the full MCTP/PLDM validator suite against the DUT and summarize results.
#
# Excludes:
#   -Y  PEC enforcement  (known DUT finding: it doesn't validate incoming PEC)
#   -F  firmware update  (writes 128 KB of firmware every run; run manually)
#
# Usage:  ./all_tests.sh [extra run_mctp_test.sh args, e.g. -b 400 -d 0x42]
# All modes run with -C (the DUT requires SMBus PEC). Exit code is non-zero if
# any check failed, so it can gate CI.

set -u
ROOT="$(cd "$(dirname "$0")" && pwd)"
EXTRA="$*" # forwarded to every run (bitrate, explicit target, verbose, ...)

total_pass=0
total_fail=0
declare -a failed_modes

run() {
	local name="$1"
	shift
	local out rc sum p f why
	out=$(timeout 180 "$ROOT/run_mctp_test.sh" "$@" $EXTRA -C 2>&1)
	rc=$?
	sum=$(echo "$out" | grep -m1 "== SUMMARY ==")
	# No SUMMARY (or non-zero exit with none) => the mode aborted before
	# running any checks — almost always because the DUT wasn't found. That
	# is a failure, not a silent 0/0 (which would falsely read as "pass").
	if [ -z "$sum" ]; then
		why=$(echo "$out" | grep -m1 -iE "no endpoint|out of memory|aa_open|failed" || true)
		printf "%-26s | ERROR — %s\n" "$name" "${why:-did not run (rc=$rc)}"
		total_fail=$((total_fail + 1))
		failed_modes+=("$name")
		return
	fi
	p=$(echo "$sum" | sed -n 's/.*== *\([0-9]*\) passed.*/\1/p')
	f=$(echo "$sum" | sed -n 's/.* \([0-9]*\) failed.*/\1/p')
	p=${p:-0}
	f=${f:-0}
	total_pass=$((total_pass + p))
	total_fail=$((total_fail + f))
	printf "%-26s | %s\n" "$name" "${sum#*== SUMMARY ==  }"
	if [ "$f" -ne 0 ]; then
		failed_modes+=("$name")
		echo "$out" | grep "\[FAIL\]" | sed 's/^/      ↳ /'
	fi
}

echo "================= MCTP/PLDM full suite ================="
echo "  (args: ${EXTRA:-none}, PEC on; -Y and -F excluded)"
echo
run "default (master+slave)" # master + slave self-test
run "slave self-test"       -S
run "discovery"             -A   # scan/print, not pass/fail-counted
run "enrollment"            -O
run "PLDM bench"            -G
run "Platform (read)"       -T
run "Platform +writes"      -T -W
run "FW-Update (read-only)" -U
run "Negative/conformance"  -N
run "Event poll"            -V
run "Multipart GetPDR"      -M
run "Soak x100"             -B 100

echo
echo "======================================================="
printf "TOTAL: %d passed, %d failed\n" "$total_pass" "$total_fail"
if [ "$total_fail" -ne 0 ]; then
	echo "FAILED modes: ${failed_modes[*]:-}"
	exit 1
fi
echo "ALL PASS"
exit 0
