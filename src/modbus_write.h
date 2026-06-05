/**
 * modbus_write.h — Modbus FC06/FC16 Register Write Support
 *
 * Waveshare ESP32-S3-ETH V1.0 Modbus-MQTT Bridge
 * Implements Modbus write register functions:
 *   FC06 — Write Single Register
 *   FC16 — Write Multiple Registers
 *   Convenience: modbus_write_slave_id() for address reassignment
 *
 * Uses the existing ModbusMaster 'node' object and follows the
 * same error-handling pattern as modbus_write_coil().
 */

#ifndef MODBUS_WRITE_H
#define MODBUS_WRITE_H

#include "modbus_mqtt_ha_bridge.h"

// ─── FC06: Write Single Register ──────────────────────────────
// Writes a single 16-bit value to a holding register.
// Returns true on success.
bool modbus_write_register(uint8_t slave_addr, uint16_t reg_addr, uint16_t value);

// ─── FC16: Write Multiple Registers ───────────────────────────
// Writes 'count' consecutive 16-bit registers starting at start_addr.
// values[] must contain at least 'count' elements.
// Returns true on success (all registers written).
bool modbus_write_registers(uint8_t slave_addr, uint16_t start_addr,
                            uint16_t count, const uint16_t *values);

// ─── Slave ID Write Convenience ────────────────────────────────
// Writes a new slave address to the specified ID register,
// then waits and verifies the new address responds.
// This is typically used for KC868-HA V2 modules where register
// 0x0064 (REG_MODEL_ID area) or a dedicated ID register holds
// the slave address.
//   slave_addr      — current Modbus address of the device
//   id_register_addr — register that stores the slave address
//   new_id           — new slave address to write (1-247)
// Returns true if write succeeded AND new address verified.
bool modbus_write_slave_id(uint8_t slave_addr, uint16_t id_register_addr, uint8_t new_id);

#endif // MODBUS_WRITE_H