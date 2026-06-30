// SPDX-License-Identifier: Apache-2.0
//
// Shared core for the Aardvark MCTP/PLDM test bench: the transport context and
// the request/response plumbing used by both the MCTP validator
// (mctp_aardvark_test.c) and the PLDM bench (pldm_test.c).
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aardvark.h"

#include "libmctp.h"
#include "libmctp-i2c.h"
#include "libmctp-sizes.h" // provides MCTP_SIZEOF_BINDING_I2C
#include "i2c-internal.h"   // struct mctp_binding_i2c (for raw neighbour access)

// ---- MCTP message-type / control constants ----
#define MCTP_MSG_TYPE_CONTROL	  0x00
#define MCTP_CTRL_FLAG_REQUEST	  0x80 // Rq=1, D=0
#define MCTP_CTRL_CMD_SET_EID	  0x01
#define MCTP_CTRL_CMD_GET_EID	  0x02
#define MCTP_CTRL_CMD_GET_UUID	  0x03
#define MCTP_CTRL_CMD_GET_VERSION 0x04
#define MCTP_CTRL_CMD_GET_TYPES	  0x05
#define MCTP_CTRL_CMD_GET_VENDOR  0x06 // Get Vendor Defined Message Support
#define MCTP_CTRL_CC_SUCCESS	  0x00
#define MCTP_CTRL_CC_UNSUPPORTED  0x05 // ERROR_UNSUPPORTED_CMD
#define EXPECT_NONE		  0xff

#define MCTP_EID_NULL_LOCAL 0x00 // NULL destination EID, used for discovery

// MCTP header flag bits
#define MCTP_FLAG_SOM 0x80
#define MCTP_FLAG_EOM 0x40
#define MCTP_FLAG_TO  0x08

// ---- PLDM (DSP0240) over MCTP (message type 0x01) ----
// Only the transport-level constants live here (used by rx_all in
// mctp_aardvark_test.c). All PLDM command/type/completion codes come from
// libpldm (pldm_test.c), to avoid clashing with libpldm's own definitions.
#define MCTP_MSG_TYPE_PLDM 0x01
#define PLDM_RQ		   0x80 // request bit in PLDM hdr byte 0

struct app_ctx {
	Aardvark aa;
	struct mctp *mctp;
	struct mctp_binding_i2c *i2c;
	int verbose;
	int pec;	  // append/verify SMBus PEC (CRC-8) byte
	int corrupt_pec;  // if set, flip the appended PEC byte (PEC-enforcement test)
	uint8_t own_addr; // our 7-bit slave address
	uint8_t own_eid;
	uint8_t inst_id;  // control message instance id, auto-increment

	// capture mode: aa_tx stores the frame instead of touching hardware
	int capture;
	uint8_t cap_buf[300];
	size_t cap_len;

	// pending request, awaiting matching response
	uint8_t expect_msgtype; // 0xff = not waiting; 0x00 control, 0x01 pldm
	uint8_t expect_cmd;
	int resp_got;
	uint8_t resp_buf[300]; // the MCTP message payload of the response
	size_t resp_len;
	uint8_t resp_src_eid;

	// live-listen accounting
	int req_seen;

	// last physical TX status (for fast scan: skip wait on SLA_NACK)
	int last_tx_status;
	int quiet; // suppress per-TX status lines (used during bus scan)
};

// ---- pass/fail bookkeeping ----
struct results {
	int pass;
	int fail;
};

// ---- shared helpers (implemented in mctp_aardvark_test.c) ----
void hexdump(const char *tag, const uint8_t *p, size_t len);
void check(struct results *r, const char *name, bool ok, const char *fmt, ...);

// Send a complete MCTP message and wait for a response that matches
// (expect_msgtype, expect_cmd). The command byte is at offset 2 for control
// (msgtype 0x00) and offset 3 for PLDM (msgtype 0x01); the matched response is
// left in ctx->resp_buf / ctx->resp_len. Returns false on SLA_NACK or timeout.
bool request_wait(struct app_ctx *ctx, uint8_t dst_eid, const uint8_t *msg,
		  size_t msg_len, uint8_t expect_msgtype, uint8_t expect_cmd,
		  int timeout_ms);

// ---- PLDM bench (implemented in pldm_test.c) ----
// Respond to an incoming PLDM request like a PLDM device (PLDM Base terminus).
void pldm_handle_request(struct app_ctx *ctx, uint8_t src_eid, uint8_t msg_tag,
			 const uint8_t *m, size_t len);
// Full PLDM bench: discover the DUT (requester) + responder self-tests.
void run_pldm_bench(struct app_ctx *ctx, uint8_t dst_eid, int timeout_ms,
		    struct results *r);

// ---- PLDM Platform validator (implemented in pldm_platform_test.c) ----
// Walk the DUT's PDR repository and read every sensor/effecter it advertises
// (GetPDRRepositoryInfo/GetPDR + GetSensorReading/GetStateSensorReadings/
// GetSensorThresholds/GetStateEffecterStates). When allow_writes is set, also
// round-trip the write commands (Set*) by writing back the value just read, so
// the DUT's live state is preserved.
void run_pldm_platform_bench(struct app_ctx *ctx, uint8_t dst_eid,
			     int timeout_ms, int allow_writes,
			     struct results *r);

// Actively drive one state effecter to a given state (changes the DUT, e.g. to
// turn an LED on/off). Reads the effecter state before and after to confirm.
void run_pldm_effecter_drive(struct app_ctx *ctx, uint8_t dst_eid,
			     uint16_t effecter_id, uint8_t state, int timeout_ms,
			     struct results *r);

// Poll the DUT for queued platform events (PollForPlatformEventMessage 0x0b).
void run_pldm_event_bench(struct app_ctx *ctx, uint8_t dst_eid, int timeout_ms,
			  struct results *r);

// Force a multipart GetPDR transfer (small requestCount) and verify the
// reassembled record matches a single-shot read.
void run_pldm_multipart_bench(struct app_ctx *ctx, uint8_t dst_eid,
			      int timeout_ms, struct results *r);

// ---- PLDM Firmware Update validator (implemented in pldm_fwup_test.c) ----
// Read-only: acts as Update Agent and queries the DUT (Firmware Device) with
// QueryDeviceIdentifiers + GetFirmwareParameters + GetStatus. Does NOT run an
// actual update (no RequestUpdate/UpdateComponent/Activate).
void run_pldm_fwup_bench(struct app_ctx *ctx, uint8_t dst_eid, int timeout_ms,
			 struct results *r);

// ---- PLDM negative/conformance validator (pldm_negative_test.c) ----
// Sends malformed / out-of-range PLDM requests and checks the DUT rejects them
// with the correct completion code (never cc=SUCCESS). Does not change the DUT.
void run_pldm_negative_bench(struct app_ctx *ctx, uint8_t dst_eid,
			     int timeout_ms, struct results *r);
