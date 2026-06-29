// SPDX-License-Identifier: Apache-2.0
//
// MCTP-over-I2C(SMBus) validator using a Total Phase Aardvark adapter.
//
// Like an OpenBMC MCTP endpoint, the Aardvark plays BOTH roles, and this tool
// validates each one:
//
//   MASTER (requester) tests  -- we send MCTP control requests to the DUT over
//     the real I2C bus and validate the responses (completion codes / fields).
//
//   SLAVE (responder) tests   -- we inject synthetic incoming requests into
//     libmctp (mctp_i2c_rx) and capture the auto-generated responses from the
//     binding tx callback, validating that our endpoint answers correctly.
//     This exercises the exact responder path a remote master would trigger,
//     deterministically and without needing the DUT to initiate traffic.
//
//   LIVE listen (-L secs)     -- optionally observe DUT-initiated requests.
//
// Use -i for an interactive command shell instead of the validator.
//
// Build with ./build_mctp_test.sh

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mctp_test.h"

// ---- Defaults (override on the command line) ----
#define DEF_PORT     0
#define DEF_BITRATE  100	// kHz (SMBus standard)
#define DEF_OWN_ADDR 0x20	// our 7-bit I2C slave address
#define DEF_OWN_EID  8
// Peer address/EID are auto-discovered when not given on the command line.

void check(struct results *r, const char *name, bool ok, const char *fmt, ...)
{
	char detail[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);
	printf("  [%s] %-28s %s\n", ok ? "PASS" : "FAIL", name, detail);
	if (ok)
		r->pass++;
	else
		r->fail++;
}

static uint64_t now_ms(void *ctx)
{
	(void)ctx;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void hexdump(const char *tag, const uint8_t *p, size_t len)
{
	printf("%s [%zu]:", tag, len);
	for (size_t i = 0; i < len; i++)
		printf(" %02x", p[i]);
	printf("\n");
}

// SMBus PEC: CRC-8, poly 0x07, init 0x00, no reflection.
static uint8_t crc8_smbus(const uint8_t *p, size_t len)
{
	uint8_t crc = 0;
	for (size_t i = 0; i < len; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) :
					     (uint8_t)(crc << 1);
	}
	return crc;
}

// libmctp tx callback. In capture mode, stash the frame for validation instead
// of transmitting. Otherwise send it over the Aardvark (stripping buf[0], the
// dest address byte, and optionally appending an SMBus PEC byte).
static int aa_tx(const void *buf, size_t len, void *vctx)
{
	struct app_ctx *ctx = vctx;
	const uint8_t *b = buf;

	if (len < 4)
		return -EINVAL;

	if (ctx->capture) {
		memcpy(ctx->cap_buf, b, len);
		ctx->cap_len = len;
		ctx->last_tx_status = AA_I2C_STATUS_OK;
		return 0;
	}

	uint8_t dst7 = b[0] >> 1;

	uint8_t out[1 + 256];
	size_t outlen = len - 1;
	if (outlen > sizeof(out) - 1)
		return -EMSGSIZE;
	memcpy(out, b + 1, outlen); // [cmd, bytecount, source, payload...]
	if (ctx->pec) {
		uint8_t pecbuf[2 + 256];
		pecbuf[0] = (uint8_t)(dst7 << 1); // write address
		memcpy(pecbuf + 1, out, outlen);
		out[outlen] = crc8_smbus(pecbuf, outlen + 1);
		outlen++;
	}

	if (ctx->verbose)
		hexdump(">> TX wire (after addr)", out, outlen);

	u16 written = 0;
	int status = aa_i2c_write_ext(ctx->aa, dst7, AA_I2C_NO_FLAGS,
				      (u16)outlen, out, &written);
	ctx->last_tx_status = status;
	if (ctx->verbose || (status != AA_I2C_STATUS_OK && !ctx->quiet))
		printf(">> TX to 0x%02x: status=%d, %u/%zu bytes acked\n", dst7,
		       status, written, outlen);
	if (status != AA_I2C_STATUS_OK)
		return -EIO;
	return 0;
}

static void record_resp(struct app_ctx *ctx, uint8_t src_eid, const uint8_t *m,
			size_t len)
{
	size_t n = len > sizeof(ctx->resp_buf) ? sizeof(ctx->resp_buf) : len;
	memcpy(ctx->resp_buf, m, n);
	ctx->resp_len = len;
	ctx->resp_src_eid = src_eid;
	ctx->resp_got = 1;
}

static void rx_all(uint8_t src_eid, bool tag_owner, uint8_t msg_tag, void *vctx,
		   void *msg, size_t len)
{
	struct app_ctx *ctx = vctx;
	const uint8_t *m = msg;

	if (ctx->verbose) {
		printf("\n<< MCTP msg from EID %u (tag=%u owner=%d, %zu bytes)\n",
		       src_eid, msg_tag, tag_owner, len);
		hexdump("   payload", m, len);
	}

	// PLDM request addressed to us -> respond like a PLDM device.
	// (MCTP control requests are auto-handled by libmctp, not delivered here.)
	if (len >= 4 && m[0] == MCTP_MSG_TYPE_PLDM && (m[1] & PLDM_RQ)) {
		pldm_handle_request(ctx, src_eid, msg_tag, m, len);
		return;
	}

	// Otherwise, record a response we are waiting for (control or PLDM).
	if (ctx->expect_msgtype == 0xff || len < 1 ||
	    m[0] != ctx->expect_msgtype)
		return;
	if (m[0] == MCTP_MSG_TYPE_CONTROL && len >= 3 &&
	    !(m[1] & MCTP_CTRL_FLAG_REQUEST) && m[2] == ctx->expect_cmd)
		record_resp(ctx, src_eid, m, len);
	else if (m[0] == MCTP_MSG_TYPE_PLDM && len >= 4 && !(m[1] & PLDM_RQ) &&
		 m[3] == ctx->expect_cmd)
		record_resp(ctx, src_eid, m, len);
}

// One receive cycle: drain any pending Aardvark slave reception into libmctp.
static void pump_rx(struct app_ctx *ctx, int timeout_ms)
{
	int r = aa_async_poll(ctx->aa, timeout_ms);
	if (r == AA_ASYNC_NO_DATA)
		return;
	if (r & AA_ASYNC_I2C_READ) {
		uint8_t rx[1024];
		u08 saddr = 0;
		int n = aa_i2c_slave_read(ctx->aa, &saddr,
					  (u16)(sizeof(rx) - 1), rx + 1);
		if (n > 0) {
			rx[0] = (uint8_t)(saddr << 1); // reconstruct dest byte
			size_t total = (size_t)n + 1;
			if (ctx->verbose)
				hexdump("<< RX wire", rx, total);
			// Note DUT-initiated requests (TO=1) for the live test.
			if (total >= 8 && (rx[7] & MCTP_FLAG_TO))
				ctx->req_seen++;
			if (ctx->pec && total >= 1) {
				uint8_t want = crc8_smbus(rx, total - 1);
				uint8_t got = rx[total - 1];
				if (want != got)
					printf("   (PEC mismatch got 0x%02x "
					       "want 0x%02x)\n", got, want);
				total--; // strip PEC before libmctp
			}
			mctp_i2c_rx(ctx->i2c, rx, total);
		}
	}
	if (r & AA_ASYNC_I2C_WRITE)
		aa_i2c_slave_write_stats(ctx->aa);
}

// Send a complete MCTP message and wait for a matching response. Shared by the
// MCTP control validator and the PLDM bench (see mctp_test.h).
bool request_wait(struct app_ctx *ctx, uint8_t dst_eid, const uint8_t *msg,
		  size_t msg_len, uint8_t expect_msgtype, uint8_t expect_cmd,
		  int timeout_ms)
{
	ctx->resp_got = 0;
	ctx->resp_len = 0;
	ctx->expect_msgtype = expect_msgtype;
	ctx->expect_cmd = expect_cmd;
	ctx->last_tx_status = AA_I2C_STATUS_OK;

	int rc = mctp_message_tx(ctx->mctp, dst_eid, true, 0, msg, msg_len);
	if (rc != 0)
		fprintf(stderr, "mctp_message_tx returned %d\n", rc);
	for (int i = 0; i < 1000 && !mctp_is_tx_ready(ctx->mctp, dst_eid); i++)
		mctp_i2c_tx_poll(ctx->i2c);

	// No device acknowledged the address: nothing to wait for.
	if (ctx->last_tx_status == AA_I2C_STATUS_SLA_NACK) {
		ctx->expect_msgtype = 0xff;
		return false;
	}
	uint64_t dl = now_ms(NULL) + (uint64_t)timeout_ms;
	while (now_ms(NULL) < dl && !ctx->resp_got) {
		pump_rx(ctx, 20);
		mctp_i2c_tx_poll(ctx->i2c);
	}
	ctx->expect_msgtype = 0xff;
	return ctx->resp_got;
}

// ===================================================================
// MASTER role: send an MCTP control request and wait for the response.
// ===================================================================
static bool master_request(struct app_ctx *ctx, uint8_t dst_eid, uint8_t cmd,
			   const uint8_t *extra, size_t elen, int timeout_ms)
{
	uint8_t msg[64];
	size_t n = 0;
	msg[n++] = MCTP_MSG_TYPE_CONTROL;
	msg[n++] = MCTP_CTRL_FLAG_REQUEST | (ctx->inst_id++ & 0x1f);
	msg[n++] = cmd;
	if (extra && elen) {
		memcpy(msg + n, extra, elen);
		n += elen;
	}
	return request_wait(ctx, dst_eid, msg, n, MCTP_MSG_TYPE_CONTROL, cmd,
			    timeout_ms);
}

static void run_master_tests(struct app_ctx *ctx, uint8_t dst_eid,
			     int timeout_ms, struct results *r)
{
	printf("\n== MASTER role: querying DUT (EID %u) ==\n", dst_eid);

	// Get Endpoint ID
	bool ok = master_request(ctx, dst_eid, MCTP_CTRL_CMD_GET_EID, NULL, 0,
				 timeout_ms);
	if (ok) {
		uint8_t cc = ctx->resp_len >= 1 ? ctx->resp_buf[3] : 0xff;
		uint8_t eid = ctx->resp_len >= 5 ? ctx->resp_buf[4] : 0;
		check(r, "Get Endpoint ID", cc == 0 && ctx->resp_len >= 5,
		      "cc=0x%02x eid=0x%02x (from EID %u)", cc, eid,
		      ctx->resp_src_eid);
	} else {
		check(r, "Get Endpoint ID", false, "no response (timeout)");
	}

	// Get MCTP Version Support (request base-spec version: type 0xff)
	uint8_t verarg = 0xff;
	ok = master_request(ctx, dst_eid, MCTP_CTRL_CMD_GET_VERSION, &verarg, 1,
			    timeout_ms);
	if (ok) {
		uint8_t cc = ctx->resp_len >= 4 ? ctx->resp_buf[3] : 0xff;
		check(r, "Get MCTP Version Support", cc == 0, "cc=0x%02x", cc);
	} else {
		check(r, "Get MCTP Version Support", false, "no response");
	}

	// Get Message Type Support
	ok = master_request(ctx, dst_eid, MCTP_CTRL_CMD_GET_TYPES, NULL, 0,
			    timeout_ms);
	if (ok) {
		uint8_t cc = ctx->resp_len >= 4 ? ctx->resp_buf[3] : 0xff;
		uint8_t cnt = ctx->resp_len >= 5 ? ctx->resp_buf[4] : 0;
		check(r, "Get Message Type Support", cc == 0,
		      "cc=0x%02x type_count=%u", cc, cnt);
	} else {
		check(r, "Get Message Type Support", false, "no response");
	}
}

// ===================================================================
// SLAVE role: inject a synthetic request as if from a remote master, then
// validate the auto-generated response captured from the tx callback.
// ===================================================================
static bool slave_selftest(struct app_ctx *ctx, const char *name, uint8_t cmd,
			   const uint8_t *extra, size_t elen, struct results *r)
{
	const uint8_t rem_addr = 0x55; // arbitrary fake remote master
	const uint8_t rem_eid = 200;

	// Build MCTP control request message.
	uint8_t msg[64];
	size_t mn = 0;
	msg[mn++] = MCTP_MSG_TYPE_CONTROL;
	msg[mn++] = MCTP_CTRL_FLAG_REQUEST | 0x07;
	msg[mn++] = cmd;
	if (extra && elen) {
		memcpy(msg + mn, extra, elen);
		mn += elen;
	}

	// Wrap in an MCTP-over-I2C frame addressed TO us, with TO=1.
	uint8_t f[80];
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

	// Inject and capture the response (no real I2C traffic).
	ctx->capture = 1;
	ctx->cap_len = 0;
	mctp_i2c_rx(ctx->i2c, f, fn);
	for (int i = 0; i < 4000 && ctx->cap_len == 0; i++)
		mctp_i2c_tx_poll(ctx->i2c);
	ctx->capture = 0;

	if (ctx->cap_len == 0) {
		check(r, name, false, "endpoint produced no response");
		return false;
	}

	const uint8_t *c = ctx->cap_buf;
	if (ctx->verbose)
		hexdump("   captured resp frame", c, ctx->cap_len);

	// Parse: [0]dest [1]0x0f [2]bytecount [3]source [4]ver [5]dest_eid
	//        [6]src_eid [7]flags [8]msgtype [9]rq/d/inst [10]cmd [11]cc...
	bool ok = ctx->cap_len >= 12 && c[1] == 0x0f &&
		  (c[0] >> 1) == rem_addr &&	  // replied to the requester
		  c[5] == rem_eid &&		  // dest EID = original src
		  c[6] == ctx->own_eid &&	  // src EID = us
		  c[8] == MCTP_MSG_TYPE_CONTROL &&
		  !(c[9] & MCTP_CTRL_FLAG_REQUEST) && // Rq=0 (response)
		  c[10] == cmd &&		  // same command
		  c[11] == 0;			  // completion code SUCCESS
	uint8_t cc = ctx->cap_len >= 12 ? c[11] : 0xff;
	check(r, name, ok, "responded to EID %u, cmd=0x%02x cc=0x%02x", rem_eid,
	      cmd, cc);
	return ok;
}

static void run_slave_tests(struct app_ctx *ctx, struct results *r)
{
	printf("\n== SLAVE role: endpoint responds to injected requests ==\n");
	slave_selftest(ctx, "Resp Get Endpoint ID", MCTP_CTRL_CMD_GET_EID, NULL,
		       0, r);
	uint8_t verarg = 0xff;
	slave_selftest(ctx, "Resp Get MCTP Version", MCTP_CTRL_CMD_GET_VERSION,
		       &verarg, 1, r);
	slave_selftest(ctx, "Resp Get Msg Type Support",
		       MCTP_CTRL_CMD_GET_TYPES, NULL, 0, r);
}

static void run_live_listen(struct app_ctx *ctx, int secs)
{
	printf("\n== LIVE: listening %ds for DUT-initiated requests ==\n", secs);
	ctx->req_seen = 0;
	uint64_t dl = now_ms(NULL) + (uint64_t)secs * 1000;
	while (now_ms(NULL) < dl) {
		pump_rx(ctx, 50);
		mctp_i2c_tx_poll(ctx->i2c);
	}
	printf("  observed %d incoming request(s); endpoint auto-responded to "
	       "any control requests.\n",
	       ctx->req_seen);
}

// ===================================================================
// AUTO-DISCOVERY (OpenBMC mctpd-style enumeration)
//
// As the bus owner we address an as-yet-unknown endpoint by its physical I2C
// address with destination EID 0 (NULL), learn its EID, optionally assign one
// with Set Endpoint ID, then query its capabilities. This mirrors what mctpd
// does when it enrols an endpoint.
// ===================================================================

// Directly install a neighbour mapping, bypassing mctp_i2c_set_neighbour()'s
// eid>=8 validation so we can route to NULL EID (0) during discovery.
static void neigh_set_raw(struct mctp_binding_i2c *i2c, uint8_t eid,
			  uint8_t addr)
{
	struct mctp_i2c_neigh *slot = NULL;
	for (size_t i = 0; i < MCTP_I2C_NEIGH_COUNT; i++) {
		struct mctp_i2c_neigh *n = &i2c->neigh[i];
		if (n->used && n->eid == eid) {
			slot = n;
			break;
		}
		if (!n->used && !slot)
			slot = n;
	}
	if (!slot)
		slot = &i2c->neigh[0]; // evict slot 0 if table full
	slot->used = true;
	slot->eid = eid;
	slot->addr = addr;
	slot->last_seen_timestamp = 0;
}

// Probe one physical address. Returns true if an MCTP endpoint answered.
static bool discover_endpoint(struct app_ctx *ctx, uint8_t phys,
			      int assign_eid, int timeout_ms, struct results *r,
			      bool quiet)
{
	// Address the unknown endpoint physically via NULL EID.
	neigh_set_raw(ctx->i2c, MCTP_EID_NULL_LOCAL, phys);
	bool ok = master_request(ctx, MCTP_EID_NULL_LOCAL, MCTP_CTRL_CMD_GET_EID,
				 NULL, 0, timeout_ms);
	if (!ok) {
		if (!quiet)
			printf("  0x%02x: no endpoint\n", phys);
		return false;
	}
	uint8_t cur_eid = ctx->resp_len >= 5 ? ctx->resp_buf[4] : 0;
	uint8_t ep_type = ctx->resp_len >= 6 ? ctx->resp_buf[5] : 0;
	printf("  0x%02x: endpoint found, current EID=0x%02x type=0x%02x\n",
	       phys, cur_eid, ep_type);

	uint8_t eid = cur_eid;

	// Optionally assign an EID (Set Endpoint ID, operation 0 = Set).
	if (assign_eid > 0) {
		uint8_t body[2] = { 0x00, (uint8_t)assign_eid };
		// Route via current EID if it has one, else stay on NULL.
		uint8_t route = cur_eid >= 8 ? cur_eid : MCTP_EID_NULL_LOCAL;
		if (cur_eid >= 8)
			neigh_set_raw(ctx->i2c, cur_eid, phys);
		ok = master_request(ctx, route, MCTP_CTRL_CMD_SET_EID, body, 2,
				    timeout_ms);
		uint8_t cc = (ok && ctx->resp_len >= 4) ? ctx->resp_buf[3] :
							  0xff;
		if (r)
			check(r, "Set Endpoint ID", ok && cc == 0,
			      "assigned 0x%02x to 0x%02x (cc=0x%02x)",
			      assign_eid, phys, cc);
		else
			printf("    Set Endpoint ID -> 0x%02x cc=0x%02x\n",
			       assign_eid, cc);
		if (ok && cc == 0)
			eid = (uint8_t)assign_eid;
	}

	// Map the (now known) EID and query capabilities.
	if (eid >= 8)
		neigh_set_raw(ctx->i2c, eid, phys);
	uint8_t query_eid = eid >= 8 ? eid : MCTP_EID_NULL_LOCAL;

	uint8_t verarg = 0xff;
	bool vok = master_request(ctx, query_eid, MCTP_CTRL_CMD_GET_VERSION,
				  &verarg, 1, timeout_ms);
	printf("    Get MCTP Version       : %s\n",
	       vok && ctx->resp_len >= 4 && ctx->resp_buf[3] == 0 ? "ok" :
								    "n/a");

	bool tok = master_request(ctx, query_eid, MCTP_CTRL_CMD_GET_TYPES, NULL,
				  0, timeout_ms);
	if (tok && ctx->resp_len >= 5 && ctx->resp_buf[3] == 0) {
		uint8_t cnt = ctx->resp_buf[4];
		printf("    Get Message Type Support: cc=0x00 count=%u types=[",
		       cnt);
		for (uint8_t i = 0; i < cnt && (size_t)(5 + i) < ctx->resp_len;
		     i++)
			printf("%s0x%02x", i ? " " : "", ctx->resp_buf[5 + i]);
		printf("]\n");
	} else {
		printf("    Get Message Type Support: n/a\n");
	}

	bool uok = master_request(ctx, query_eid, MCTP_CTRL_CMD_GET_UUID, NULL,
				  0, timeout_ms);
	printf("    Get Endpoint UUID      : %s\n",
	       uok && ctx->resp_len >= 4 && ctx->resp_buf[3] == 0 ?
		       "ok" :
		       "n/a (optional)");

	if (r)
		check(r, "Discover endpoint", true, "EID 0x%02x at addr 0x%02x",
		      eid, phys);
	return true;
}

// Scan the bus and return the first endpoint's physical address and EID.
static bool auto_find_first(struct app_ctx *ctx, int timeout_ms,
			    uint8_t *out_addr, uint8_t *out_eid)
{
	int to = timeout_ms < 200 ? timeout_ms : 200;
	ctx->quiet = 1;
	bool found = false;
	for (uint8_t a = 0x08; a <= 0x77; a++) {
		if (a == ctx->own_addr)
			continue;
		neigh_set_raw(ctx->i2c, MCTP_EID_NULL_LOCAL, a);
		if (master_request(ctx, MCTP_EID_NULL_LOCAL,
				   MCTP_CTRL_CMD_GET_EID, NULL, 0, to) &&
		    ctx->resp_len >= 5) {
			*out_addr = a;
			*out_eid = ctx->resp_buf[4];
			found = true;
			break;
		}
	}
	ctx->quiet = 0;
	return found;
}

// Learn the EID of the endpoint at a known physical address (NULL-EID Get EID).
static bool probe_eid_at(struct app_ctx *ctx, uint8_t addr, int timeout_ms,
			 uint8_t *out_eid)
{
	neigh_set_raw(ctx->i2c, MCTP_EID_NULL_LOCAL, addr);
	if (master_request(ctx, MCTP_EID_NULL_LOCAL, MCTP_CTRL_CMD_GET_EID, NULL,
			   0, timeout_ms) &&
	    ctx->resp_len >= 5) {
		*out_eid = ctx->resp_buf[4];
		return true;
	}
	return false;
}

static void run_discovery(struct app_ctx *ctx, uint8_t peer_addr, int scan,
			  int assign_eid, int timeout_ms, struct results *r)
{
	printf("\n== AUTO-DISCOVERY (OpenBMC mctpd-style) ==\n");
	int found = 0;
	if (scan) {
		printf("Scanning I2C addresses 0x08..0x77 ...\n");
		// Per-address probe is fast: absent addresses NACK immediately.
		int to = timeout_ms < 200 ? timeout_ms : 200;
		ctx->quiet = 1; // silence the flood of expected NACKs
		for (uint8_t a = 0x08; a <= 0x77; a++) {
			if (a == ctx->own_addr)
				continue; // don't probe ourselves
			if (discover_endpoint(ctx, a, 0, to, NULL, true))
				found++;
		}
		ctx->quiet = 0;
	} else {
		if (discover_endpoint(ctx, peer_addr, assign_eid, timeout_ms, r,
				      false))
			found++;
	}
	printf("\nDiscovered %d endpoint(s).\n", found);
}

// ===================================================================
// Interactive shell (-i)
// ===================================================================
static void handle_line(struct app_ctx *ctx, char *line, uint8_t dflt_eid)
{
	char *tok = strtok(line, " \t\r\n");
	if (!tok)
		return;
	if (!strcmp(tok, "q")) {
		exit(0);
	} else if (!strcmp(tok, "g")) {
		char *e = strtok(NULL, " \t\r\n");
		master_request(ctx, e ? (uint8_t)strtol(e, NULL, 0) : dflt_eid,
			       MCTP_CTRL_CMD_GET_EID, NULL, 0, 1000);
	} else if (!strcmp(tok, "ver")) {
		char *e = strtok(NULL, " \t\r\n");
		uint8_t a = 0xff;
		master_request(ctx, e ? (uint8_t)strtol(e, NULL, 0) : dflt_eid,
			       MCTP_CTRL_CMD_GET_VERSION, &a, 1, 1000);
	} else if (!strcmp(tok, "types")) {
		char *e = strtok(NULL, " \t\r\n");
		master_request(ctx, e ? (uint8_t)strtol(e, NULL, 0) : dflt_eid,
			       MCTP_CTRL_CMD_GET_TYPES, NULL, 0, 1000);
	} else {
		printf("commands: g [eid] | ver [eid] | types [eid] | q\n");
	}
	printf("> ");
	fflush(stdout);
}

static void run_interactive(struct app_ctx *ctx, uint8_t dflt_eid)
{
	printf("\nInteractive: g [eid] | ver [eid] | types [eid] | q\n"
	       "(also auto-responds to incoming requests)\n> ");
	fflush(stdout);
	struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
	for (;;) {
		pump_rx(ctx, 20);
		mctp_i2c_tx_poll(ctx->i2c);
		if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
			char line[512];
			if (fgets(line, sizeof(line), stdin))
				handle_line(ctx, line, dflt_eid);
			else
				break;
		}
	}
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -p port   Aardvark port             (default %d)\n"
		"  -b kHz    I2C bitrate               (default %d)\n"
		"  -s addr   our 7-bit I2C address     (default 0x%02x)\n"
		"  -e eid    our MCTP EID              (default %d)\n"
		"  -d addr   peer 7-bit I2C address    (default: auto-discover)\n"
		"  -E eid    peer MCTP EID             (default: auto-learn)\n"
		"  -t ms     master response timeout   (default 1000)\n"
		"  -C        use SMBus PEC (CRC-8)\n"
		"  -u        enable I2C pullups\n"
		"  -P        enable target power (5V)\n"
		"  -m        run MASTER tests only\n"
		"  -S        run SLAVE tests only\n"
		"  -A        AUTO-DISCOVER the peer (OpenBMC-style enumeration)\n"
		"  -R        scan the whole I2C bus (0x08..0x77) for endpoints\n"
		"  -x eid    assign this EID during discovery (Set Endpoint ID)\n"
		"  -G        PLDM bench: discover DUT's PLDM + responder self-tests\n"
		"  -L secs   also LIVE-listen for DUT requests\n"
		"  -i        interactive shell (no validator)\n"
		"  -v        verbose (wire dumps + libmctp debug)\n",
		prog, DEF_PORT, DEF_BITRATE, DEF_OWN_ADDR, DEF_OWN_EID);
}

int main(int argc, char **argv)
{
	int port = DEF_PORT, bitrate = DEF_BITRATE, timeout = 1000;
	int own_addr = DEF_OWN_ADDR, own_eid = DEF_OWN_EID;
	int dst_addr = -1, dst_eid = -1; // <0 => auto-discover
	int power = 0, pullup = 0, verbose = 0, pec = 0;
	int only_master = 0, only_slave = 0, interactive = 0, live = 0;
	int discover = 0, scan = 0, assign_eid = 0, pldm = 0;
	int opt;

	while ((opt = getopt(argc, argv, "p:b:s:e:d:E:t:L:x:CuPmSARGivh")) != -1) {
		switch (opt) {
		case 'p': port = (int)strtol(optarg, NULL, 0); break;
		case 'b': bitrate = (int)strtol(optarg, NULL, 0); break;
		case 's': own_addr = (int)strtol(optarg, NULL, 0); break;
		case 'e': own_eid = (int)strtol(optarg, NULL, 0); break;
		case 'd': dst_addr = (int)strtol(optarg, NULL, 0); break;
		case 'E': dst_eid = (int)strtol(optarg, NULL, 0); break;
		case 't': timeout = (int)strtol(optarg, NULL, 0); break;
		case 'L': live = (int)strtol(optarg, NULL, 0); break;
		case 'C': pec = 1; break;
		case 'u': pullup = 1; break;
		case 'P': power = 1; break;
		case 'm': only_master = 1; break;
		case 'S': only_slave = 1; break;
		case 'A': discover = 1; break;
		case 'R': discover = 1; scan = 1; break;
		case 'x': assign_eid = (int)strtol(optarg, NULL, 0); break;
		case 'G': pldm = 1; break;
		case 'i': interactive = 1; break;
		case 'v': verbose = 1; break;
		default: usage(argv[0]); return 1;
		}
	}

	printf("MCTP-over-I2C validator via Aardvark\n");
	printf("  port=%d bitrate=%dkHz pec=%s\n", port, bitrate,
	       pec ? "on" : "off");
	printf("  self : addr=0x%02x eid=%d\n", own_addr, own_eid);
	if (dst_addr < 0)
		printf("  peer : auto-discover\n");
	else if (dst_eid < 0)
		printf("  peer : addr=0x%02x eid=auto\n", dst_addr);
	else
		printf("  peer : addr=0x%02x eid=%d\n", dst_addr, dst_eid);

	Aardvark aa = aa_open(port);
	if (aa <= 0) {
		fprintf(stderr, "aa_open(port=%d) failed: %d\n", port, aa);
		return 1;
	}
	aa_configure(aa, AA_CONFIG_GPIO_I2C);
	if (power)
		aa_target_power(aa, AA_TARGET_POWER_BOTH);
	if (pullup)
		aa_i2c_pullup(aa, AA_I2C_PULLUP_BOTH);
	aa_i2c_bitrate(aa, bitrate);
	aa_i2c_slave_enable(aa, (u08)own_addr, 0, 0);

	struct mctp *mctp = mctp_init();
	struct mctp_binding_i2c *i2c = malloc(MCTP_SIZEOF_BINDING_I2C);
	if (!mctp || !i2c) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}

	struct app_ctx ctx = {
		.aa = aa,
		.mctp = mctp,
		.i2c = i2c,
		.verbose = verbose,
		.pec = pec,
		.own_addr = (uint8_t)own_addr,
		.own_eid = (uint8_t)own_eid,
		.inst_id = 0,
		.expect_cmd = EXPECT_NONE,
	};

	mctp_set_now_op(mctp, now_ms, NULL);
	if (verbose)
		mctp_set_log_stdio(MCTP_LOG_DEBUG);

	mctp_i2c_setup(i2c, (uint8_t)own_addr, aa_tx, &ctx);
	mctp_register_bus(mctp, mctp_binding_i2c_core(i2c), (uint8_t)own_eid);
	mctp_set_rx_all(mctp, rx_all, &ctx);

	int rc = 0;

	// "-A" with no explicit address behaves as a full bus scan.
	if (discover && dst_addr < 0)
		scan = 1;

	// Modes that send requests to a specific EID need a concrete target.
	bool need_target = interactive || pldm || (!discover && !only_slave);
	if (need_target) {
		if (dst_addr < 0) {
			uint8_t fa = 0, fe = 0;
			printf("\nNo peer given; scanning bus for an endpoint...\n");
			if (auto_find_first(&ctx, timeout, &fa, &fe)) {
				dst_addr = fa;
				dst_eid = fe;
				printf("Discovered endpoint: addr=0x%02x eid=0x%02x\n",
				       dst_addr, dst_eid);
			} else {
				fprintf(stderr, "no endpoint found "
						"(try -C for PEC, or -d <addr>)\n");
				rc = 1;
			}
		} else if (dst_eid < 0) {
			uint8_t fe = 0;
			if (probe_eid_at(&ctx, (uint8_t)dst_addr, timeout,
					 &fe)) {
				dst_eid = fe;
				printf("\nLearned EID 0x%02x at addr 0x%02x\n",
				       dst_eid, dst_addr);
			} else {
				fprintf(stderr,
					"no endpoint at 0x%02x (try -C)\n",
					dst_addr);
				rc = 1;
			}
		}
		if (rc == 0)
			mctp_i2c_set_neighbour(i2c, (uint8_t)dst_eid,
					       (uint8_t)dst_addr);
	}

	if (rc == 0 && interactive) {
		run_interactive(&ctx, (uint8_t)dst_eid);
	} else if (rc == 0) {
		struct results r = { 0, 0 };
		if (discover) {
			run_discovery(&ctx, (uint8_t)dst_addr, scan, assign_eid,
				      timeout, &r);
		} else if (pldm) {
			run_pldm_bench(&ctx, (uint8_t)dst_eid, timeout, &r);
		} else {
			if (!only_slave)
				run_master_tests(&ctx, (uint8_t)dst_eid,
						 timeout, &r);
			if (!only_master)
				run_slave_tests(&ctx, &r);
		}
		if (live > 0)
			run_live_listen(&ctx, live);

		printf("\n== SUMMARY ==  %d passed, %d failed\n", r.pass,
		       r.fail);
		rc = r.fail ? 1 : 0;
	}

	aa_i2c_slave_disable(aa);
	aa_close(aa);
	free(i2c);
	mctp_destroy(mctp);
	return rc;
}
