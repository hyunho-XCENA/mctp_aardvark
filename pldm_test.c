// SPDX-License-Identifier: Apache-2.0
//
// PLDM (DSP0240) test bench over MCTP-over-I2C, built on the shared transport
// core in mctp_test.h. PLDM messages are encoded/decoded with OpenBMC's
// libpldm (the same library OpenBMC's pldmd uses) — only the encode/decode
// library, not the daemon.
//
//   requester  -- query the DUT's PLDM terminus (Base discovery + FRU) using
//                 libpldm encode/decode, and validate the responses.
//   responder  -- our side answers PLDM Base requests like a PLDM device,
//                 validated by injecting requests and capturing our responses.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mctp_test.h"

#include <libpldm/base.h>
#include <libpldm/fru.h>
#include <libpldm/pldm_types.h>

#define PLDM_OUR_TID 0x01 // our terminus id when acting as a device

// A PLDM message on the wire = MCTP type byte (0x01) + struct pldm_msg
// (3-byte header + payload). libpldm encodes into a struct pldm_msg, so we
// place it at offset 1 and prepend the MCTP type byte ourselves.
#define PLDM_HDR_SZ (sizeof(struct pldm_msg_hdr))
#define MCTP_PLDM_OVERHEAD (1 + PLDM_HDR_SZ) // MCTP byte + PLDM header

static uint8_t pl_iid(struct app_ctx *ctx)
{
	return ctx->inst_id++ & 0x1f;
}

// After request_wait, return the response as a struct pldm_msg + payload len.
static const struct pldm_msg *pldm_resp(struct app_ctx *ctx, size_t *payload_len)
{
	*payload_len = ctx->resp_len > MCTP_PLDM_OVERHEAD ?
			       ctx->resp_len - MCTP_PLDM_OVERHEAD :
			       0;
	return (const struct pldm_msg *)(ctx->resp_buf + 1);
}

static const char *pldm_type_name(int t)
{
	switch (t) {
	case PLDM_BASE: return "Base";
	case 1: return "SMBIOS";
	case PLDM_PLATFORM: return "Platform";
	case 3: return "BIOS";
	case PLDM_FRU: return "FRU";
	case PLDM_FWUP: return "FW-Update";
	case 6: return "RDE";
	default: return "?";
	}
}

// Decode a PLDM BCD version byte (0xFx -> x, else two BCD digits).
static int bcd(uint8_t b)
{
	if ((b >> 4) == 0xf)
		return b & 0x0f;
	return (b >> 4) * 10 + (b & 0x0f);
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

// ===================================================================
// Requester role: discover and validate the DUT's PLDM terminus (libpldm).
// ===================================================================
static void pldm_discover(struct app_ctx *ctx, uint8_t eid, int to,
			  struct results *r)
{
	uint8_t tx[512];
	tx[0] = MCTP_MSG_TYPE_PLDM;
	struct pldm_msg *req = (struct pldm_msg *)(tx + 1);
	size_t pl;
	const struct pldm_msg *rsp;

	printf("\n-- requester: querying DUT PLDM (EID %u) via libpldm --\n",
	       eid);

	// GetTID
	bool ok = encode_get_tid_req(pl_iid(ctx), req) == 0 &&
		  request_wait(ctx, eid, tx,
			       MCTP_PLDM_OVERHEAD + PLDM_GET_TID_REQ_BYTES,
			       MCTP_MSG_TYPE_PLDM, PLDM_GET_TID, to);
	uint8_t cc = 0xff, tid = 0;
	if (ok) {
		rsp = pldm_resp(ctx, &pl);
		decode_get_tid_resp(rsp, pl, &cc, &tid);
	}
	check(r, "PLDM GetTID", ok && cc == PLDM_SUCCESS, "cc=0x%02x tid=%u", cc,
	      tid);

	// GetPLDMTypes — ask the DUT what it supports (no libpldm decode helper,
	// so read the 8-byte type bitfield from the payload directly).
	ok = encode_get_types_req(pl_iid(ctx), req) == 0 &&
	     request_wait(ctx, eid, tx,
			  MCTP_PLDM_OVERHEAD + PLDM_GET_TYPES_REQ_BYTES,
			  MCTP_MSG_TYPE_PLDM, PLDM_GET_PLDM_TYPES, to);
	uint8_t types[8] = { 0 };
	cc = 0xff;
	if (ok) {
		rsp = pldm_resp(ctx, &pl);
		if (pl >= 9) {
			cc = rsp->payload[0];
			memcpy(types, &rsp->payload[1], 8);
		}
	}
	char tbuf[160];
	fmt_types(tbuf, sizeof(tbuf), types);
	check(r, "PLDM GetPLDMTypes", ok && cc == PLDM_SUCCESS,
	      "cc=0x%02x types=[%s]", cc, tbuf);
	if (!(ok && cc == PLDM_SUCCESS))
		return;

	// For each supported PLDM type: GetPLDMVersion + GetPLDMCommands.
	for (int t = 0; t < 64; t++) {
		if (!(types[t / 8] & (1 << (t % 8))))
			continue;

		ver32_t ver = { 0 };
		uint8_t vcc = 0xff;
		uint32_t next = 0;
		uint8_t flag = 0;
		bool vok = encode_get_version_req(pl_iid(ctx), 0,
						  PLDM_GET_FIRSTPART,
						  (uint8_t)t, req) == 0 &&
			   request_wait(ctx, eid, tx,
					MCTP_PLDM_OVERHEAD +
						PLDM_GET_VERSION_REQ_BYTES,
					MCTP_MSG_TYPE_PLDM,
					PLDM_GET_PLDM_VERSION, to);
		if (vok) {
			rsp = pldm_resp(ctx, &pl);
			decode_get_version_resp(rsp, pl, &vcc, &next, &flag,
						&ver);
		}
		char nm[40];
		snprintf(nm, sizeof(nm), "  type %d(%s) version", t,
			 pldm_type_name(t));
		check(r, nm, vok && vcc == PLDM_SUCCESS, "cc=0x%02x ver=%d.%d.%d",
		      vcc, bcd(ver.major), bcd(ver.minor), bcd(ver.update));

		bitfield8_t cmds[PLDM_MAX_CMDS_PER_TYPE / 8];
		memset(cmds, 0, sizeof(cmds));
		uint8_t ccc = 0xff;
		bool cok = encode_get_commands_req(pl_iid(ctx), (uint8_t)t, ver,
						   req) == 0 &&
			   request_wait(ctx, eid, tx,
					MCTP_PLDM_OVERHEAD +
						PLDM_GET_COMMANDS_REQ_BYTES,
					MCTP_MSG_TYPE_PLDM,
					PLDM_GET_PLDM_COMMANDS, to);
		if (cok) {
			rsp = pldm_resp(ctx, &pl);
			decode_get_commands_resp(rsp, pl, &ccc, cmds);
		}
		char cbuf[256];
		fmt_cmds(cbuf, sizeof(cbuf), (const uint8_t *)cmds);
		char nm2[40];
		snprintf(nm2, sizeof(nm2), "  type %d(%s) commands", t,
			 pldm_type_name(t));
		check(r, nm2, cok && ccc == PLDM_SUCCESS, "cc=0x%02x cmds=[%s]",
		      ccc, cbuf);
	}
}

// Read the DUT's FRU record table (FRU type 0x04) using libpldm.
static void pldm_fru(struct app_ctx *ctx, uint8_t eid, int to,
		     struct results *r)
{
	uint8_t tx[512];
	tx[0] = MCTP_MSG_TYPE_PLDM;
	struct pldm_msg *req = (struct pldm_msg *)(tx + 1);
	size_t pl;
	const struct pldm_msg *rsp;

	printf("\n-- requester: reading DUT FRU table --\n");

	// GetFRURecordTableMetadata
	uint8_t cc = 0xff, vmaj = 0, vmin = 0;
	uint32_t maxsz = 0, tablen = 0, csum = 0;
	uint16_t total_rsi = 0, total_rec = 0;
	bool ok = encode_get_fru_record_table_metadata_req(
			  pl_iid(ctx), req,
			  PLDM_GET_FRU_RECORD_TABLE_METADATA_REQ_BYTES) == 0 &&
		  request_wait(
			  ctx, eid, tx,
			  MCTP_PLDM_OVERHEAD +
				  PLDM_GET_FRU_RECORD_TABLE_METADATA_REQ_BYTES,
			  MCTP_MSG_TYPE_PLDM,
			  PLDM_GET_FRU_RECORD_TABLE_METADATA, to);
	if (ok) {
		rsp = pldm_resp(ctx, &pl);
		decode_get_fru_record_table_metadata_resp(
			rsp, pl, &cc, &vmaj, &vmin, &maxsz, &tablen, &total_rsi,
			&total_rec, &csum);
	}
	check(r, "PLDM FRU TableMetadata", ok && cc == PLDM_SUCCESS,
	      "cc=0x%02x records=%u table_len=%u", cc, total_rec, tablen);
	if (!(ok && cc == PLDM_SUCCESS))
		return;

	// GetFRURecordTable (first part)
	uint8_t tbl[1024];
	size_t tbllen = 0;
	uint32_t next = 0;
	uint8_t flag = 0;
	cc = 0xff;
	ok = encode_get_fru_record_table_req(
		     pl_iid(ctx), 0, PLDM_GET_FIRSTPART, req,
		     PLDM_GET_FRU_RECORD_TABLE_REQ_BYTES) == 0 &&
	     request_wait(ctx, eid, tx,
			  MCTP_PLDM_OVERHEAD + PLDM_GET_FRU_RECORD_TABLE_REQ_BYTES,
			  MCTP_MSG_TYPE_PLDM, PLDM_GET_FRU_RECORD_TABLE, to);
	if (ok) {
		rsp = pldm_resp(ctx, &pl);
		decode_get_fru_record_table_resp_safe(rsp, pl, &cc, &next, &flag,
						      tbl, &tbllen, sizeof(tbl));
	}
	check(r, "PLDM FRU RecordTable", ok && cc == PLDM_SUCCESS,
	      "cc=0x%02x got %zu bytes", cc, tbllen);
	if (ok && cc == PLDM_SUCCESS && ctx->verbose)
		hexdump("   FRU table", tbl, tbllen);
}

// ===================================================================
// Responder role: answer PLDM Base requests like a PLDM device.
// Built by hand (our device is simple); the wire layout is
// [0]=0x01 MCTP [1]=Rq/D/inst [2]=hdrver|type [3]=cmd [4]=cc [5..]=data.
// ===================================================================

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
	r[n++] = inst;		  // Rq=0, D=0, echo instance id
	r[n++] = (0 << 6) | type; // hdr ver 0, PLDM type
	r[n++] = cmd;

	if (type != PLDM_BASE) {
		r[n++] = PLDM_ERROR_INVALID_PLDM_TYPE;
	} else {
		switch (cmd) {
		case PLDM_GET_TID:
			r[n++] = PLDM_SUCCESS;
			r[n++] = PLDM_OUR_TID;
			break;
		case PLDM_GET_PLDM_TYPES: {
			r[n++] = PLDM_SUCCESS;
			uint8_t bits[8] = { 0 };
			bits[0] |= 1 << PLDM_BASE; // we support PLDM Base only
			memcpy(r + n, bits, 8);
			n += 8;
			break;
		}
		case PLDM_GET_PLDM_VERSION: {
			r[n++] = PLDM_SUCCESS;
			uint8_t next[4] = { 0, 0, 0, 0 };
			memcpy(r + n, next, 4);
			n += 4;
			r[n++] = PLDM_START_AND_END;
			uint8_t ver[4] = { 0x00, 0xf0, 0xf0, 0xf1 }; // 1.0.0
			memcpy(r + n, ver, 4);
			n += 4;
			uint32_t crc = crc32_ieee(ver, 4);
			memcpy(r + n, &crc, 4); // little-endian
			n += 4;
			break;
		}
		case PLDM_GET_PLDM_COMMANDS: {
			r[n++] = PLDM_SUCCESS;
			uint8_t bits[32] = { 0 };
			bits[0] |= 1 << PLDM_GET_TID;
			bits[0] |= 1 << PLDM_GET_PLDM_VERSION;
			bits[0] |= 1 << PLDM_GET_PLDM_TYPES;
			bits[0] |= 1 << PLDM_GET_PLDM_COMMANDS;
			memcpy(r + n, bits, 32);
			n += 32;
			break;
		}
		default:
			r[n++] = PLDM_ERROR_UNSUPPORTED_PLDM_CMD;
			break;
		}
	}

	if (ctx->verbose)
		hexdump("   PLDM responding", r, n);
	mctp_message_tx(ctx->mctp, src_eid, false, msg_tag, r, n);
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
		  !(c[9] & PLDM_RQ) && c[11] == cmd && c[12] == PLDM_SUCCESS;
	uint8_t cc = ctx->cap_len >= 13 ? c[12] : 0xff;
	check(r, name, ok, "cmd=0x%02x cc=0x%02x", cmd, cc);
	return ok;
}

static void pldm_responder_selftests(struct app_ctx *ctx, struct results *r)
{
	printf("\n-- responder: our PLDM device answers injected requests --\n");
	pldm_selftest(ctx, "PLDM resp GetTID", PLDM_BASE, PLDM_GET_TID, NULL, 0,
		      r);
	pldm_selftest(ctx, "PLDM resp GetPLDMTypes", PLDM_BASE,
		      PLDM_GET_PLDM_TYPES, NULL, 0, r);
	uint8_t vreq[6] = { 0, 0, 0, 0, PLDM_GET_FIRSTPART, PLDM_BASE };
	pldm_selftest(ctx, "PLDM resp GetPLDMVersion", PLDM_BASE,
		      PLDM_GET_PLDM_VERSION, vreq, 6, r);
	uint8_t creq[5] = { PLDM_BASE, 0x00, 0xf0, 0xf0, 0xf1 };
	pldm_selftest(ctx, "PLDM resp GetPLDMCommands", PLDM_BASE,
		      PLDM_GET_PLDM_COMMANDS, creq, 5, r);
}

void run_pldm_bench(struct app_ctx *ctx, uint8_t dst_eid, int timeout_ms,
		    struct results *r)
{
	printf("\n== PLDM bench (libpldm, OpenBMC-style) ==\n");
	pldm_discover(ctx, dst_eid, timeout_ms, r);
	pldm_fru(ctx, dst_eid, timeout_ms, r);
	pldm_responder_selftests(ctx, r);
}
