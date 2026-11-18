// Shared between the parts of the Home Assistant API server.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "automation.h"
#include "zb.h"

// ---- protobuf (ha_pb.c) ----------------------------------------------------------------------
// Writer: appends fields; `ovf` is set (and nothing more written) when the buffer is full.
typedef struct {
    uint8_t *buf;
    size_t cap, len;
    bool ovf;
} pb_w_t;

void pb_varint(pb_w_t *w, uint32_t field, uint64_t v);       // omitted when 0
void pb_bool(pb_w_t *w, uint32_t field, bool v);             // omitted when false
void pb_fixed32(pb_w_t *w, uint32_t field, uint32_t v);      // always written (keys)
void pb_float(pb_w_t *w, uint32_t field, float v);           // always written
void pb_bytes(pb_w_t *w, uint32_t field, const void *p, size_t n);  // always written
void pb_str(pb_w_t *w, uint32_t field, const char *s);       // omitted when ""

// Reader: one field at a time, every length checked against the buffer.
typedef struct {
    const uint8_t *p, *end;
} pb_r_t;

typedef struct {
    uint32_t field;
    uint8_t wire;              // 0 varint, 1 fixed64, 2 length-delimited, 5 fixed32
    uint64_t v;                // varint / fixed32 / fixed64 value
    const uint8_t *data;       // length-delimited
    size_t len;
} pb_field_t;

// true with *f filled; false at the end or on malformed input (then r->p = NULL).
bool pb_next(pb_r_t *r, pb_field_t *f);

// ---- Noise_NNpsk0_25519_ChaChaPoly_SHA256, responder (ha_noise.c) ----------------------------
#define HA_NOISE_MSG2 48       // e (32) + tag of the empty payload (16)

typedef struct {
    uint8_t h[32], ck[32], k[32];
    uint64_t n;
    uint8_t re[32];
    uint8_t send_k[32], recv_k[32];
    uint64_t send_n, recv_n;
} ha_noise_t;

// prologue = "NoiseAPIInit" + be16(len(client hello)) + client hello.
void ha_noise_init(ha_noise_t *s, const uint8_t *prologue, size_t plen, const uint8_t psk[32]);
// msg1 = e (32) + encrypted payload (>= 16). false: wrong key (MAC failure) or malformed.
bool ha_noise_read_msg1(ha_noise_t *s, const uint8_t *msg, size_t len);
// Writes msg2 (HA_NOISE_MSG2 bytes) and derives the transport keys.
bool ha_noise_write_msg2(ha_noise_t *s, uint8_t out[HA_NOISE_MSG2]);
// Transport: out needs len + 16 / gets len - 16. false on failure.
bool ha_noise_encrypt(ha_noise_t *s, const uint8_t *in, size_t len, uint8_t *out);
bool ha_noise_decrypt(ha_noise_t *s, const uint8_t *in, size_t len, uint8_t *out);
void ha_noise_wipe(ha_noise_t *s);

// ---- entity model (ha_model.c) -------------------------------------------------------------------
typedef enum { ET_BINARY = 1, ET_SENSOR, ET_TEXT, ET_SWITCH, ET_BUTTON, ET_EVENT, ET_UPDATE } ent_type_t;

typedef enum {
    V_HUB_PERMIT = 1, V_HUB_PAIRING, V_HUB_CHANNEL, V_HUB_DEVICES, V_HUB_RSSI, V_HUB_UPTIME, V_HUB_MOTION,
    V_ZB_ZONE, V_ZB_TAMPER, V_ZB_BATT_LOW, V_ZB_BATT, V_ZB_BATT_V, V_ZB_TEMP, V_ZB_HUM, V_ZB_OCC, V_ZB_ON,
    V_ZB_ONLINE, V_ZB_RSSI, V_ZB_SEEN, V_ZB_WATTS, V_ZB_VOLTS, V_ZB_AMPS, V_ZB_KWH, V_ZB_FW,
    V_SCRIPT_ENABLED, V_SCRIPT_VALUE,
} ent_val_t;

#define HA_MAX_ENT   416      // hub 6 + motion 1 + 32 Zigbee devices × 11 + 16 script switches + 32 script entities
#define HA_MAX_DEV   (1 + ZB_MAX_DEVICES + AUTO_MAX_SCRIPTS)   // motion sensor + Zigbee + scripts
#define HA_NAME_LEN  40

typedef struct {
    uint8_t type;              // ent_type_t
    uint8_t val;               // ent_val_t
    uint8_t category;          // 0 none, 2 diagnostic
    bool disabled;             // disabled by default in Home Assistant
    int8_t decimals;
    uint8_t state_class;
    uint32_t key;
    uint32_t device_id;        // 0 = the hub itself
    char name[HA_NAME_LEN];
    char device_class[32];
    char unit[16];
    char icon[48];
    char ref[AUTO_NAME_MAX + 1];      // Zigbee IEEE hex / script name
    char ref2[AUTO_HA_NAME + 1];      // script entity name
    uint8_t n_types;                  // events
    char types[AUTO_HA_EVENT_TYPES][AUTO_HA_TYPE_LEN];
} ha_ent_t;

typedef struct {
    uint32_t id;
    char name[ZB_NAME_LEN + 8];
} ha_dev_t;

typedef struct {
    int n_ent, n_dev;
    ha_ent_t ent[HA_MAX_ENT];
    ha_dev_t dev[HA_MAX_DEV];
    uint32_t sig;              // hash of everything Home Assistant learns from ListEntities / DeviceInfo
} ha_model_t;

// Everything the model and the states are computed from, refreshed by the server task.
typedef struct {
    int n_zb;
    zb_dev_info_t zb[ZB_MAX_DEVICES];
    int n_scripts;
    auto_brief_t scripts[AUTO_MAX_SCRIPTS];
    int n_sent;
    auto_ha_entity_t sent[AUTO_HA_MAX];
} ha_inputs_t;

typedef struct {
    bool missing;
    bool b;
    float f;
    char text[32];             // text sensors; update entities: the installed version
    char text2[12];            // update entities: the latest version
    int64_t src_us;            // "Last seen": the frame time the text was made from
} ha_state_t;

void ha_model_build(ha_model_t *m, const ha_inputs_t *in);
// `slow` entities (uptime, signal strengths) are only re-read when `slow_tick` is set,
// so Home Assistant's history isn't filled with ±1 dBm wiggles.
bool ha_model_is_slow(const ha_ent_t *e);
// `prev` = this entity's previous state (kept as is where nothing changed).
void ha_model_state(const ha_ent_t *e, const ha_inputs_t *in, const ha_state_t *prev, ha_state_t *st);
