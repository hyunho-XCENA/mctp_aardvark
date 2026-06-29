// SPDX-License-Identifier: Apache-2.0
//
// PLDM (DSP0240) test bench over MCTP-over-I2C, built on the shared transport
// core in mctp_test.h. Like OpenBMC's pldmd, our endpoint plays both roles:
//
//   requester  -- query the DUT's PLDM terminus (GetTID, GetPLDMTypes, and for
//                 each supported type GetPLDMVersion + GetPLDMCommands) and
//                 validate the responses. We ask the DUT what it supports first.
//
//   responder  -- our side answers PLDM Base requests like a PLDM device.
//                 Validated by injecting requests and capturing our responses
//                 (libmctp does not implement a PLDM responder, so we do).

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mctp_test.h"

// CRC-32 (IEEE 802.3, reflected) — used in GetPLDMVersion responses.
static uint32_t crc32_ieee(const uint8_t *p, size_t len)
{
	uint32_t c = 0xffffffff;
	for (size_t i = 0; i < len; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (c >> 1) ^ 0xedb88820 : (c >> 1);
	}
	return c ^ 0xffffffff;
}

static const char *pldm_type_name(int t)
{
	switch (t) {
	case 0: return "Base";
	case 1: return "SMBIOS";
	case 2: return "Platform";
	case 3: return "BIOS";
	case 4: return "FRU";
	case 5: return "FW-Update";
	case 6: return "RDE";
	default: return "?";
	}
}

static void fmt_types(char *out, size_t osz, const uint8_t *bits8)
{
	size_t p = 0;
	out[0] = 0;
	for (int t = 0; t < 64; t++)
		if (bits8[t / 8] & (1 << (t % 8)))
			p += snprintf(out + p, p < osz ? osz - p : 0, "%s%d(%s)",
				      p ? "," : "", t, pldm_type_name(t));
}

static void fmt_cmds(char *out, size_t osz, const uint8_t *bits32)
{
	size_t p = 0;
	out[0] = 0;
	for (int c = 0; c < 256; c++)
		if (bits32[c / 8] & (1 << (c % 8)))
			p += snprintf(out + p, p < osz ? osz - p : 0, "%s0x%02x",
				      p ? "," : "", c);
}

// Build a PLDM request, send it, and wait for the matching response.
static bool pldm_request(struct app_ctx *ctx, uint8_t eid, uint8_t type,
			 uint8_t cmd, const uint8_t *data, size_t dlen,
			 int timeout_ms)
{
	uint8_t msg[64];
	size_t n = 0;
	msg[n++] = MCTP_MSG_TYPE_PLDM;
	msg[n++] = PLDM_RQ | (ctx->inst_id++ & 0x1f); // Rq=1, D=0, instance
	msg[n++] = (0 << 6) | (type & 0x3f);	      // hdr ver 0, PLDM type
	msg[n++] = cmd;
	if (data && dlen) {
		memcpy(msg + n, data, dlen);
		n += dlen;
	}
	return request_wait(ctx, eid, msg, n, MCTP_MSG_TYPE_PLDM, cmd,
			    timeout_ms);
}

// ===================================================================
// Responder role: answer PLDM Base requests like a PLDM device.
// Response layout in resp_buf: [0]=0x01 [1]=Rq/D/inst [2]=hdrver|type
//                              [3]=cmd [4]=completion code [5..]=data
// ===================================================================
void pldm_handle_request(struct app_ctx *ctx, uint8_t src_eid, uint8_t msg_tag,
			 const uint8_t *m, size_t len)
{
	if (len < 4)
		return;
	uint8_t inst = m[1] & 0x1f;
	uint8_t type = m[2] & 0x3f;
	uint8_t cmd = m[3];

	uint8_t r[64];
	size_t n = 0;
	r[n++] = MCTP_MSG_TYPE_PLDM;
	r[n++] = inst;		    // Rq=0, D=0, echo instance id
	r[n++] = (0 << 6) | type;   // hdr ver 0, PLDM type
	r[n++] = cmd;

	if (type != PLDM_BASE) {
		r[n++] = PLDM_CC_ERROR_INVALID_TYPE;
	} else {
		switch (cmd) {
		case PLDM_CMD_GET_TID:
			r[n++] = PLDM_CC_SUCCESS;
			r[n++] = PLDM_OUR_TID;
			break;
		case PLDM_CMD_GET_TYPES: {
			r[n++] = PLDM_CC_SUCCESS;
			uint8_t bits[8] = { 0 };
			bits[0] |= 1 << PLDM_BASE; // we support PLDM Base only
			memcpy(r + n, bits, 8);
			n += 8;
			break;
		}
		case PLDM_CMD_GET_VERSION: {
			r[n++] = PLDM_CC_SUCCESS;
			uint8_t next[4] = { 0, 0, 0, 0 }; // NextDataTransferHandle
			memcpy(r + n, next, 4);
			n += 4;
			r[n++] = 0x05; // TransferFlag = Start and End
			uint8_t ver[4] = { 0xF1, 0xF0, 0xF0, 0x00 }; // 1.0.0
			memcpy(r + n, ver, 4);
			n += 4;
			uint32_t crc = crc32_ieee(ver, 4);
			memcpy(r + n, &crc, 4); // little-endian
			n += 4;
			break;
		}
		case PLDM_CMD_GET_COMMANDS: {
			r[n++] = PLDM_CC_SUCCESS;
			uint8_t bits[32] = { 0 };
			bits[0] |= 1 << PLDM_CMD_GET_TID;	// 0x02
			bits[0] |= 1 << PLDM_CMD_GET_VERSION;	// 0x03
			bits[0] |= 1 << PLDM_CMD_GET_TYPES;	// 0x04
			bits[0] |= 1 << PLDM_CMD_GET_COMMANDS;	// 0x05
			memcpy(r + n, bits, 32);
			n += 32;
			break;
		}
		default:
			r[n++] = PLDM_CC_ERROR_UNSUPPORTED;
			break;
		}
	}

	if (ctx->verbose)
		hexdump("   PLDM responding", r, n);
	mctp_message_tx(ctx->mctp, src_eid, false, msg_tag, r, n);
}

// ===================================================================
// Requester role: discover and validate the DUT's PLDM terminus.
// ===================================================================
static void pldm_discover(struct app_ctx *ctx, uint8_t eid, int to,
			  struct results *r)
{
	printf("\n-- requester: querying DUT PLDM (EID %u) --\n", eid);

	// GetTID
	bool ok = pldm_request(ctx, eid, PLDM_BASE, PLDM_CMD_GET_TID, NULL, 0,
			       to);
	uint8_t cc = (ok && ctx->resp_len >= 5) ? ctx->resp_buf[4] : 0xff;
	uint8_t tid = (ok && ctx->resp_len >= 6) ? ctx->resp_buf[5] : 0;
	check(r, "PLDM GetTID", ok && cc == 0, "cc=0x%02x tid=%u", cc, tid);

	// GetPLDMTypes — ask the DUT what it supports before going further.
	ok = pldm_request(ctx, eid, PLDM_BASE, PLDM_CMD_GET_TYPES, NULL, 0, to);
	cc = (ok && ctx->resp_len >= 5) ? ctx->resp_buf[4] : 0xff;
	uint8_t types[8] = { 0 };
	if (ok && cc == 0 && ctx->resp_len >= 13)
		memcpy(types, ctx->resp_buf + 5, 8);
	char tbuf[128];
	fmt_types(tbuf, sizeof(tbuf), types);
	check(r, "PLDM GetPLDMTypes", ok && cc == 0, "cc=0x%02x types=[%s]", cc,
	      tbuf);
	if (!(ok && cc == 0))
		return;

	// For each supported PLDM type: GetPLDMVersion + GetPLDMCommands.
	for (int t = 0; t < 64; t++) {
		if (!(types[t / 8] & (1 << (t % 8))))
			continue;

		uint8_t ver[4] = { 0 };
		uint8_t vreq[6] = { 0, 0, 0, 0, 0x01, (uint8_t)t };
		bool vok = pldm_request(ctx, eid, PLDM_BASE,
					PLDM_CMD_GET_VERSION, vreq, 6, to);
		uint8_t vcc = (vok && ctx->resp_len >= 5) ? ctx->resp_buf[4] :
							    0xff;
		if (vok && vcc == 0 && ctx->resp_len >= 14)
			memcpy(ver, ctx->resp_buf + 10, 4);
		char nm[40];
		snprintf(nm, sizeof(nm), "  type %d(%s) version", t,
			 pldm_type_name(t));
		check(r, nm, vok && vcc == 0,
		      "cc=0x%02x ver=%02x.%02x.%02x.%02x", vcc, ver[0], ver[1],
		      ver[2], ver[3]);

		uint8_t creq[5] = { (uint8_t)t, ver[0], ver[1], ver[2] };
		creq[4] = ver[3];
		bool cok = pldm_request(ctx, eid, PLDM_BASE,
					PLDM_CMD_GET_COMMANDS, creq, 5, to);
		uint8_t ccc = (cok && ctx->resp_len >= 5) ? ctx->resp_buf[4] :
							    0xff;
		uint8_t cmds[32] = { 0 };
		if (cok && ccc == 0 && ctx->resp_len >= 37)
			memcpy(cmds, ctx->resp_buf + 5, 32);
		char cbuf[256];
		fmt_cmds(cbuf, sizeof(cbuf), cmds);
		char nm2[40];
		snprintf(nm2, sizeof(nm2), "  type %d(%s) commands", t,
			 pldm_type_name(t));
		check(r, nm2, cok && ccc == 0, "cc=0x%02x cmds=[%s]", ccc, cbuf);
	}
}

// ===================================================================
// Responder self-test: inject a PLDM request as if from a remote master and
// validate the response our PLDM device emits (captured, no real I2C).
// ===================================================================
static bool pldm_selftest(struct app_ctx *ctx, const char *name, uint8_t type,
			  uint8_t cmd, const uint8_t *data, size_t dlen,
			  struct results *r)
{
	const uint8_t rem_addr = 0x55, rem_eid = 200;

	uint8_t msg[64];
	size_t mn = 0;
	msg[mn++] = MCTP_MSG_TYPE_PLDM;
	msg[mn++] = PLDM_RQ | 0x07;
	msg[mn++] = (0 << 6) | type;
	msg[mn++] = cmd;
	if (data && dlen) {
		memcpy(msg + mn, data, dlen);
		mn += dlen;
	}

	uint8_t f[96];
	size_t fn = 0;
	f[fn++] = (uint8_t)(ctx->own_addr << 1);	   // dest addr (us)
	f[fn++] = 0x0f;					   // MCTP I2C command
	f[fn++] = (uint8_t)(1 + 4 + mn);		   // bytecount
	f[fn++] = (uint8_t)(rem_addr << 1 | 1);		   // source addr
	f[fn++] = 0x01;					   // MCTP ver
	f[fn++] = ctx->own_eid;				   // dest EID (us)
	f[fn++] = rem_eid;				   // src EID
	f[fn++] = MCTP_FLAG_SOM | MCTP_FLAG_EOM | MCTP_FLAG_TO;
	memcpy(f + fn, msg, mn);
	fn += mn;

	ctx->capture = 1;
	ctx->cap_len = 0;
	mctp_i2c_rx(ctx->i2c, f, fn);
	for (int i = 0; i < 4000 && ctx->cap_len == 0; i++)
		mctp_i2c_tx_poll(ctx->i2c);
	ctx->capture = 0;

	if (ctx->cap_len == 0) {
		check(r, name, false, "no response");
		return false;
	}
	const uint8_t *c = ctx->cap_buf;
	if (ctx->verbose)
		hexdump("   captured PLDM resp", c, ctx->cap_len);

	// [0]dest [1]0x0f [2]bc [3]src [4..7]mctp hdr [8]=0x01 [9]=Rq/D/inst
	// [10]=type [11]=cmd [12]=completion code
	bool ok = ctx->cap_len >= 13 && c[1] == 0x0f &&
		  (c[0] >> 1) == rem_addr && c[8] == MCTP_MSG_TYPE_PLDM &&
		  !(c[9] & PLDM_RQ) && c[11] == cmd &&
		  c[12] == PLDM_CC_SUCCESS;
	uint8_t cc = ctx->cap_len >= 13 ? c[12] : 0xff;
	check(r, name, ok, "cmd=0x%02x cc=0x%02x", cmd, cc);
	return ok;
}

static void pldm_responder_selftests(struct app_ctx *ctx, struct results *r)
{
	printf("\n-- responder: our PLDM device answers injected requests --\n");
	pldm_selftest(ctx, "PLDM resp GetTID", PLDM_BASE, PLDM_CMD_GET_TID, NULL,
		      0, r);
	pldm_selftest(ctx, "PLDM resp GetPLDMTypes", PLDM_BASE,
		      PLDM_CMD_GET_TYPES, NULL, 0, r);
	uint8_t vreq[6] = { 0, 0, 0, 0, 0x01, PLDM_BASE };
	pldm_selftest(ctx, "PLDM resp GetPLDMVersion", PLDM_BASE,
		      PLDM_CMD_GET_VERSION, vreq, 6, r);
	uint8_t creq[5] = { PLDM_BASE, 0xF1, 0xF0, 0xF0, 0x00 };
	pldm_selftest(ctx, "PLDM resp GetPLDMCommands", PLDM_BASE,
		      PLDM_CMD_GET_COMMANDS, creq, 5, r);
}

void run_pldm_bench(struct app_ctx *ctx, uint8_t dst_eid, int timeout_ms,
		    struct results *r)
{
	printf("\n== PLDM bench (OpenBMC-style) ==\n");
	pldm_discover(ctx, dst_eid, timeout_ms, r);
	pldm_responder_selftests(ctx, r);
}
