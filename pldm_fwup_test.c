// SPDX-License-Identifier: Apache-2.0
//
// PLDM Firmware Update (DSP0267) validator over MCTP-over-I2C, read-only.
//
// The DUT is a Firmware Device (FD); we act as the Update Agent (UA) and issue
// only the non-mutating UA->FD commands, so the DUT's firmware/state is never
// changed:
//
//   QueryDeviceIdentifiers (0x01) -> the descriptors a firmware package is
//                                    matched against (PCI VID/DID, IANA, UUID…)
//   GetFirmwareParameters  (0x02) -> active/pending component versions + caps
//   GetStatus              (0x1b) -> update state machine status (idle if not
//                                    mid-update)
//
// The actual update flow (RequestUpdate/PassComponentTable/UpdateComponent/
// ActivateFirmware) is intentionally NOT exercised here — it changes the DUT
// and can brick it; it needs a real firmware package and the FD-initiated
// RequestFirmwareData loop.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mctp_test.h"

#include <libpldm/base.h>
#include <libpldm/firmware_update.h>
#include <libpldm/pldm_types.h>

#define MCTP_PLDM_OVERHEAD (1 + sizeof(struct pldm_msg_hdr))

static uint8_t pl_iid(struct app_ctx *ctx)
{
	return ctx->inst_id++ & 0x1f;
}

static const struct pldm_msg *pldm_resp(struct app_ctx *ctx, size_t *payload_len)
{
	*payload_len = ctx->resp_len > MCTP_PLDM_OVERHEAD ?
			       ctx->resp_len - MCTP_PLDM_OVERHEAD :
			       0;
	return (const struct pldm_msg *)(ctx->resp_buf + 1);
}

static const char *descriptor_name(uint16_t t)
{
	switch (t) {
	case PLDM_FWUP_PCI_VENDOR_ID:		    return "PCI-VID";
	case PLDM_FWUP_IANA_ENTERPRISE_ID:	    return "IANA";
	case PLDM_FWUP_UUID:			    return "UUID";
	case PLDM_FWUP_ACPI_VENDOR_ID:		    return "ACPI-VID";
	case PLDM_FWUP_PCI_DEVICE_ID:		    return "PCI-DID";
	case PLDM_FWUP_PCI_SUBSYSTEM_VENDOR_ID:	    return "PCI-SubVID";
	case PLDM_FWUP_PCI_SUBSYSTEM_ID:	    return "PCI-SubID";
	default:				    return "Descriptor";
	}
}

static const char *fd_state_name(uint8_t s)
{
	switch (s) {
	case PLDM_FD_STATE_IDLE:	    return "IDLE";
	case PLDM_FD_STATE_LEARN_COMPONENTS: return "LEARN_COMPONENTS";
	case PLDM_FD_STATE_READY_XFER:	    return "READY_XFER";
	case PLDM_FD_STATE_DOWNLOAD:	    return "DOWNLOAD";
	case PLDM_FD_STATE_VERIFY:	    return "VERIFY";
	case PLDM_FD_STATE_APPLY:	    return "APPLY";
	case PLDM_FD_STATE_ACTIVATE:	    return "ACTIVATE";
	default:			    return "?";
	}
}

// Print a descriptor value: PCI IDs as 0x%04x, IANA as a number, else hex.
static void print_descriptor_value(uint16_t type, const uint8_t *v, size_t len)
{
	if ((type == PLDM_FWUP_PCI_VENDOR_ID || type == PLDM_FWUP_PCI_DEVICE_ID ||
	     type == PLDM_FWUP_PCI_SUBSYSTEM_VENDOR_ID ||
	     type == PLDM_FWUP_PCI_SUBSYSTEM_ID) &&
	    len == 2) {
		printf("0x%04x", v[0] | (v[1] << 8));
	} else if (type == PLDM_FWUP_IANA_ENTERPRISE_ID && len == 4) {
		printf("%u", v[0] | (v[1] << 8) | (v[2] << 16) |
				     ((uint32_t)v[3] << 24));
	} else {
		for (size_t i = 0; i < len; i++)
			printf("%02x", v[i]);
	}
}

static void fwup_query_identifiers(struct app_ctx *ctx, uint8_t eid, int to,
				   struct results *r)
{
	uint8_t tx[64];
	struct pldm_msg *req = (struct pldm_msg *)(tx + 1);
	size_t pl;
	const struct pldm_msg *rsp;

	printf("\n-- fw-update: QueryDeviceIdentifiers (0x01) --\n");
	tx[0] = MCTP_MSG_TYPE_PLDM;
	uint8_t cc = 0xff, dcount = 0;
	uint32_t dlen = 0;
	uint8_t *ddata = NULL;
	bool ok = encode_query_device_identifiers_req(
			  pl_iid(ctx), PLDM_QUERY_DEVICE_IDENTIFIERS_REQ_BYTES,
			  req) == 0 &&
		  request_wait(ctx, eid, tx,
			       MCTP_PLDM_OVERHEAD +
				       PLDM_QUERY_DEVICE_IDENTIFIERS_REQ_BYTES,
			       MCTP_MSG_TYPE_PLDM, PLDM_QUERY_DEVICE_IDENTIFIERS,
			       to);
	if (ok) {
		rsp = pldm_resp(ctx, &pl);
		ok = decode_query_device_identifiers_resp(rsp, pl, &cc, &dlen,
							  &dcount, &ddata) == 0;
	}
	check(r, "FWUP QueryDeviceIdentifiers", ok && cc == PLDM_SUCCESS,
	      "cc=0x%02x descriptors=%u", cc, dcount);
	if (!(ok && cc == PLDM_SUCCESS) || !ddata)
		return;

	// Walk the descriptor TLVs: [type(2)][length(2)][value(length)].
	const uint8_t *p = ddata;
	size_t remaining = dlen;
	int parsed = 0;
	for (int i = 0; i < dcount && remaining >= 4; i++) {
		uint16_t dtype = 0;
		struct variable_field val = { 0 };
		if (decode_descriptor_type_length_value(p, remaining, &dtype,
							&val) != 0)
			break;
		printf("   [%d] %-10s : ", i, descriptor_name(dtype));
		print_descriptor_value(dtype, val.ptr, val.length);
		printf("\n");
		size_t consumed = 4 + val.length;
		if (consumed > remaining)
			break;
		p += consumed;
		remaining -= consumed;
		parsed++;
	}
	check(r, "FWUP descriptors parsed", parsed == dcount, "%d of %u", parsed,
	      dcount);
}

static void fwup_get_parameters(struct app_ctx *ctx, uint8_t eid, int to,
				struct results *r)
{
	uint8_t tx[64];
	struct pldm_msg *req = (struct pldm_msg *)(tx + 1);
	size_t pl;
	const struct pldm_msg *rsp;

	printf("\n-- fw-update: GetFirmwareParameters (0x02) --\n");
	tx[0] = MCTP_MSG_TYPE_PLDM;
	struct pldm_get_firmware_parameters_resp fp = { 0 };
	struct variable_field active_set = { 0 }, pending_set = { 0 },
			      comp_table = { 0 };
	bool ok = encode_get_firmware_parameters_req(
			  pl_iid(ctx), PLDM_GET_FIRMWARE_PARAMETERS_REQ_BYTES,
			  req) == 0 &&
		  request_wait(ctx, eid, tx,
			       MCTP_PLDM_OVERHEAD +
				       PLDM_GET_FIRMWARE_PARAMETERS_REQ_BYTES,
			       MCTP_MSG_TYPE_PLDM, PLDM_GET_FIRMWARE_PARAMETERS,
			       to);
	if (ok) {
		rsp = pldm_resp(ctx, &pl);
		ok = decode_get_firmware_parameters_resp(rsp, pl, &fp, &active_set,
							 &pending_set,
							 &comp_table) == 0;
	}
	check(r, "FWUP GetFirmwareParameters", ok && fp.completion_code == PLDM_SUCCESS,
	      "cc=0x%02x components=%u active_set='%.*s'", fp.completion_code,
	      fp.comp_count, (int)active_set.length, active_set.ptr ?
							     (const char *)active_set.ptr :
							     "");
	if (!(ok && fp.completion_code == PLDM_SUCCESS))
		return;

	// Walk the component parameter table, one entry per component.
	const uint8_t *p = comp_table.ptr;
	size_t remaining = comp_table.length;
	int parsed = 0;
	for (int i = 0; i < fp.comp_count && p && remaining > 0; i++) {
		struct pldm_component_parameter_entry e = { 0 };
		struct variable_field active_ver = { 0 }, pending_ver = { 0 };
		if (decode_get_firmware_parameters_resp_comp_entry(
			    p, remaining, &e, &active_ver, &pending_ver) != 0)
			break;
		printf("   comp[%d] class=0x%04x id=0x%04x active_ver='%.*s'\n",
		       i, e.comp_classification, e.comp_identifier,
		       (int)active_ver.length,
		       active_ver.ptr ? (const char *)active_ver.ptr : "");
		// Entry = fixed packed struct + the two version strings. (Using
		// the lengths is robust even when a version string is empty.)
		size_t consumed = sizeof(struct pldm_component_parameter_entry) +
				  e.active_comp_ver_str_len +
				  e.pending_comp_ver_str_len;
		if (consumed > remaining)
			break;
		p += consumed;
		remaining -= consumed;
		parsed++;
	}
	check(r, "FWUP component table parsed", parsed == fp.comp_count,
	      "%d of %u", parsed, fp.comp_count);
}

static void fwup_get_status(struct app_ctx *ctx, uint8_t eid, int to,
			    struct results *r)
{
	uint8_t tx[64];
	struct pldm_msg *req = (struct pldm_msg *)(tx + 1);
	size_t pl;
	const struct pldm_msg *rsp;

	printf("\n-- fw-update: GetStatus (0x1b) --\n");
	tx[0] = MCTP_MSG_TYPE_PLDM;
	uint8_t cc = 0xff, cur = 0xff, prev = 0, aux = 0, aux_st = 0, pct = 0,
		reason = 0;
	bitfield32_t flags = { 0 };
	bool ok = encode_get_status_req(pl_iid(ctx), req,
					PLDM_GET_STATUS_REQ_BYTES) == 0 &&
		  request_wait(ctx, eid, tx,
			       MCTP_PLDM_OVERHEAD + PLDM_GET_STATUS_REQ_BYTES,
			       MCTP_MSG_TYPE_PLDM, PLDM_GET_STATUS, to);
	if (ok) {
		rsp = pldm_resp(ctx, &pl);
		ok = decode_get_status_resp(rsp, pl, &cc, &cur, &prev, &aux,
					    &aux_st, &pct, &reason, &flags) == 0;
	}
	// progressPercent is only meaningful while an operation is running.
	bool in_progress = (cur == PLDM_FD_STATE_DOWNLOAD ||
			    cur == PLDM_FD_STATE_VERIFY ||
			    cur == PLDM_FD_STATE_APPLY);
	char extra[24] = "";
	if (in_progress)
		snprintf(extra, sizeof(extra), " progress=%u%%", pct);
	check(r, "FWUP GetStatus", ok && cc == PLDM_SUCCESS,
	      "cc=0x%02x state=%s(%u)%s", cc, fd_state_name(cur), cur, extra);
}

void run_pldm_fwup_bench(struct app_ctx *ctx, uint8_t dst_eid, int timeout_ms,
			 struct results *r)
{
	printf("\n== PLDM Firmware Update validator (read-only, DSP0267) ==\n");
	fwup_query_identifiers(ctx, dst_eid, timeout_ms, r);
	fwup_get_parameters(ctx, dst_eid, timeout_ms, r);
	fwup_get_status(ctx, dst_eid, timeout_ms, r);
}
