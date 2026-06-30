// SPDX-License-Identifier: Apache-2.0
//
// PLDM negative / conformance validator over MCTP-over-I2C.
//
// Every other bench checks the happy path (valid request -> cc=0x00). This one
// checks the opposite: the DUT must REJECT malformed or out-of-range requests
// with the right PLDM completion code, never silently accept them. Sending a
// bad request and getting PLDM_SUCCESS back is a real firmware bug (the device
// acted on garbage), so that is the hard failure here.
//
// All requests are reads or invalid, so the DUT state is not changed.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mctp_test.h"

#include <libpldm/base.h>
#include <libpldm/platform.h>

#define MCTP_PLDM_OVERHEAD (1 + sizeof(struct pldm_msg_hdr))

// Unsupported types / commands used to provoke errors.
#define PLDM_TYPE_SMBIOS 1
#define PLDM_TYPE_RDE	 6
#define BASE_BAD_CMD	 0x7e // not a real Base command
#define FRU_GET_BY_OPTION 0x04 // FRU cmd the DUT does not implement

// Send a raw PLDM request (type/cmd/payload) and return the completion code,
// or -1 if no response came back.
static int pldm_raw(struct app_ctx *ctx, uint8_t eid, uint8_t type, uint8_t cmd,
		    const uint8_t *payload, size_t plen, int to)
{
	uint8_t tx[80];
	tx[0] = MCTP_MSG_TYPE_PLDM;
	tx[1] = PLDM_RQ | (ctx->inst_id++ & 0x1f);
	tx[2] = (0 << 6) | (type & 0x3f);
	tx[3] = cmd;
	if (payload && plen)
		memcpy(tx + 4, payload, plen);
	if (!request_wait(ctx, eid, tx, 4 + plen, MCTP_MSG_TYPE_PLDM, cmd, to))
		return -1;
	size_t pl = ctx->resp_len > MCTP_PLDM_OVERHEAD ?
			    ctx->resp_len - MCTP_PLDM_OVERHEAD :
			    0;
	const struct pldm_msg *rsp = (const struct pldm_msg *)(ctx->resp_buf + 1);
	return pl >= 1 ? rsp->payload[0] : -1;
}

// Assert that an invalid request was rejected. Accepting it (cc=SUCCESS) is the
// real bug -> FAIL. A spec-defined error code -> PASS (noted if it differs from
// the expected code). No response -> FAIL when the DUT must answer (well-formed
// message), PASS when a transport-level drop is acceptable (malformed length).
static void neg_check(struct results *r, const char *name, int cc, int expected,
		      bool must_respond)
{
	bool ok;
	char det[96];
	if (cc < 0) {
		ok = !must_respond;
		snprintf(det, sizeof(det),
			 ok ? "dropped (acceptable, expected 0x%02x)" :
			      "NO RESPONSE (expected error 0x%02x)",
			 expected);
	} else if (cc == PLDM_SUCCESS) {
		ok = false;
		snprintf(det, sizeof(det), "WRONGLY ACCEPTED (cc=0x00)");
	} else {
		ok = true;
		if (cc == expected)
			snprintf(det, sizeof(det), "rejected cc=0x%02x (expected)",
				 cc);
		else
			snprintf(det, sizeof(det),
				 "rejected cc=0x%02x (expected 0x%02x)", cc,
				 expected);
	}
	check(r, name, ok, "%s", det);
}

void run_pldm_negative_bench(struct app_ctx *ctx, uint8_t dst_eid,
			     int timeout_ms, struct results *r)
{
	int to = timeout_ms;
	printf("\n== PLDM negative / conformance validator ==\n");
	printf("   (the DUT must reject bad input with an error, never cc=0x00)\n");

	// 1. Unsupported PLDM type (SMBIOS=1) -> INVALID_PLDM_TYPE.
	int cc = pldm_raw(ctx, dst_eid, PLDM_TYPE_SMBIOS, 0x01, NULL, 0, to);
	neg_check(r, "unsupported type 1 (SMBIOS)", cc,
		  PLDM_ERROR_INVALID_PLDM_TYPE, true);

	// 2. Unsupported PLDM type (RDE=6) -> INVALID_PLDM_TYPE.
	cc = pldm_raw(ctx, dst_eid, PLDM_TYPE_RDE, 0x01, NULL, 0, to);
	neg_check(r, "unsupported type 6 (RDE)", cc,
		  PLDM_ERROR_INVALID_PLDM_TYPE, true);

	// 3. Supported type (Base) but bogus command -> UNSUPPORTED_PLDM_CMD.
	cc = pldm_raw(ctx, dst_eid, PLDM_BASE, BASE_BAD_CMD, NULL, 0, to);
	neg_check(r, "Base unsupported cmd 0x7e", cc,
		  PLDM_ERROR_UNSUPPORTED_PLDM_CMD, true);

	// 4. FRU command the DUT does not implement -> UNSUPPORTED_PLDM_CMD.
	cc = pldm_raw(ctx, dst_eid, PLDM_FRU, FRU_GET_BY_OPTION, NULL, 0, to);
	neg_check(r, "FRU unsupported cmd 0x04", cc,
		  PLDM_ERROR_UNSUPPORTED_PLDM_CMD, true);

	// 5. GetSensorReading for a non-existent sensor -> INVALID_SENSOR_ID.
	uint8_t bad_sensor[3] = { 0xff, 0xff, 0x00 }; // sensorID=0xffff, rearm=0
	cc = pldm_raw(ctx, dst_eid, PLDM_PLATFORM, PLDM_GET_SENSOR_READING,
		      bad_sensor, sizeof(bad_sensor), to);
	neg_check(r, "GetSensorReading invalid id 0xffff", cc,
		  PLDM_PLATFORM_INVALID_SENSOR_ID, true);

	// 6. GetStateEffecterStates for a non-existent effecter ->
	//    INVALID_EFFECTER_ID.
	uint8_t bad_eff[2] = { 0xff, 0xff }; // effecterID=0xffff
	cc = pldm_raw(ctx, dst_eid, PLDM_PLATFORM, PLDM_GET_STATE_EFFECTER_STATES,
		      bad_eff, sizeof(bad_eff), to);
	neg_check(r, "GetStateEffecterStates invalid id 0xffff", cc,
		  PLDM_PLATFORM_INVALID_EFFECTER_ID, true);

	// 7. GetPDR with a record handle that does not exist ->
	//    INVALID_RECORD_HANDLE.
	uint8_t bad_pdr[13] = { 0 };
	bad_pdr[0] = bad_pdr[1] = bad_pdr[2] = bad_pdr[3] = 0xff; // record_handle
	bad_pdr[8] = PLDM_GET_FIRSTPART;			 // transfer_op_flag
	bad_pdr[9] = 0x80;					 // request_count lo
	cc = pldm_raw(ctx, dst_eid, PLDM_PLATFORM, PLDM_GET_PDR, bad_pdr,
		      sizeof(bad_pdr), to);
	neg_check(r, "GetPDR invalid record handle", cc,
		  PLDM_PLATFORM_INVALID_RECORD_HANDLE, true);

	// 8. Truncated GetSensorReading (1 payload byte instead of 3) ->
	//    INVALID_LENGTH. A transport-level drop is also acceptable here.
	uint8_t short_payload[1] = { 0x01 };
	cc = pldm_raw(ctx, dst_eid, PLDM_PLATFORM, PLDM_GET_SENSOR_READING,
		      short_payload, sizeof(short_payload), to);
	neg_check(r, "GetSensorReading truncated payload", cc,
		  PLDM_ERROR_INVALID_LENGTH, false);
}
