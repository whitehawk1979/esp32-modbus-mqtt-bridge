/**
 * sabiana_profile.h — Sabiana Fan-Coil Controller Modbus Register Profile (PROGMEM)
 *
 * Sabiana fan-coil units (5 slaves) connected via RS485 Modbus RTU.
 * Slave addresses: configurable (default 1-5)
 * Function codes: FC03 Read Holding Registers, FC06 Write Single Register
 *
 * Register map based on Sabiana Modbus protocol (typical):
 *   - Supply air temperature (FC04 input reg)
 *   - Room temperature (FC04 input reg)
 *   - Setpoint temperature (FC03 holding, FC06 writable)
 *   - Fan speed (FC03 holding, FC06 writable: 0=off, 1=low, 2=med, 3=high, 4=auto)
 *   - Operating mode (FC03 holding, FC06 writable: 0=off, 1=cool, 2=heat, 3=auto)
 *   - Valve status (FC03 holding, read-only)
 *
 * Scale conventions: scale=10 → raw/10 (e.g. 225 → 22.5°C)
 */

#ifndef SABIANA_PROFILE_H
#define SABIANA_PROFILE_H

#include <stdint.h>
#include <Arduino.h>
#include "modbus_mqtt_ha_bridge.h"

// ─── Sabiana fan-coil profile entry (PROGMEM-friendly) ────
struct SabianaRegEntry {
    uint16_t   addr;       // Modbus register address
    RegType    reg_type;   // FC03=REG_HOLDING or FC04=REG_INPUT
    RegHAClass ha_class;   // HA device_class
    uint16_t   scale;      // divisor (10 = /10, 1 = raw)
    bool       writable;   // true if FC06 can write this register
    char       name[24];   // short human-readable name
    char       unit[8];    // unit string
};

// ─── Default slave addresses for 5 fan-coil units ────────
// These can be overridden via web config
static constexpr uint8_t SABIANA_DEFAULT_SLAVE_ADDR = 1;    // 1st fan-coil
static constexpr uint8_t SABIANA_SLAVE_COUNT        = 5;    // max units on bus

// ─── Sabiana register map ──────────────────────────────────
// Typical Sabiana fan-coil controller registers
static const SabianaRegEntry sabiana_profile[] PROGMEM = {
    // ── Read-only: input registers (FC04) ──────────
    {  1, REG_INPUT,  HAC_TEMPERATURE, 10, false, "Szoba homerseklet", "°C"  },  // Room temp
    {  2, REG_INPUT,  HAC_TEMPERATURE, 10, false, "Eloremeno homerseklet", "°C" },  // Supply air temp
    {  3, REG_INPUT,  HAC_TEMPERATURE, 10, false, "Visszatero homerseklet", "°C"},  // Return air temp
    { 10, REG_INPUT,  HAC_SENSOR,       1, false, "Allapot",          ""   },  // Status flags

    // ── Read/write: holding registers (FC03/FC06) ───
    { 20, REG_HOLDING, HAC_TEMPERATURE, 10, true,  "Homerseklet setpoint", "°C" },  // Setpoint
    { 21, REG_HOLDING, HAC_SENSOR,       1, true,  "Ventillator sebesseg", ""  },  // Fan speed 0-4
    { 22, REG_HOLDING, HAC_SENSOR,       1, true,  "Uzemmód",          ""   },  // Mode 0-3
    { 23, REG_HOLDING, HAC_SENSOR,       1, false, "Szelep allapot",   ""   },  // Valve status
};

static constexpr uint8_t SABIANA_PROFILE_SIZE =
    sizeof(sabiana_profile) / sizeof(sabiana_profile[0]);

// ─── Load Sabiana profile into runtime RegisterConfig array ────
// slave_addr: the Modbus slave address for this fan-coil unit
// Returns: number of registers loaded
static inline uint8_t sabiana_load_profile(RegisterConfig *regs, uint8_t *count, uint8_t slave_addr) {
    uint8_t loaded = 0;
    for (uint8_t i = 0; i < SABIANA_PROFILE_SIZE && (*count + i) < MAX_REGISTERS; ++i) {
        SabianaRegEntry tmp;
        memcpy_P(&tmp, &sabiana_profile[i], sizeof(SabianaRegEntry));

        uint8_t idx = *count + i;
        memset(&regs[idx], 0, sizeof(RegisterConfig));
        regs[idx].addr       = tmp.addr;
        regs[idx].reg_type   = tmp.reg_type;
        regs[idx].ha_class   = tmp.ha_class;
        regs[idx].scale      = tmp.scale;
        regs[idx].slave_addr = slave_addr;
        regs[idx].writable   = tmp.writable;
        strncpy(regs[idx].name, tmp.name, sizeof(regs[idx].name) - 1);
        strncpy(regs[idx].unit, tmp.unit, sizeof(regs[idx].unit) - 1);
        regs[idx].enabled    = true;
        regs[idx].last_value = 0;
        regs[idx].published  = false;
        regs[idx].last_read_ms = 0;
        loaded++;
    }
    *count += loaded;
    return loaded;
}

#endif // SABIANA_PROFILE_H