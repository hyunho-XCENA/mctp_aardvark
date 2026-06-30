// SPDX-License-Identifier: Apache-2.0
//
// PLDM Platform (DSP0248) validator over MCTP-over-I2C.
//
// The DUT advertises PLDM type 2 (Platform) with a PDR repository plus state
// sensors, numeric sensors and state effecters. This validator behaves like a
// PLDM monitoring agent: it reads the PDR repository (GetPDRRepositoryInfo +
// GetPDR), discovers every sensor/effecter from the PDRs, then exercises the
// per-object read commands. With allow_writes it also round-trips the Set*
// commands by writing back the value it just read, so the DUT's live state is
// never actually changed.
//
// Commands covered (DUT GetPLDMCommands for type 2 = 0x0b,10,11,12,13,21,39,3a,50,51):
//   read : GetPDRRepositoryInfo(0x50) GetPDR(0x51) GetSensorReading(0x11)
//          GetStateSensorReadings(0x21) GetSensorThresholds(0x12)
//          GetStateEffecterStates(0x3a)
//   write: SetNumericSensorEnable(0x10) SetSensorThresholds(0x13)
//          SetStateEffecterStates(0x39)   [round-trip, value preserved]
//
// GetSensorThresholds/SetSensorThresholds/SetNumericSensorEnable have no
// libpldm encode/decode helpers, so those are hand-built on the wire.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mctp_test.h"

#include <libpldm/base.h>
#include <libpldm/platform.h>
#include <libpldm/pdr.h>
#include <libpldm/pldm_types.h>

#define MCTP_PLDM_OVERHEAD (1 + sizeof(struct pldm_msg_hdr))

// Hand-built Platform commands (no libpldm helper).
#define PLDM_SET_NUMERIC_SENSOR_ENABLE 0x10
#define PLDM_GET_SENSOR_THRESHOLDS     0x12
#define PLDM_SET_SENSOR_THRESHOLDS     0x13

#define MAX_OBJ 32 // sensors / effecters we track per kind

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

// Bytes per PLDM sensorDataSize enum value.
static size_t sds_len(uint8_t sds)
{
	switch (sds) {
	case PLDM_SENSOR_DATA_SIZE_UINT8:
	case PLDM_SENSOR_DATA_SIZE_SINT8:
		return 1;
	case PLDM_SENSOR_DATA_SIZE_UINT16:
	case PLDM_SENSOR_DATA_SIZE_SINT16:
		return 2;
	case PLDM_SENSOR_DATA_SIZE_UINT32:
	case PLDM_SENSOR_DATA_SIZE_SINT32:
		return 4;
	default:
		return 0;
	}
}

// Build a hand-rolled PLDM Platform request into tx (tx[0]=MCTP type), payload
// of plen bytes copied after the 4-byte (MCTP+PLDM) header. Returns total len.
static size_t pldm_build(uint8_t *tx, struct app_ctx *ctx, uint8_t cmd,
			 const uint8_t *payload, size_t plen)
{
	tx[0] = MCTP_MSG_TYPE_PLDM;
	tx[1] = PLDM_RQ | pl_iid(ctx);
	tx[2] = (0 << 6) | PLDM_PLATFORM;
	tx[3] = cmd;
	if (payload && plen)
		memcpy(tx + 4, payload, plen);
	return 4 + plen;
}

// ===================================================================
// Discovered objects, harvested from the PDR repository.
// ===================================================================
struct obj {
	uint16_t id;	    // sensor / effecter id
	uint8_t comp_count; // composite sensor/effecter count
	// Numeric sensors only: conversion parameters from the PDR, used to
	// turn a raw GetSensorReading into a real engineering value:
	//   value = (raw * resolution + offset) * 10^unit_modifier
	uint8_t base_unit;
	uint8_t sensor_data_size;
	int8_t unit_modifier;
	float resolution;
	float offset;
};

// Short names for the PLDM base units we expect from this DUT.
static const char *unit_name(uint8_t u)
{
	switch (u) {
	case PLDM_SENSOR_UNIT_NONE:	 return "";
	case PLDM_SENSOR_UNIT_DEGRESS_C: return "C";
	case PLDM_SENSOR_UNIT_DEGRESS_F: return "F";
	case PLDM_SENSOR_UNIT_KELVINS:	 return "K";
	case PLDM_SENSOR_UNIT_VOLTS:	 return "V";
	case PLDM_SENSOR_UNIT_AMPS:	 return "A";
	case PLDM_SENSOR_UNIT_WATTS:	 return "W";
	case PLDM_SENSOR_UNIT_JOULES:	 return "J";
	case PLDM_SENSOR_UNIT_RPM:	 return "RPM";
	case PLDM_SENSOR_UNIT_HERTZ:	 return "Hz";
	case PLDM_SENSOR_UNIT_KPA:	 return "kPa";
	case PLDM_SENSOR_UNIT_PSI:	 return "psi";
	case PLDM_SENSOR_UNIT_CFM:	 return "CFM";
	default:			 return "?";
	}
}

// 10^e for small integer e, without pulling in libm.
static double pow10i(int e)
{
	double r = 1.0;
	while (e > 0) {
		r *= 10.0;
		e--;
	}
	while (e < 0) {
		r /= 10.0;
		e++;
	}
	return r;
}

// Interpret raw GetSensorReading bytes (little-endian) per sensorDataSize.
static double raw_to_signed(const uint8_t *b, uint8_t sds)
{
	switch (sds) {
	case PLDM_SENSOR_DATA_SIZE_UINT8:
		return b[0];
	case PLDM_SENSOR_DATA_SIZE_SINT8:
		return (int8_t)b[0];
	case PLDM_SENSOR_DATA_SIZE_UINT16:
		return (uint16_t)(b[0] | (b[1] << 8));
	case PLDM_SENSOR_DATA_SIZE_SINT16:
		return (int16_t)(b[0] | (b[1] << 8));
	case PLDM_SENSOR_DATA_SIZE_UINT32:
		return (uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16) |
				  ((uint32_t)b[3] << 24));
	case PLDM_SENSOR_DATA_SIZE_SINT32:
		return (int32_t)(b[0] | (b[1] << 8) | (b[2] << 16) |
				 ((uint32_t)b[3] << 24));
	default:
		return b[0];
	}
}

struct inventory {
	struct obj numeric[MAX_OBJ]; // numeric sensors
	int n_numeric;
	struct obj state[MAX_OBJ]; // state sensors
	int n_state;
	struct obj eff[MAX_OBJ]; // state effecters
	int n_eff;
};

static void add_obj(struct obj *arr, int *n, struct obj o)
{
	if (*n < MAX_OBJ)
		arr[(*n)++] = o;
}

// ===================================================================
// PDR repository walk.
// ===================================================================
static void parse_pdr(struct inventory *inv, const uint8_t *rec, size_t len)
{
	if (len < sizeof(struct pldm_pdr_hdr))
		return;
	const struct pldm_pdr_hdr *h = (const struct pldm_pdr_hdr *)rec;

	switch (h->type) {
	case PLDM_NUMERIC_SENSOR_PDR: {
		// The on-wire numeric sensor PDR is densely packed and differs
		// from the unpacked struct, so use libpldm's dedicated decoder.
		struct pldm_numeric_sensor_value_pdr p = { 0 };
		if (decode_numeric_sensor_pdr_data(rec, len, &p) != 0)
			break;
		add_obj(inv->numeric, &inv->n_numeric,
			(struct obj){ .id = p.sensor_id,
				      .comp_count = 1,
				      .base_unit = p.base_unit,
				      .sensor_data_size = p.sensor_data_size,
				      .unit_modifier = p.unit_modifier,
				      .resolution = p.resolution,
				      .offset = p.offset });
		break;
	}
	case PLDM_STATE_SENSOR_PDR: {
		if (len < sizeof(struct pldm_state_sensor_pdr))
			break;
		const struct pldm_state_sensor_pdr *p =
			(const struct pldm_state_sensor_pdr *)rec;
		add_obj(inv->state, &inv->n_state,
			(struct obj){ .id = p->sensor_id,
				      .comp_count = p->composite_sensor_count });
		break;
	}
	case PLDM_STATE_EFFECTER_PDR: {
		if (len < sizeof(struct pldm_state_effecter_pdr))
			break;
		const struct pldm_state_effecter_pdr *p =
			(const struct pldm_state_effecter_pdr *)rec;
		add_obj(inv->eff, &inv->n_eff,
			(struct obj){ .id = p->effecter_id,
				      .comp_count =
					      p->composite_effecter_count });
		break;
	}
	default:
		break; // terminus locator, entity assoc, etc. — not exercised
	}
}

static const char *pdr_type_name(uint8_t t)
{
	switch (t) {
	case PLDM_TERMINUS_LOCATOR_PDR:		   return "TerminusLocator";
	case PLDM_NUMERIC_SENSOR_PDR:		   return "NumericSensor";
	case PLDM_NUMERIC_SENSOR_INITIALIZATION_PDR: return "NumericSensorInit";
	case PLDM_STATE_SENSOR_PDR:		   return "StateSensor";
	case PLDM_STATE_SENSOR_INITIALIZATION_PDR: return "StateSensorInit";
	case PLDM_SENSOR_AUXILIARY_NAMES_PDR:	   return "SensorAuxNames";
	case PLDM_OEM_UNIT_PDR:			   return "OEMUnit";
	case PLDM_OEM_STATE_SET_PDR:		   return "OEMStateSet";
	case PLDM_NUMERIC_EFFECTER_PDR:		   return "NumericEffecter";
	case PLDM_NUMERIC_EFFECTER_INITIALIZATION_PDR: return "NumericEffecterInit";
	case PLDM_STATE_EFFECTER_PDR:		   return "StateEffecter";
	case PLDM_STATE_EFFECTER_INITIALIZATION_PDR: return "StateEffecterInit";
	case PLDM_EFFECTER_AUXILIARY_NAMES_PDR:	   return "EffecterAuxNames";
	case PLDM_EFFECTER_OEM_SEMANTIC_PDR:	   return "EffecterOEMSemantic";
	case PLDM_PDR_ENTITY_ASSOCIATION:	   return "EntityAssociation";
	case PLDM_ENTITY_AUXILIARY_NAMES_PDR:	   return "EntityAuxNames";
	case PLDM_OEM_ENTITY_ID_PDR:		   return "OEMEntityID";
	case PLDM_INTERRUPT_ASSOCIATION_PDR:	   return "InterruptAssociation";
	case PLDM_EVENT_LOG_PDR:		   return "EventLog";
	case PLDM_PDR_FRU_RECORD_SET:		   return "FRURecordSet";
	case PLDM_COMPACT_NUMERIC_SENSOR_PDR:	   return "CompactNumericSensor";
	default:				   return "Unknown";
	}
}

// Format the state_set_id of each composite entry in a state sensor/effecter
// PDR's possible_states blob: [setId(2)][size(1)][states(size)] repeated.
static void fmt_state_sets(char *out, size_t osz, const uint8_t *ps,
			   const uint8_t *end, uint8_t count)
{
	size_t p = 0;
	out[0] = 0;
	for (uint8_t i = 0; i < count && ps + 3 <= end; i++) {
		uint16_t sid = ps[0] | (ps[1] << 8);
		uint8_t sz = ps[2];
		p += snprintf(out + p, p < osz ? osz - p : 0, "%s%u", i ? "," : "",
			      sid);
		ps += 3 + sz;
	}
}

// Decode and structurally validate one PDR record, printing its key fields.
// Returns true if the record is sound: a known PDR type whose header length
// field matches the bytes actually returned.
static bool validate_pdr(struct results *r, const uint8_t *rec, size_t cnt)
{
	char det[176] = "";

	if (cnt < sizeof(struct pldm_pdr_hdr)) {
		check(r, "  PDR (truncated)", false, "only %zu bytes", cnt);
		return false;
	}
	const struct pldm_pdr_hdr *h = (const struct pldm_pdr_hdr *)rec;
	uint32_t handle = h->record_handle;
	uint8_t type = h->type;
	const char *name = pdr_type_name(type);
	const uint8_t *end = rec + cnt;
	// The header length field counts the bytes following the common header.
	bool len_ok = (cnt == sizeof(struct pldm_pdr_hdr) + h->length);
	bool known = strcmp(name, "Unknown") != 0;

	switch (type) {
	case PLDM_TERMINUS_LOCATOR_PDR: {
		const struct pldm_terminus_locator_pdr *p = (const void *)rec;
		uint8_t eid = (p->terminus_locator_type == 0 &&
			       p->terminus_locator_value_size >= 1) ?
				      p->terminus_locator_value[0] :
				      0;
		snprintf(det, sizeof(det),
			 "validity=%u tid=%u container=%u loc_type=%u eid=%u",
			 p->validity, p->tid, p->container_id,
			 p->terminus_locator_type, eid);
		break;
	}
	case PLDM_NUMERIC_SENSOR_PDR: {
		struct pldm_numeric_sensor_value_pdr p = { 0 };
		if (decode_numeric_sensor_pdr_data(rec, cnt, &p) == 0)
			snprintf(det, sizeof(det),
				 "sensor_id=%u entity_type=%u unit=%s mod=%d data_size=%u",
				 p.sensor_id, p.entity_type,
				 unit_name(p.base_unit), p.unit_modifier,
				 p.sensor_data_size);
		break;
	}
	case PLDM_STATE_SENSOR_PDR: {
		const struct pldm_state_sensor_pdr *p = (const void *)rec;
		char ss[80];
		fmt_state_sets(ss, sizeof(ss), p->possible_states, end,
			       p->composite_sensor_count);
		snprintf(det, sizeof(det),
			 "sensor_id=%u entity_type=%u composite=%u state_sets=[%s]",
			 p->sensor_id, p->entity_type,
			 p->composite_sensor_count, ss);
		break;
	}
	case PLDM_STATE_EFFECTER_PDR: {
		const struct pldm_state_effecter_pdr *p = (const void *)rec;
		char ss[80];
		fmt_state_sets(ss, sizeof(ss), p->possible_states, end,
			       p->composite_effecter_count);
		snprintf(det, sizeof(det),
			 "effecter_id=%u entity_type=%u composite=%u state_sets=[%s]",
			 p->effecter_id, p->entity_type,
			 p->composite_effecter_count, ss);
		break;
	}
	case PLDM_NUMERIC_EFFECTER_PDR: {
		struct pldm_numeric_effecter_value_pdr p = { 0 };
		if (decode_numeric_effecter_pdr_data(rec, cnt, &p) == 0)
			snprintf(det, sizeof(det),
				 "effecter_id=%u entity_type=%u unit=%s mod=%d",
				 p.effecter_id, p.entity_type,
				 unit_name(p.base_unit), p.unit_modifier);
		break;
	}
	case PLDM_SENSOR_AUXILIARY_NAMES_PDR: {
		const struct pldm_sensor_auxiliary_names_pdr *p =
			(const void *)rec;
		snprintf(det, sizeof(det), "sensor_id=%u name_count=%u",
			 p->sensor_id, p->sensor_count);
		break;
	}
	case PLDM_COMPACT_NUMERIC_SENSOR_PDR: {
		const struct pldm_compact_numeric_sensor_pdr *p =
			(const void *)rec;
		snprintf(det, sizeof(det),
			 "sensor_id=%u entity_type=%u unit=%s mod=%d",
			 p->sensor_id, p->entity_type, unit_name(p->base_unit),
			 p->unit_modifier);
		break;
	}
	case PLDM_PDR_ENTITY_ASSOCIATION: {
		// Body after the common header.
		const struct pldm_pdr_entity_association *p =
			(const void *)(rec + sizeof(struct pldm_pdr_hdr));
		snprintf(det, sizeof(det),
			 "container_id=%u assoc=%s container_entity=%u.%u children=%u",
			 p->container_id,
			 p->association_type == PLDM_ENTITY_ASSOCIAION_PHYSICAL ?
				 "physical" :
				 "logical",
			 p->container.entity_type,
			 p->container.entity_instance_num, p->num_children);
		break;
	}
	case PLDM_PDR_FRU_RECORD_SET: {
		const struct pldm_pdr_fru_record_set *p =
			(const void *)(rec + sizeof(struct pldm_pdr_hdr));
		snprintf(det, sizeof(det),
			 "fru_rsi=%u entity_type=%u instance=%u container=%u",
			 p->fru_rsi, p->entity_type, p->entity_instance_num,
			 p->container_id);
		break;
	}
	default:
		snprintf(det, sizeof(det), "(not decoded)");
		break;
	}

	printf("   PDR %u: %-21s v%u len=%u  %s\n", handle, name, h->version,
	       h->length, det);
	char nm[48];
	snprintf(nm, sizeof(nm), "  PDR %u %s", handle, name);
	check(r, nm, known && len_ok, "type=%u len_ok=%d", type, len_ok);
	return known && len_ok;
}

static void pdr_walk(struct app_ctx *ctx, uint8_t eid, int to,
		     struct inventory *inv, struct results *r)
{
	uint8_t tx[64];
	struct pldm_msg *req = (struct pldm_msg *)(tx + 1);
	size_t pl;
	const struct pldm_msg *rsp;

	printf("\n-- platform: PDR repository --\n");

	// GetPDRRepositoryInfo (0x50)
	tx[0] = MCTP_MSG_TYPE_PLDM;
	uint8_t cc = 0xff;
	struct pldm_pdr_repository_info_resp info = { 0 };
	bool ok = encode_get_pdr_repository_info_req(pl_iid(ctx), req, 0) == 0 &&
		  request_wait(ctx, eid, tx, MCTP_PLDM_OVERHEAD,
			       MCTP_MSG_TYPE_PLDM,
			       PLDM_GET_PDR_REPOSITORY_INFO, to);
	if (ok) {
		rsp = pldm_resp(ctx, &pl);
		ok = decode_get_pdr_repository_info_resp_safe(rsp, pl, &info) ==
		     0;
		cc = info.completion_code;
	}
	check(r, "PLDM GetPDRRepositoryInfo", ok && cc == PLDM_SUCCESS,
	      "cc=0x%02x records=%u repo_size=%u largest=%u", cc,
	      info.record_count, info.repository_size, info.largest_record_size);
	uint32_t expect_records = (ok && cc == PLDM_SUCCESS) ? info.record_count :
								0;

	// GetPDR (0x51) — walk handles until next_record_handle wraps to 0.
	uint32_t handle = 0;
	uint32_t seen = 0;
	int walk_ok = 1;
	do {
		uint8_t recbuf[512];
		uint8_t gcc = 0xff, flag = 0, crc = 0;
		uint32_t next = 0, next_xfer = 0;
		uint16_t cnt = 0;
		ok = encode_get_pdr_req(pl_iid(ctx), handle, 0,
					PLDM_GET_FIRSTPART, sizeof(recbuf), 0,
					req, PLDM_GET_PDR_REQ_BYTES) == 0 &&
		     request_wait(ctx, eid, tx,
				  MCTP_PLDM_OVERHEAD + PLDM_GET_PDR_REQ_BYTES,
				  MCTP_MSG_TYPE_PLDM, PLDM_GET_PDR, to);
		if (ok) {
			rsp = pldm_resp(ctx, &pl);
			ok = decode_get_pdr_resp(rsp, pl, &gcc, &next, &next_xfer,
						 &flag, &cnt, recbuf,
						 sizeof(recbuf), &crc) == 0;
		}
		if (!(ok && gcc == PLDM_SUCCESS)) {
			walk_ok = 0;
			check(r, "  GetPDR", false, "handle=%u cc=0x%02x", handle,
			      gcc);
			break;
		}
		if (flag != PLDM_START_AND_END)
			printf("   note: handle %u is multipart (flag=0x%02x), "
			       "only first part parsed\n",
			       handle, flag);
		validate_pdr(r, recbuf, cnt); // decode + structurally validate
		parse_pdr(inv, recbuf, cnt);  // collect sensors/effecters to read
		seen++;
		handle = next;
	} while (handle != 0 && seen < 256);

	check(r, "  GetPDR walk", walk_ok && (expect_records == 0 ||
					      seen == expect_records),
	      "read %u of %u PDRs", seen, expect_records);
	printf("   inventory: %d numeric sensor(s), %d state sensor(s), "
	       "%d state effecter(s)\n",
	       inv->n_numeric, inv->n_state, inv->n_eff);
}

// ===================================================================
// Sensor reads.
// ===================================================================
static void read_numeric_sensors(struct app_ctx *ctx, uint8_t eid, int to,
				  struct inventory *inv, struct results *r)
{
	uint8_t tx[32];
	struct pldm_msg *req = (struct pldm_msg *)(tx + 1);
	size_t pl;
	const struct pldm_msg *rsp;

	if (!inv->n_numeric)
		return;
	printf("\n-- platform: numeric sensors (GetSensorReading 0x11) --\n");
	for (int i = 0; i < inv->n_numeric; i++) {
		struct obj *o = &inv->numeric[i];
		uint16_t sid = o->id;
		tx[0] = MCTP_MSG_TYPE_PLDM;
		uint8_t cc = 0xff, sds = 0, opstate = 0, evt_en = 0, present = 0,
			prev = 0, evstate = 0;
		uint8_t reading[4] = { 0 };
		bool ok = encode_get_sensor_reading_req(pl_iid(ctx), sid, false,
							req) == 0 &&
			  request_wait(ctx, eid, tx,
				       MCTP_PLDM_OVERHEAD +
					       PLDM_GET_SENSOR_READING_REQ_BYTES,
				       MCTP_MSG_TYPE_PLDM,
				       PLDM_GET_SENSOR_READING, to);
		if (ok) {
			rsp = pldm_resp(ctx, &pl);
			ok = decode_get_sensor_reading_resp(
				     rsp, pl, &cc, &sds, &opstate, &evt_en,
				     &present, &prev, &evstate, reading) == 0;
		}
		// Apply the PDR conversion to turn the raw reading into an
		// engineering value: value = (raw * resolution + offset)
		//                             * 10^unit_modifier.
		double raw = raw_to_signed(reading,
					   sds ? sds : o->sensor_data_size);
		double res = o->resolution ? o->resolution : 1.0;
		double val = (raw * res + o->offset) * pow10i(o->unit_modifier);
		char nm[40];
		snprintf(nm, sizeof(nm), "  sensor %u reading", sid);
		check(r, nm, ok && cc == PLDM_SUCCESS,
		      "cc=0x%02x opstate=%u raw=%g value=%g %s", cc, opstate, raw,
		      val, unit_name(o->base_unit));
	}
}

static void read_state_sensors(struct app_ctx *ctx, uint8_t eid, int to,
			       struct inventory *inv, struct results *r)
{
	uint8_t tx[32];
	struct pldm_msg *req = (struct pldm_msg *)(tx + 1);
	size_t pl;
	const struct pldm_msg *rsp;

	if (!inv->n_state)
		return;
	printf("\n-- platform: state sensors (GetStateSensorReadings 0x21) --\n");
	for (int i = 0; i < inv->n_state; i++) {
		uint16_t sid = inv->state[i].id;
		tx[0] = MCTP_MSG_TYPE_PLDM;
		bitfield8_t rearm = { 0 };
		uint8_t cc = 0xff, count = 8;
		get_sensor_state_field field[8] = { 0 };
		bool ok = encode_get_state_sensor_readings_req(pl_iid(ctx), sid,
							       rearm, 0,
							       req) == 0 &&
			  request_wait(
				  ctx, eid, tx,
				  MCTP_PLDM_OVERHEAD +
					  PLDM_GET_STATE_SENSOR_READINGS_REQ_BYTES,
				  MCTP_MSG_TYPE_PLDM,
				  PLDM_GET_STATE_SENSOR_READINGS, to);
		if (ok) {
			rsp = pldm_resp(ctx, &pl);
			ok = decode_get_state_sensor_readings_resp(
				     rsp, pl, &cc, &count, field) == 0;
		}
		char states[96] = "";
		size_t p = 0;
		for (int s = 0; ok && s < count && s < 8; s++)
			p += snprintf(states + p, p < sizeof(states) ?
						       sizeof(states) - p :
						       0,
				      "%s%u", s ? "," : "",
				      field[s].present_state);
		char nm[40];
		snprintf(nm, sizeof(nm), "  sensor %u state", sid);
		check(r, nm, ok && cc == PLDM_SUCCESS,
		      "cc=0x%02x count=%u present=[%s]", cc, count, states);
	}
}

static void read_thresholds(struct app_ctx *ctx, uint8_t eid, int to,
			    struct inventory *inv, struct results *r)
{
	uint8_t tx[32];
	size_t pl;
	const struct pldm_msg *rsp;

	if (!inv->n_numeric)
		return;
	printf("\n-- platform: thresholds (GetSensorThresholds 0x12) --\n");
	for (int i = 0; i < inv->n_numeric; i++) {
		uint16_t sid = inv->numeric[i].id;
		uint8_t body[2] = { (uint8_t)(sid & 0xff),
				    (uint8_t)(sid >> 8) };
		size_t len = pldm_build(tx, ctx, PLDM_GET_SENSOR_THRESHOLDS,
					body, sizeof(body));
		bool ok = request_wait(ctx, eid, tx, len, MCTP_MSG_TYPE_PLDM,
				       PLDM_GET_SENSOR_THRESHOLDS, to);
		uint8_t cc = 0xff, sds = 0;
		size_t vlen = 0;
		if (ok) {
			rsp = pldm_resp(ctx, &pl);
			if (pl >= 2) {
				cc = rsp->payload[0];
				sds = rsp->payload[1];
				vlen = sds_len(sds);
			}
		}
		char nm[44];
		snprintf(nm, sizeof(nm), "  sensor %u thresholds", sid);
		// A sensor may legitimately not support thresholds; accept
		// SUCCESS (with 6 threshold values) or a clean error code.
		bool good = ok && (cc == PLDM_SUCCESS ?
					   pl >= 2 + 6 * vlen :
					   cc != 0xff);
		check(r, nm, good, "cc=0x%02x data_size=%u", cc, sds);
	}
}

static void read_effecters(struct app_ctx *ctx, uint8_t eid, int to,
			   struct inventory *inv, struct results *r)
{
	uint8_t tx[32];
	struct pldm_msg *req = (struct pldm_msg *)(tx + 1);
	size_t pl;
	const struct pldm_msg *rsp;

	if (!inv->n_eff)
		return;
	printf("\n-- platform: state effecters (GetStateEffecterStates 0x3a) --\n");
	for (int i = 0; i < inv->n_eff; i++) {
		uint16_t eff = inv->eff[i].id;
		tx[0] = MCTP_MSG_TYPE_PLDM;
		struct pldm_get_state_effecter_states_resp resp = { 0 };
		bool ok = encode_get_state_effecter_states_req(
				  pl_iid(ctx), eff, req,
				  PLDM_GET_STATE_EFFECTER_STATES_REQ_BYTES) ==
				  0 &&
			  request_wait(
				  ctx, eid, tx,
				  MCTP_PLDM_OVERHEAD +
					  PLDM_GET_STATE_EFFECTER_STATES_REQ_BYTES,
				  MCTP_MSG_TYPE_PLDM,
				  PLDM_GET_STATE_EFFECTER_STATES, to);
		if (ok) {
			rsp = pldm_resp(ctx, &pl);
			ok = decode_get_state_effecter_states_resp(rsp, pl,
								   &resp) == 0;
		}
		char states[96] = "";
		size_t p = 0;
		for (int s = 0;
		     ok && s < resp.comp_effecter_count &&
		     s < PLDM_GET_EFFECTER_STATE_FIELD_COUNT_MAX;
		     s++)
			p += snprintf(states + p, p < sizeof(states) ?
						       sizeof(states) - p :
						       0,
				      "%s%u", s ? "," : "",
				      resp.field[s].present_state);
		char nm[44];
		snprintf(nm, sizeof(nm), "  effecter %u states", eff);
		check(r, nm, ok && resp.completion_code == PLDM_SUCCESS,
		      "cc=0x%02x count=%u present=[%s]", resp.completion_code,
		      resp.comp_effecter_count, states);
	}
}

// ===================================================================
// Write round-trips (allow_writes). Each Set* writes back the value just read
// from the corresponding Get*, so the DUT's live state is preserved.
// ===================================================================
static void write_effecters(struct app_ctx *ctx, uint8_t eid, int to,
			    struct inventory *inv, struct results *r)
{
	uint8_t tx[64];
	struct pldm_msg *req = (struct pldm_msg *)(tx + 1);
	size_t pl;
	const struct pldm_msg *rsp;

	if (!inv->n_eff)
		return;
	printf("\n-- platform: effecter write-back "
	       "(SetStateEffecterStates 0x39) --\n");
	for (int i = 0; i < inv->n_eff; i++) {
		uint16_t eff = inv->eff[i].id;

		// Read current states.
		tx[0] = MCTP_MSG_TYPE_PLDM;
		struct pldm_get_state_effecter_states_resp cur = { 0 };
		bool ok = encode_get_state_effecter_states_req(
				  pl_iid(ctx), eff, req,
				  PLDM_GET_STATE_EFFECTER_STATES_REQ_BYTES) ==
				  0 &&
			  request_wait(
				  ctx, eid, tx,
				  MCTP_PLDM_OVERHEAD +
					  PLDM_GET_STATE_EFFECTER_STATES_REQ_BYTES,
				  MCTP_MSG_TYPE_PLDM,
				  PLDM_GET_STATE_EFFECTER_STATES, to);
		if (ok) {
			rsp = pldm_resp(ctx, &pl);
			ok = decode_get_state_effecter_states_resp(rsp, pl,
								   &cur) == 0 &&
			     cur.completion_code == PLDM_SUCCESS;
		}
		char nm[48];
		snprintf(nm, sizeof(nm), "  effecter %u write-back", eff);
		if (!ok) {
			check(r, nm, false, "read-back failed");
			continue;
		}

		// Write the same present_state back for each composite field.
		uint8_t count = cur.comp_effecter_count;
		if (count > PLDM_GET_EFFECTER_STATE_FIELD_COUNT_MAX)
			count = PLDM_GET_EFFECTER_STATE_FIELD_COUNT_MAX;
		set_effecter_state_field set[PLDM_GET_EFFECTER_STATE_FIELD_COUNT_MAX];
		for (int s = 0; s < count; s++) {
			set[s].set_request = PLDM_REQUEST_SET;
			set[s].effecter_state = cur.field[s].present_state;
		}
		tx[0] = MCTP_MSG_TYPE_PLDM;
		uint8_t cc = 0xff;
		ok = encode_set_state_effecter_states_req(pl_iid(ctx), eff,
							  count, set, req) == 0 &&
		     request_wait(ctx, eid, tx,
				  MCTP_PLDM_OVERHEAD +
					  PLDM_SET_STATE_EFFECTER_STATES_REQ_BYTES,
				  MCTP_MSG_TYPE_PLDM,
				  PLDM_SET_STATE_EFFECTER_STATES, to);
		if (ok) {
			rsp = pldm_resp(ctx, &pl);
			cc = pl >= 1 ? rsp->payload[0] : 0xff;
		}
		check(r, nm, ok && cc == PLDM_SUCCESS,
		      "wrote back %u state(s), cc=0x%02x", count, cc);
	}
}

static void write_thresholds(struct app_ctx *ctx, uint8_t eid, int to,
			     struct inventory *inv, struct results *r)
{
	uint8_t tx[64];
	size_t pl;
	const struct pldm_msg *rsp;

	if (!inv->n_numeric)
		return;
	printf("\n-- platform: threshold write-back "
	       "(SetSensorThresholds 0x13) --\n");
	for (int i = 0; i < inv->n_numeric; i++) {
		uint16_t sid = inv->numeric[i].id;

		// Read current thresholds.
		uint8_t body[2] = { (uint8_t)(sid & 0xff), (uint8_t)(sid >> 8) };
		size_t len = pldm_build(tx, ctx, PLDM_GET_SENSOR_THRESHOLDS,
					body, sizeof(body));
		bool ok = request_wait(ctx, eid, tx, len, MCTP_MSG_TYPE_PLDM,
				       PLDM_GET_SENSOR_THRESHOLDS, to);
		uint8_t cc = 0xff, sds = 0;
		uint8_t thr[24]; // up to 6 values * 4 bytes
		size_t vlen = 0, tbytes = 0;
		if (ok) {
			rsp = pldm_resp(ctx, &pl);
			if (pl >= 2) {
				cc = rsp->payload[0];
				sds = rsp->payload[1];
				vlen = sds_len(sds);
				tbytes = 6 * vlen;
				if (cc == PLDM_SUCCESS && pl >= 2 + tbytes &&
				    tbytes <= sizeof(thr))
					memcpy(thr, &rsp->payload[2], tbytes);
				else
					cc = 0xfe; // unusable for round-trip
			}
		}
		char nm[48];
		snprintf(nm, sizeof(nm), "  sensor %u threshold write-back", sid);
		if (!(ok && cc == PLDM_SUCCESS)) {
			// Sensor without writable thresholds: not a failure,
			// just nothing to round-trip.
			check(r, nm, ok, "skipped (get cc=0x%02x)", cc);
			continue;
		}

		// Write the same thresholds back: sensorID, sensorDataSize,
		// then the 6 threshold values verbatim.
		uint8_t wbody[3 + sizeof(thr)];
		wbody[0] = body[0];
		wbody[1] = body[1];
		wbody[2] = sds;
		memcpy(wbody + 3, thr, tbytes);
		len = pldm_build(tx, ctx, PLDM_SET_SENSOR_THRESHOLDS, wbody,
				 3 + tbytes);
		ok = request_wait(ctx, eid, tx, len, MCTP_MSG_TYPE_PLDM,
				  PLDM_SET_SENSOR_THRESHOLDS, to);
		uint8_t scc = 0xff;
		if (ok) {
			rsp = pldm_resp(ctx, &pl);
			scc = pl >= 1 ? rsp->payload[0] : 0xff;
		}
		// A DUT may legitimately keep thresholds read-only and reject a
		// Set with a defined error code; that is spec-conformant. So the
		// check is: did the DUT return a well-formed response to a valid
		// request? SUCCESS = accepted, other cc = cleanly rejected.
		bool wellformed = ok && scc != 0xff;
		check(r, nm, wellformed, "%s cc=0x%02x",
		      scc == PLDM_SUCCESS ? "accepted" :
					    "rejected (read-only?)",
		      scc);
	}
}

static void write_sensor_enable(struct app_ctx *ctx, uint8_t eid, int to,
				struct inventory *inv, struct results *r)
{
	uint8_t tx[32];
	struct pldm_msg *req = (struct pldm_msg *)(tx + 1);
	size_t pl;
	const struct pldm_msg *rsp;

	if (!inv->n_numeric)
		return;
	printf("\n-- platform: sensor enable write-back "
	       "(SetNumericSensorEnable 0x10) --\n");
	for (int i = 0; i < inv->n_numeric; i++) {
		uint16_t sid = inv->numeric[i].id;

		// Read current operational + event-message-enable state.
		tx[0] = MCTP_MSG_TYPE_PLDM;
		uint8_t cc = 0xff, sds = 0, opstate = 0, evt_en = 0, ps = 0,
			pv = 0, es = 0, reading[4] = { 0 };
		bool ok = encode_get_sensor_reading_req(pl_iid(ctx), sid, false,
							req) == 0 &&
			  request_wait(ctx, eid, tx,
				       MCTP_PLDM_OVERHEAD +
					       PLDM_GET_SENSOR_READING_REQ_BYTES,
				       MCTP_MSG_TYPE_PLDM,
				       PLDM_GET_SENSOR_READING, to);
		if (ok) {
			rsp = pldm_resp(ctx, &pl);
			ok = decode_get_sensor_reading_resp(
				     rsp, pl, &cc, &sds, &opstate, &evt_en, &ps,
				     &pv, &es, reading) == 0 &&
			     cc == PLDM_SUCCESS;
		}
		char nm[48];
		snprintf(nm, sizeof(nm), "  sensor %u enable write-back", sid);
		if (!ok) {
			check(r, nm, false, "read-back failed (cc=0x%02x)", cc);
			continue;
		}

		// SetNumericSensorEnable: sensorID(2), opState(1), evtEnable(1).
		uint8_t wbody[4] = { (uint8_t)(sid & 0xff), (uint8_t)(sid >> 8),
				     opstate, evt_en };
		size_t len = pldm_build(tx, ctx, PLDM_SET_NUMERIC_SENSOR_ENABLE,
					wbody, sizeof(wbody));
		ok = request_wait(ctx, eid, tx, len, MCTP_MSG_TYPE_PLDM,
				  PLDM_SET_NUMERIC_SENSOR_ENABLE, to);
		uint8_t scc = 0xff;
		if (ok) {
			rsp = pldm_resp(ctx, &pl);
			scc = pl >= 1 ? rsp->payload[0] : 0xff;
		}
		check(r, nm, ok && scc == PLDM_SUCCESS,
		      "opstate=%u evt_en=%u cc=0x%02x", opstate, evt_en, scc);
	}
}

// ===================================================================
// Active effecter drive: set one state effecter to a specific state (NOT a
// round-trip — this changes the DUT). Reads the state before and after so you
// can see the change, e.g. to drive an LED. Returns the post-set state.
// ===================================================================
static int get_effecter_state(struct app_ctx *ctx, uint8_t eid,
			      uint16_t effecter_id, int to, uint8_t *count_out)
{
	uint8_t tx[32];
	struct pldm_msg *req = (struct pldm_msg *)(tx + 1);
	size_t pl;
	tx[0] = MCTP_MSG_TYPE_PLDM;
	struct pldm_get_state_effecter_states_resp resp = { 0 };
	bool ok = encode_get_state_effecter_states_req(
			  pl_iid(ctx), effecter_id, req,
			  PLDM_GET_STATE_EFFECTER_STATES_REQ_BYTES) == 0 &&
		  request_wait(ctx, eid, tx,
			       MCTP_PLDM_OVERHEAD +
				       PLDM_GET_STATE_EFFECTER_STATES_REQ_BYTES,
			       MCTP_MSG_TYPE_PLDM, PLDM_GET_STATE_EFFECTER_STATES,
			       to);
	if (ok) {
		const struct pldm_msg *rsp = pldm_resp(ctx, &pl);
		if (decode_get_state_effecter_states_resp(rsp, pl, &resp) == 0 &&
		    resp.completion_code == PLDM_SUCCESS) {
			if (count_out)
				*count_out = resp.comp_effecter_count;
			return resp.field[0].present_state;
		}
	}
	return -1;
}

void run_pldm_effecter_drive(struct app_ctx *ctx, uint8_t dst_eid,
			     uint16_t effecter_id, uint8_t state, int timeout_ms,
			     struct results *r)
{
	uint8_t tx[64];
	struct pldm_msg *req = (struct pldm_msg *)(tx + 1);
	size_t pl;

	printf("\n== PLDM effecter drive: effecter %u -> state %u ==\n",
	       effecter_id, state);

	uint8_t count = 1;
	int before = get_effecter_state(ctx, dst_eid, effecter_id, timeout_ms,
					&count);
	printf("   before: present_state=%d (composite_count=%u)\n", before,
	       count);

	// Set composite field 0 to the requested state; leave any others alone.
	if (count == 0 || count > PLDM_GET_EFFECTER_STATE_FIELD_COUNT_MAX)
		count = 1;
	set_effecter_state_field set[PLDM_GET_EFFECTER_STATE_FIELD_COUNT_MAX];
	for (int i = 0; i < count; i++) {
		set[i].set_request = (i == 0) ? PLDM_REQUEST_SET : PLDM_NO_CHANGE;
		set[i].effecter_state = (i == 0) ? state : 0;
	}
	tx[0] = MCTP_MSG_TYPE_PLDM;
	uint8_t cc = 0xff;
	bool ok = encode_set_state_effecter_states_req(pl_iid(ctx), effecter_id,
						       count, set, req) == 0 &&
		  request_wait(ctx, dst_eid, tx,
			       MCTP_PLDM_OVERHEAD +
				       PLDM_SET_STATE_EFFECTER_STATES_REQ_BYTES,
			       MCTP_MSG_TYPE_PLDM, PLDM_SET_STATE_EFFECTER_STATES,
			       timeout_ms);
	if (ok) {
		const struct pldm_msg *rsp = pldm_resp(ctx, &pl);
		cc = pl >= 1 ? rsp->payload[0] : 0xff;
	}
	check(r, "SetStateEffecterStates", ok && cc == PLDM_SUCCESS,
	      "effecter %u <- state %u, cc=0x%02x", effecter_id, state, cc);

	int after = get_effecter_state(ctx, dst_eid, effecter_id, timeout_ms,
				       NULL);
	printf("   after : present_state=%d\n", after);
	check(r, "effecter state changed", after == (int)state,
	      "present_state=%d (wanted %u)", after, state);
}

void run_pldm_platform_bench(struct app_ctx *ctx, uint8_t dst_eid,
			     int timeout_ms, int allow_writes,
			     struct results *r)
{
	printf("\n== PLDM Platform validator (libpldm, DSP0248) ==\n");

	struct inventory inv = { 0 };
	pdr_walk(ctx, dst_eid, timeout_ms, &inv, r);

	read_numeric_sensors(ctx, dst_eid, timeout_ms, &inv, r);
	read_state_sensors(ctx, dst_eid, timeout_ms, &inv, r);
	read_thresholds(ctx, dst_eid, timeout_ms, &inv, r);
	read_effecters(ctx, dst_eid, timeout_ms, &inv, r);

	if (allow_writes) {
		write_effecters(ctx, dst_eid, timeout_ms, &inv, r);
		write_thresholds(ctx, dst_eid, timeout_ms, &inv, r);
		write_sensor_enable(ctx, dst_eid, timeout_ms, &inv, r);
	} else {
		printf("\n(write round-trips skipped; pass -W to enable)\n");
	}
}

// ===================================================================
// Event polling: PollForPlatformEventMessage (0x0b). The DUT advertises it;
// we poll the first part and report any queued event (event_id 0 = none).
// Read-only: we poll with GetFirstPart and do not acknowledge (ack would
// consume the event from the FD's queue).
// ===================================================================
void run_pldm_event_bench(struct app_ctx *ctx, uint8_t dst_eid, int timeout_ms,
			  struct results *r)
{
	uint8_t tx[64];
	struct pldm_msg *req = (struct pldm_msg *)(tx + 1);
	size_t pl;
	const struct pldm_msg *rsp;

	printf("\n== PLDM event poll (PollForPlatformEventMessage 0x0b) ==\n");

	tx[0] = MCTP_MSG_TYPE_PLDM;
	uint8_t cc = 0xff, tid = 0, flag = 0, eclass = 0;
	uint16_t event_id = 0xffff;
	uint32_t next_handle = 0, edata_size = 0, checksum = 0;
	void *edata = NULL;
	bool ok = encode_poll_for_platform_event_message_req(
			  pl_iid(ctx), 0x01, PLDM_GET_FIRSTPART, 0,
			  PLDM_PLATFORM_EVENT_ID_NULL, req,
			  PLDM_POLL_FOR_PLATFORM_EVENT_MESSAGE_REQ_BYTES) == 0 &&
		  request_wait(
			  ctx, dst_eid, tx,
			  MCTP_PLDM_OVERHEAD +
				  PLDM_POLL_FOR_PLATFORM_EVENT_MESSAGE_REQ_BYTES,
			  MCTP_MSG_TYPE_PLDM,
			  PLDM_POLL_FOR_PLATFORM_EVENT_MESSAGE, timeout_ms);
	if (ok) {
		rsp = pldm_resp(ctx, &pl);
		ok = decode_poll_for_platform_event_message_resp(
			     rsp, pl, &cc, &tid, &event_id, &next_handle, &flag,
			     &eclass, &edata_size, &edata, &checksum) == 0;
	}
	check(r, "PLDM PollForPlatformEventMessage", ok && cc == PLDM_SUCCESS,
	      "cc=0x%02x", cc);
	if (ok && cc == PLDM_SUCCESS) {
		if (event_id == PLDM_PLATFORM_EVENT_ID_NULL)
			printf("   no events queued (event_id=0)\n");
		else
			printf("   event_id=0x%04x class=0x%02x tid=%u "
			       "data_size=%u\n",
			       event_id, eclass, tid, edata_size);
	}
}

// ===================================================================
// Multipart transfer: request a PDR with a small requestCount to force the FD
// to split one record across GetFirstPart/GetNextPart chunks, then verify the
// reassembled record matches a single-shot read of the same handle.
// ===================================================================
void run_pldm_multipart_bench(struct app_ctx *ctx, uint8_t dst_eid,
			      int timeout_ms, struct results *r)
{
	uint8_t tx[64];
	struct pldm_msg *req = (struct pldm_msg *)(tx + 1);
	size_t pl;
	const struct pldm_msg *rsp;
	const uint32_t handle = 0; // 0 = first record in the repository
	const uint16_t chunk = 16; // small enough to force several parts

	printf("\n== PLDM multipart GetPDR (forced %u-byte chunks) ==\n", chunk);
	tx[0] = MCTP_MSG_TYPE_PLDM;

	// Reference: single-shot read of the record.
	uint8_t ref[512];
	uint8_t cc = 0xff, flag = 0, crc = 0;
	uint32_t next_rec = 0, next_xfer = 0;
	uint16_t rcnt = 0;
	bool ok = encode_get_pdr_req(pl_iid(ctx), handle, 0, PLDM_GET_FIRSTPART,
				     sizeof(ref), 0, req,
				     PLDM_GET_PDR_REQ_BYTES) == 0 &&
		  request_wait(ctx, dst_eid, tx,
			       MCTP_PLDM_OVERHEAD + PLDM_GET_PDR_REQ_BYTES,
			       MCTP_MSG_TYPE_PLDM, PLDM_GET_PDR, timeout_ms);
	if (ok) {
		rsp = pldm_resp(ctx, &pl);
		ok = decode_get_pdr_resp(rsp, pl, &cc, &next_rec, &next_xfer,
					 &flag, &rcnt, ref, sizeof(ref),
					 &crc) == 0;
	}
	check(r, "  reference single-shot GetPDR", ok && cc == PLDM_SUCCESS,
	      "cc=0x%02x len=%u flag=0x%02x", cc, rcnt, flag);
	if (!(ok && cc == PLDM_SUCCESS))
		return;
	size_t ref_len = rcnt;

	// Multipart: same record handle, walk the data-transfer-handle chain.
	uint8_t asm_buf[512];
	size_t total = 0;
	uint32_t xfer = 0;
	uint8_t op = PLDM_GET_FIRSTPART;
	int parts = 0, walk_ok = 1;
	for (parts = 0; parts < 64; parts++) {
		uint8_t pcc = 0xff, pflag = 0, pcrc = 0;
		uint32_t pnext_rec = 0, pnext_xfer = 0;
		uint16_t pcnt = 0;
		uint8_t part[512]; // big enough even if the DUT ignores chunk size
		ok = encode_get_pdr_req(pl_iid(ctx), handle, xfer, op, chunk, 0,
					req, PLDM_GET_PDR_REQ_BYTES) == 0 &&
		     request_wait(ctx, dst_eid, tx,
				  MCTP_PLDM_OVERHEAD + PLDM_GET_PDR_REQ_BYTES,
				  MCTP_MSG_TYPE_PLDM, PLDM_GET_PDR, timeout_ms);
		if (ok) {
			rsp = pldm_resp(ctx, &pl);
			ok = decode_get_pdr_resp(rsp, pl, &pcc, &pnext_rec,
						 &pnext_xfer, &pflag, &pcnt,
						 part, sizeof(part),
						 &pcrc) == 0;
		}
		if (!(ok && pcc == PLDM_SUCCESS)) {
			walk_ok = 0;
			printf("   part %d failed: cc=0x%02x\n", parts, pcc);
			break;
		}
		if (total + pcnt <= sizeof(asm_buf)) {
			memcpy(asm_buf + total, part, pcnt);
			total += pcnt;
		}
		// First part must be Start (or StartAndEnd if it all fit);
		// continuations are Middle then End.
		if (pflag == PLDM_END || pflag == PLDM_START_AND_END)
			break;
		xfer = pnext_xfer;
		op = PLDM_GET_NEXTPART;
	}

	bool match = walk_ok && total == ref_len &&
		     memcmp(asm_buf, ref, ref_len) == 0;
	check(r, "  multipart reassembly matches", match,
	      "%d part(s), %zu bytes (ref %zu)", parts + 1, total, ref_len);
	if (match && parts == 0)
		printf("   note: DUT returned the whole %zu-byte record in one "
		       "part — it does not honor the %u-byte requestCount, so "
		       "true multipart wasn't exercised (data integrity still "
		       "verified)\n",
		       ref_len, chunk);
	else if (match)
		printf("   multipart transfer reassembled correctly over %d "
		       "parts\n",
		       parts + 1);
}
