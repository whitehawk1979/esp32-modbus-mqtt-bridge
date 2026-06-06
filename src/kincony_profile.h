/**
 * kincony_profile.h — KinCony KC868-HA V2 Modbus Register Profile (PROGMEM)
 *
 * KinCony KC868-HA V2: 6 Digital Inputs + 6 Relay Outputs
 * Slave addresses: configurable (typically 1-247)
 * FC01 Read Coils, FC02 Read Discrete Inputs, FC05 Write Single Coil
 * Also supports FC03 @ 0x00C8 for combined DI+DO status
 */

#ifndef KINCONY_PROFILE_H
#define KINCONY_PROFILE_H

#include <stdint.h>
#include <Arduino.h>
#include "modbus_mqtt_ha_bridge.h"

// KC868-HA V2 has fixed 6 DI + 6 Relay — no register-based profile needed
// The existing MB_PROFILE_KC868_HA handles DI/DO via FC01/FC02/FC05
// and combined read via FC03 @ 0x00C8

// ─── Identification registers (FC03 holding) ─────────────────
static constexpr uint16_t KC_MODEL_ID_REG    = 0x0064;
static constexpr uint16_t KC_FW_VERSION_REG  = 0x0065;
static constexpr uint16_t KC_SERIAL_LO_REG   = 0x0100;
static constexpr uint16_t KC_SERIAL_HI_REG    = 0x0101;
static constexpr uint16_t KC_DI_DO_STATUS_REG = 0x00C8; // DI bitmask (reg0) + DO bitmask (reg1)

// ── Model name lookup (model_id → name) ──────────────────
struct KCModelLookup {
    uint8_t model_id;
    char name[16]; // non-const to allow default construction
};

static const KCModelLookup kc_model_table[] PROGMEM = {
    { 1,  "KC868-HA1"  },
    { 2,  "KC868-HA2"  },
    { 4,  "KC868-HA4"  },
    { 8,  "KC868-HA8"  },
    { 16, "KC868-HA16" },
    { 32, "KC868-HA32" },
    // V2 universal board
    { 0,  "KC868-HA V2" }, // default/fallback
};

static constexpr uint8_t KC_MODEL_TABLE_SIZE =
    sizeof(kc_model_table) / sizeof(kc_model_table[0]);

// ─── DI friendly names (Hungarian) ────────────────────────
static const char kc_di_names[HA_V2_DI_COUNT][24] PROGMEM = {
    "DI1",
    "DI2",
    "DI3",
    "DI4",
    "DI5",
    "DI6",
};

// ─── Relay friendly names (Hungarian) ─────────────────────
static const char kc_relay_names[HA_V2_RELAY_COUNT][24] PROGMEM = {
    "Relay 1",
    "Relay 2",
    "Relay 3",
    "Relay 4",
    "Relay 5",
    "Relay 6",
};

/**
 * kc_model_name — look up model name by model_id
 * Returns "KC868-HA V2" for unknown IDs.
 */
static inline void kc_model_name(uint8_t model_id, char *buf, size_t buf_len) {
    for (uint8_t i = 0; i < KC_MODEL_TABLE_SIZE; i++) {
        KCModelLookup tmp;
        memcpy_P(&tmp, &kc_model_table[i], sizeof(KCModelLookup));
        if (tmp.model_id == model_id) {
            strncpy(buf, tmp.name, buf_len - 1);
            buf[buf_len - 1] = '\0';
            return;
        }
    }
    strncpy(buf, "KC868-HA V2", buf_len - 1);
    buf[buf_len - 1] = '\0';
}

#endif // KINCONY_PROFILE_H