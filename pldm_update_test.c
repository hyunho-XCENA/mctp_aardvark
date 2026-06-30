// SPDX-License-Identifier: Apache-2.0
//
// PLDM firmware update (DSP0267) Update Agent over MCTP-over-I2C.
//
// Parses a PLDM firmware package and runs the full update against the DUT
// (Firmware Device):
//   RequestUpdate -> PassComponentTable -> UpdateComponent
//   -> [FD-initiated] RequestFirmwareData / TransferComplete / VerifyComplete
//      / ApplyComplete   (we serve the image bytes and ack each)
//   -> ActivateFirmware -> GetStatus
//
// WARNING: this writes firmware to the device. The transfer/verify phases are
// recoverable (the FD stays in update mode on failure), but ActivateFirmware
// is not, and the device may reset.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mctp_test.h"

#include <libpldm/base.h>
#include <libpldm/firmware_update.h>
#include <libpldm/pldm_types.h>

#define MCTP_PLDM_OVERHEAD (1 + sizeof(struct pldm_msg_hdr))
// Max bytes we serve per RequestFirmwareData. Bigger = fewer round-trips =
// faster flash, but the resulting frame (8 framing + 5 MCTP/PLDM hdr + data +
// PEC) must fit the DUT's I2C slave RX buffer. This DUT NACKs frames over
// ~128 bytes (it ACKed only 130/237 at data=224), so cap data at 96
// (frame ~109 B). The MCTP BTU (254) is not the limit here, the DUT is.
#define UA_MAX_XFER_SIZE   96
#define UA_XFER_BUDGET_MS  300000 // overall budget for the FD-driven transfer

static uint8_t pl_iid(struct app_ctx *ctx)
{
	return ctx->inst_id++ & 0x1f;
}

static const struct pldm_msg *pldm_resp(struct app_ctx *ctx, size_t *pl)
{
	*pl = ctx->resp_len > MCTP_PLDM_OVERHEAD ?
		      ctx->resp_len - MCTP_PLDM_OVERHEAD :
		      0;
	return (const struct pldm_msg *)(ctx->resp_buf + 1);
}

// ===================================================================
// FD-initiated transfer phase: the DUT sends us RequestFirmwareData /
// TransferComplete / VerifyComplete / ApplyComplete; we serve image bytes and
// acknowledge. Responses are hand-built (we are the responder here).
// ===================================================================
struct fwup_session {
	const uint8_t *image; // component image (package buffer + offset)
	uint32_t comp_size;
	uint32_t served;     // total bytes handed out
	uint32_t high_water; // furthest offset+len requested
	int transfer_done, verify_done, apply_done;
	uint8_t transfer_result, verify_result, apply_result;
	int verbose;
};

void fwup_handle_request(struct app_ctx *ctx, uint8_t src_eid, uint8_t msg_tag,
			 const uint8_t *m, size_t len)
{
	struct fwup_session *s = ctx->fwup;
	if (!s || len < 4)
		return;
	uint8_t inst = m[1] & 0x1f;
	uint8_t cmd = m[3];
	const struct pldm_msg *req = (const struct pldm_msg *)(m + 1);
	size_t plen = len - (1 + sizeof(struct pldm_msg_hdr));

	uint8_t r[8 + UA_MAX_XFER_SIZE + 16];
	size_t n = 0;
	r[n++] = MCTP_MSG_TYPE_PLDM;
	r[n++] = inst;			    // Rq=0, echo instance id
	r[n++] = (0 << 6) | PLDM_FWUP;	    // PLDM type 5
	r[n++] = cmd;

	switch (cmd) {
	case PLDM_REQUEST_FIRMWARE_DATA: {
		uint32_t off = 0, length = 0;
		if (decode_request_firmware_data_req(req, plen, &off, &length) !=
		    0) {
			r[n++] = PLDM_ERROR_INVALID_LENGTH;
			break;
		}
		if ((uint64_t)off + length > s->comp_size ||
		    length > UA_MAX_XFER_SIZE) {
			r[n++] = 0x82; // PLDM_FWUP_DATA_OUT_OF_RANGE
			break;
		}
		r[n++] = PLDM_SUCCESS;
		memcpy(r + n, s->image + off, length);
		n += length;
		s->served += length;
		if (off + length > s->high_water)
			s->high_water = off + length;
		break;
	}
	case PLDM_TRANSFER_COMPLETE:
		s->transfer_result = plen >= 1 ? req->payload[0] : 0xff;
		s->transfer_done = 1;
		r[n++] = PLDM_SUCCESS;
		break;
	case PLDM_VERIFY_COMPLETE:
		s->verify_result = plen >= 1 ? req->payload[0] : 0xff;
		s->verify_done = 1;
		r[n++] = PLDM_SUCCESS;
		break;
	case PLDM_APPLY_COMPLETE:
		s->apply_result = plen >= 1 ? req->payload[0] : 0xff;
		s->apply_done = 1;
		r[n++] = PLDM_SUCCESS;
		break;
	default:
		r[n++] = PLDM_ERROR_UNSUPPORTED_PLDM_CMD;
		break;
	}

	mctp_message_tx(ctx->mctp, src_eid, false, msg_tag, r, n);
}

// ===================================================================
// Package parsing (libpldm decoders).
// ===================================================================
struct fw_package {
	uint8_t *buf;
	long size;
	// parsed component (single component assumed for this DUT)
	uint16_t comp_classification;
	uint16_t comp_identifier;
	uint32_t comp_comparison_stamp;
	uint32_t comp_offset;
	uint32_t comp_size;
	struct variable_field comp_ver;	   // component version string
	struct variable_field set_ver;	   // component image set version string
	uint8_t comp_ver_type, set_ver_type;
};

static bool parse_package(const char *path, struct fw_package *p,
			  struct results *r)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		check(r, "open package", false, "cannot open %s", path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	p->size = ftell(f);
	fseek(f, 0, SEEK_SET);
	p->buf = malloc(p->size);
	bool readok = p->buf && fread(p->buf, 1, p->size, f) == (size_t)p->size;
	fclose(f);
	if (!readok) {
		check(r, "read package", false, "read failed");
		return false;
	}

	struct pldm_package_header_information hdr = { 0 };
	struct variable_field pkgver = { 0 };
	if (decode_pldm_package_header_info(p->buf, p->size, &hdr, &pkgver) != 0) {
		check(r, "package header", false, "decode failed");
		return false;
	}
	printf("   package: format v%u, version '%.*s', bitmap %u bits\n",
	       hdr.package_header_format_version, (int)pkgver.length, pkgver.ptr,
	       hdr.component_bitmap_bit_length);

	size_t off1 = sizeof(struct pldm_package_header_information) +
		      hdr.package_version_string_length;
	uint8_t rec_count = p->buf[off1];
	size_t off2 = off1 + 1;

	struct pldm_firmware_device_id_record fdrec = { 0 };
	struct variable_field applicable = { 0 }, descriptors = { 0 },
			      fwpkgdata = { 0 };
	if (decode_firmware_device_id_record(
		    p->buf + off2, p->size - off2,
		    hdr.component_bitmap_bit_length, &fdrec, &applicable,
		    &p->set_ver, &descriptors, &fwpkgdata) != 0) {
		check(r, "FD ID record", false, "decode failed");
		return false;
	}
	p->set_ver_type = fdrec.comp_image_set_version_string_type;
	printf("   FD records=%u, image-set version '%.*s', %u descriptor(s)\n",
	       rec_count, (int)p->set_ver.length, p->set_ver.ptr,
	       fdrec.descriptor_count);

	size_t off3 = off2 + fdrec.record_length;
	uint16_t comp_count = p->buf[off3] | (p->buf[off3 + 1] << 8);
	size_t off4 = off3 + 2;

	struct pldm_component_image_information cii = { 0 };
	if (decode_pldm_comp_image_info(p->buf + off4, p->size - off4, &cii,
					&p->comp_ver) != 0) {
		check(r, "component image info", false, "decode failed");
		return false;
	}
	p->comp_classification = cii.comp_classification;
	p->comp_identifier = cii.comp_identifier;
	p->comp_comparison_stamp = cii.comp_comparison_stamp;
	p->comp_offset = cii.comp_location_offset;
	p->comp_size = cii.comp_size;
	p->comp_ver_type = cii.comp_version_string_type;
	printf("   components=%u -> class=0x%04x id=0x%04x version '%.*s' "
	       "size=%u offset=%u\n",
	       comp_count, p->comp_classification, p->comp_identifier,
	       (int)p->comp_ver.length, p->comp_ver.ptr, p->comp_size,
	       p->comp_offset);

	bool sane = (uint64_t)p->comp_offset + p->comp_size <= (uint64_t)p->size;
	check(r, "package parsed + image in range", sane,
	      "image [%u,%u) of %ld-byte file", p->comp_offset,
	      p->comp_offset + p->comp_size, p->size);
	return sane;
}

// ===================================================================
// Update Agent flow.
// ===================================================================
int run_pldm_update(struct app_ctx *ctx, uint8_t dst_eid, const char *pkg_path,
		    int timeout_ms, struct results *r)
{
	uint8_t tx[128];
	struct pldm_msg *req = (struct pldm_msg *)(tx + 1);
	size_t pl;
	const struct pldm_msg *rsp;
	int to = timeout_ms;

	printf("\n== PLDM firmware update (Update Agent, DSP0267) ==\n");
	printf("   package: %s\n", pkg_path);

	struct fw_package pkg = { 0 };
	if (!parse_package(pkg_path, &pkg, r))
		return -1;

	// RequestUpdate
	tx[0] = MCTP_MSG_TYPE_PLDM;
	uint8_t cc = 0xff;
	uint16_t fd_meta_len = 0;
	uint8_t fd_will_send_pkg = 0;
	size_t ru_len = 11 + pkg.set_ver.length;
	// maxOutstandingTransferRequests = 1: this FD requests one chunk at a
	// time (tested 4, no change), so the transfer is paced by the FD.
	bool ok = encode_request_update_req(
			  pl_iid(ctx), UA_MAX_XFER_SIZE, 1, 1, 0,
			  pkg.set_ver_type, pkg.set_ver.length, &pkg.set_ver,
			  req, ru_len) == 0 &&
		  request_wait(ctx, dst_eid, tx, MCTP_PLDM_OVERHEAD + ru_len,
			       MCTP_MSG_TYPE_PLDM, PLDM_REQUEST_UPDATE, to);
	if (ok) {
		rsp = pldm_resp(ctx, &pl);
		ok = decode_request_update_resp(rsp, pl, &cc, &fd_meta_len,
						&fd_will_send_pkg) == 0;
	}
	check(r, "RequestUpdate", ok && cc == PLDM_SUCCESS, "cc=0x%02x", cc);
	if (!(ok && cc == PLDM_SUCCESS))
		goto done;

	// PassComponentTable (single component => Start and End)
	tx[0] = MCTP_MSG_TYPE_PLDM;
	cc = 0xff;
	uint8_t comp_resp = 0, comp_resp_code = 0;
	size_t pc_len = 12 + pkg.comp_ver.length;
	ok = encode_pass_component_table_req(
		     pl_iid(ctx), PLDM_START_AND_END, pkg.comp_classification,
		     pkg.comp_identifier, 0, pkg.comp_comparison_stamp,
		     pkg.comp_ver_type, pkg.comp_ver.length, &pkg.comp_ver, req,
		     pc_len) == 0 &&
	     request_wait(ctx, dst_eid, tx, MCTP_PLDM_OVERHEAD + pc_len,
			  MCTP_MSG_TYPE_PLDM, PLDM_PASS_COMPONENT_TABLE, to);
	if (ok) {
		rsp = pldm_resp(ctx, &pl);
		ok = decode_pass_component_table_resp(rsp, pl, &cc, &comp_resp,
						      &comp_resp_code) == 0;
	}
	check(r, "PassComponentTable", ok && cc == PLDM_SUCCESS,
	      "cc=0x%02x comp_resp=%u code=0x%02x", cc, comp_resp,
	      comp_resp_code);
	if (!(ok && cc == PLDM_SUCCESS))
		goto cancel;

	// UpdateComponent
	tx[0] = MCTP_MSG_TYPE_PLDM;
	cc = 0xff;
	uint8_t compat_resp = 0, compat_code = 0;
	bitfield32_t flags_enabled = { 0 };
	uint16_t time_before = 0;
	bitfield32_t upd_flags = { 0 };
	size_t uc_len = 19 + pkg.comp_ver.length;
	ok = encode_update_component_req(
		     pl_iid(ctx), pkg.comp_classification, pkg.comp_identifier,
		     0, pkg.comp_comparison_stamp, pkg.comp_size, upd_flags,
		     pkg.comp_ver_type, pkg.comp_ver.length, &pkg.comp_ver, req,
		     uc_len) == 0 &&
	     request_wait(ctx, dst_eid, tx, MCTP_PLDM_OVERHEAD + uc_len,
			  MCTP_MSG_TYPE_PLDM, PLDM_UPDATE_COMPONENT, to);
	if (ok) {
		rsp = pldm_resp(ctx, &pl);
		ok = decode_update_component_resp(rsp, pl, &cc, &compat_resp,
						  &compat_code, &flags_enabled,
						  &time_before) == 0;
	}
	check(r, "UpdateComponent", ok && cc == PLDM_SUCCESS,
	      "cc=0x%02x compat=%u code=0x%02x", cc, compat_resp, compat_code);
	if (!(ok && cc == PLDM_SUCCESS))
		goto cancel;

	// FD-initiated transfer phase: serve firmware data until ApplyComplete.
	printf("\n   transfer phase: serving %u bytes in <=%u-byte chunks...\n",
	       pkg.comp_size, UA_MAX_XFER_SIZE);
	struct fwup_session s = { .image = pkg.buf + pkg.comp_offset,
				  .comp_size = pkg.comp_size,
				  .verbose = ctx->verbose };
	ctx->fwup = &s;
	uint32_t last_report = 0;
	int idle = 0;
	for (int i = 0; i < UA_XFER_BUDGET_MS / 20 && !s.apply_done; i++) {
		uint32_t before = s.served;
		mctp_pump(ctx, 20);
		if (s.served == before) {
			idle++;
		} else {
			idle = 0;
		}
		if (s.high_water - last_report >= pkg.comp_size / 10 ||
		    (s.transfer_done && last_report == 0)) {
			printf("      %u / %u bytes (%u%%)%s\n", s.high_water,
			       pkg.comp_size,
			       pkg.comp_size ? s.high_water * 100 / pkg.comp_size :
					       0,
			       s.transfer_done ? " [transfer complete]" : "");
			last_report = s.high_water;
		}
		if (idle > 500) // ~10s with no activity
			break;
	}
	ctx->fwup = NULL;

	check(r, "firmware transfer", s.transfer_done && s.transfer_result == 0,
	      "served %u bytes, result=0x%02x", s.served, s.transfer_result);
	check(r, "firmware verify", s.verify_done && s.verify_result == 0,
	      "result=0x%02x", s.verify_result);
	check(r, "firmware apply", s.apply_done && s.apply_result == 0,
	      "result=0x%02x", s.apply_result);
	if (!(s.apply_done && s.apply_result == 0))
		goto cancel;

	// ActivateFirmware (self-contained activation).
	tx[0] = MCTP_MSG_TYPE_PLDM;
	cc = 0xff;
	uint16_t est_time = 0;
	ok = encode_activate_firmware_req(pl_iid(ctx), 1, req, 1) == 0 &&
	     request_wait(ctx, dst_eid, tx, MCTP_PLDM_OVERHEAD + 1,
			  MCTP_MSG_TYPE_PLDM, PLDM_ACTIVATE_FIRMWARE, to);
	if (ok) {
		rsp = pldm_resp(ctx, &pl);
		ok = decode_activate_firmware_resp(rsp, pl, &cc, &est_time) == 0;
	}
	check(r, "ActivateFirmware", ok && cc == PLDM_SUCCESS,
	      "cc=0x%02x est_activation=%us", cc, est_time);

	printf("\n   firmware update complete; the DUT may reset to activate "
	       "%.*s.\n",
	       (int)pkg.comp_ver.length, pkg.comp_ver.ptr);
	free(pkg.buf);
	return 0;

cancel:
	// Best-effort: back the FD out of update mode so it isn't left stuck.
	ctx->fwup = NULL;
	tx[0] = MCTP_MSG_TYPE_PLDM;
	if (encode_cancel_update_req(pl_iid(ctx), req,
				     PLDM_CANCEL_UPDATE_REQ_BYTES) == 0)
		request_wait(ctx, dst_eid, tx,
			     MCTP_PLDM_OVERHEAD + PLDM_CANCEL_UPDATE_REQ_BYTES,
			     MCTP_MSG_TYPE_PLDM, PLDM_CANCEL_UPDATE, to);
	printf("   update aborted; sent CancelUpdate to leave the FD idle.\n");
done:
	free(pkg.buf);
	return -1;
}
