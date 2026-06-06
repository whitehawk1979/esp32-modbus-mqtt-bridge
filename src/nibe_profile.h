/**
 * nibe_profile.h — NIBE S1156-18 Modbus Register Profile (PROGMEM)
 *
 * Slave address: 40 (0x28) on Modbus RTU
 * All registers are FC04 (input registers) per NIBE Modbus spec.
 * Scale = divisor: raw_value / scale = display value
 *   e.g. scale=10 means raw 235 → 23.5
 *
 * RegHAClass enum: HAC_SENSOR=0, HAC_TEMPERATURE=1, HAC_HUMIDITY=2,
 *                  HAC_POWER=3, HAC_ENERGY=4, HAC_PRESSURE=5,
 *                  HAC_VOLTAGE=6, HAC_CURRENT=7, HAC_FREQUENCY=8, HAC_COP=9
 * RegType enum:    REG_HOLDING=3, REG_INPUT=4
 */

#ifndef NIBE_PROFILE_H
#define NIBE_PROFILE_H

#include <stdint.h>
#include <Arduino.h>
#include "modbus_mqtt_ha_bridge.h"

// ─── NIBE S1156-18 profile entry (PROGMEM-friendly, no pointers) ───
struct NibeRegEntry {
    uint16_t   addr;       // Modbus register address
    RegType    reg_type;   // FC03 or FC04
    RegHAClass ha_class;   // HA device_class mapping
    uint16_t   scale;      // divisor (10 = /10, 1 = raw)
    char       name[24];   // short human-readable name
    char       unit[8];    // unit string: °C, kW, bar …
};

// ─── Slave address for NIBE S1156-18 ────────────────────────
static constexpr uint8_t NIBE_S1156_SLAVE_ADDR = 0x28; // 40 decimal

// ── TOP-25 monitoring registers ──────────────────────────
//
//  Categories:
//   T = Temperature sensors   (scale 10, °C)
//   C = Compressor status     (scale 1, enum / Hz / h / count)
//   P = Power & Energy        (scale 10 or 1, kW / kWh)
//   R = Pressure              (scale 10, bar)
//   F = Flow                  (scale 10, l/min)
//   I = Electrical current    (scale 10, A)
//   S = Status / Setpoint     (scale 1 or 10, enum / °C)
//
//  All are FC04 (input registers) per NIBE Modbus spec.
static const NibeRegEntry nibe_s1156_profile[] PROGMEM = {

    // ── Temperature sensors (FC04, scale 10, °C) ────
    {    4, REG_INPUT, HAC_TEMPERATURE, 10, "Kulter BT1",         "°C"  }, // T  outdoor temperature
    {    8, REG_INPUT, HAC_TEMPERATURE, 10, "Eloremeno BT2",      "°C"  }, // T  heating supply (forward)
    {   10, REG_INPUT, HAC_TEMPERATURE, 10, "Visszatero BT3",    "°C"  }, // T  heating return
    {   12, REG_INPUT, HAC_TEMPERATURE, 10, "Melegviz BT6",      "°C"  }, // T  domestic hot water
    {   13, REG_INPUT, HAC_TEMPERATURE, 10, "Talaj be BT10",      "°C"  }, // T  ground loop in
    {   14, REG_INPUT, HAC_TEMPERATURE, 10, "Talaj ki BT11",     "°C"  }, // T  ground loop out
    {   54, REG_INPUT, HAC_TEMPERATURE, 10, "Atlagos hom BT1",   "°C"  }, // T  avg outdoor (control alg.)
    {   57, REG_INPUT, HAC_TEMPERATURE, 10, "Kulso elorem BT25",  "°C"  }, // T  external supply line

    // ── Compressor (FC04) ────────────────────────────
    { 1965, REG_INPUT, HAC_SENSOR,       1, "Kompr uzemmod",      ""    }, // C  0=off 1=standby 2=heat 3=DHW 4=cool
    { 5927, REG_INPUT, HAC_FREQUENCY,   10, "Kompr frekvencia",   "Hz"  }, // C  compressor frequency
    { 1961, REG_INPUT, HAC_SENSOR,       1, "Kompr uzemido",     "h"   }, // C  total compressor run-hours
    { 1959, REG_INPUT, HAC_SENSOR,       1, "Kompr inditas",      ""    }, // C  compressor start count

    // ── Power & Energy (FC04) ────────────────────────
    {22130, REG_INPUT, HAC_POWER,       10, "Pillanatnyi kW",     "kW"  }, // P  instantaneous consumption
    {27335, REG_INPUT, HAC_POWER,       10, "Rendszer kW",        "kW"  }, // P  system power consumption
    {28392, REG_INPUT, HAC_ENERGY,      10, "Termeles kWh",       "kWh" }, // P  total energy produced
    {28393, REG_INPUT, HAC_ENERGY,      10, "Fogyasztas kWh",     "kWh" }, // P  total energy consumed
    {27378, REG_INPUT, HAC_ENERGY,      10, "Futes kompr kWh",    "kWh" }, // P  heating (compressor only)
    {27379, REG_INPUT, HAC_ENERGY,      10, "Melegviz kompr kWh", "kWh" }, // P  DHW (compressor only)

    // ── Pressure (FC04, scale 10, bar) ───────────────
    {   94, REG_INPUT, HAC_PRESSURE,    10, "Alacsony ny BP8",   "bar" }, // R  low / suction pressure
    { 6994, REG_INPUT, HAC_PRESSURE,    10, "Magas ny BP9",      "bar" }, // R  high / discharge pressure

    // ── Flow (FC04, scale 10, l/min) ────────────────
    {   58, REG_INPUT, HAC_SENSOR,      10, "Aramlas BF1",       "l/min"}, // F  brine flow rate

    // ── Electrical current (FC04, scale 10, A) ──────
    {   64, REG_INPUT, HAC_CURRENT,     10, "Aram BE3",           "A"   }, // I  phase current L3

    // ── Status / Setpoint (FC04) ─────────────────────
    { 1708, REG_INPUT, HAC_TEMPERATURE, 10, "Szamitott elorem",  "°C"  }, // S  calculated supply setpoint
    {55000, REG_INPUT, HAC_SENSOR,       1, "Prioritas",          ""    }, // S  operational priority enum
    { 1975, REG_INPUT, HAC_SENSOR,      10, "Futesi szivattyu",  ""    }, // S  heating circ pump RPM
};

// Number of entries in the NIBE profile
static constexpr uint8_t NIBE_S1156_PROFILE_SIZE =
    sizeof(nibe_s1156_profile) / sizeof(nibe_s1156_profile[0]);

// ─── Load NIBE profile into runtime RegisterConfig array ────────
// Call: nibe_load_profile(registers, &register_count);
// Returns: number of registers loaded
static inline uint8_t nibe_load_profile(RegisterConfig *regs, uint8_t *count) {
    uint8_t loaded = 0;
    for (uint8_t i = 0; i < NIBE_S1156_PROFILE_SIZE && i < MAX_REGISTERS; ++i) {
        NibeRegEntry tmp;
        memcpy_P(&tmp, &nibe_s1156_profile[i], sizeof(NibeRegEntry));
        memset(&regs[i], 0, sizeof(RegisterConfig));
        regs[i].addr       = tmp.addr;
        regs[i].reg_type   = tmp.reg_type;
        regs[i].ha_class   = tmp.ha_class;
        regs[i].scale      = tmp.scale;
        regs[i].slave_addr = NIBE_S1156_SLAVE_ADDR;
        strncpy(regs[i].name, tmp.name, sizeof(regs[i].name) - 1);
        strncpy(regs[i].unit, tmp.unit, sizeof(regs[i].unit) - 1);
        regs[i].enabled    = true;
        regs[i].last_value = 0;
        regs[i].published  = false;
        regs[i].last_read_ms = 0;
        loaded++;
    }
    *count = loaded;
    return loaded;
}

#endif // NIBE_PROFILE_H