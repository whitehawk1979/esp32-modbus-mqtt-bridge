/**
 * modbus_write.cpp — Modbus FC06/FC16 Register Write Support
 *
 * Waveshare ESP32-S3-ETH V1.0 Modbus-MQTT Bridge
 *
 * Implements:
 *   modbus_write_register()  — FC06 Write Single Register
 *   modbus_write_registers() — FC16 Write Multiple Registers
 *   modbus_write_slave_id()  — Convenience: write new slave address + verify
 *
 * Uses the existing ModbusMaster 'node' object (modbus_handler.cpp).
 * The 'node' is declared static in modbus_handler.cpp, so we access it
 * through modbus_raw_request() which already wraps all FC codes,
 * OR we declare node extern — but since node is static there, we add
 * dedicated write functions here that use the same pattern as
 * modbus_write_coil(): call set_slave(), then node.writeSingleRegister()
 * or node.writeMultipleRegisters() via the ModbusMaster API.
 *
 * NOTE: We need access to the 'node' and 'set_slave()' from
 * modbus_handler.cpp. They are currently static/file-scope.
 * We make set_slave() accessible via a wrapper and use
 * modbus_raw_request() for the actual transaction, which already
 * handles node.begin() per call.
 *
 * Strategy: Use modbus_raw_request() as the transport layer since
 * it already handles FC06 and FC16 correctly, and we get consistent
 * error handling + statistics for free.
 */

#include <Arduino.h>
#include <ModbusMaster.h>
#include "modbus_mqtt_ha_bridge.h"
#include "modbus_write.h"

// ─── FC06: Write Single Register ──────────────────────────────
bool modbus_write_register(uint8_t slave_addr, uint16_t reg_addr, uint16_t value)
{
    if (modbus_is_paused())
    {
        LOG_E("[MODBUS] Write register blocked — bus paused (SD exclusive)\n");
        return false;
    }

    if (slave_addr < 1 || slave_addr > 247)
    {
        LOG_E("[MODBUS] Invalid slave address: %d\n", slave_addr);
        return false;
    }

    mb_stats.tx_count++;

    ModbusRawResult result = modbus_raw_request(slave_addr, 0x06, reg_addr, value);

    if (result.status != 0x00) // ku8MBSuccess = 0
    {
        mb_stats.err_count++;
        mb_stats.last_err_code = result.status;
        mb_stats.last_err_time = millis();
        LOG_E("[MODBUS] FC06 Write S%d R%04X=%u FAILED: err=%d\n",
              slave_addr, reg_addr, value, result.status);
        return false;
    }

    mb_stats.rx_count++;
    LOG_I("[MODBUS] FC06 Write S%d R%04X = %u OK\n", slave_addr, reg_addr, value);
    return true;
}

// ─── FC16: Write Multiple Registers ───────────────────────────
bool modbus_write_registers(uint8_t slave_addr, uint16_t start_addr,
                            uint16_t count, const uint16_t *values)
{
    if (modbus_is_paused())
    {
        LOG_E("[MODBUS] Write registers blocked — bus paused (SD exclusive)\n");
        return false;
    }

    if (slave_addr < 1 || slave_addr > 247)
    {
        LOG_E("[MODBUS] Invalid slave address: %d\n", slave_addr);
        return false;
    }

    if (count == 0 || count > 123)
    {
        LOG_E("[MODBUS] Invalid register count: %d (1-123)\n", count);
        return false;
    }

    if (!values)
    {
        LOG_E("[MODBUS] NULL values pointer\n");
        return false;
    }

    // modbus_fc16_write_registers increments tx_count internally
    uint8_t result = modbus_fc16_write_registers(slave_addr, start_addr, count, values);

    if (result != 0x00) // ku8MBSuccess = 0
    {
        mb_stats.err_count++;
        mb_stats.last_err_code = result;
        mb_stats.last_err_time = millis();
        LOG_E("[MODBUS] FC16 Write S%d R%04X count=%d FAILED: err=%d\n",
              slave_addr, start_addr, count, result);
        return false;
    }

    mb_stats.rx_count++;
    LOG_I("[MODBUS] FC16 Write S%d R%04X..R%04X (%d regs) OK\n",
          slave_addr, start_addr, start_addr + count - 1, count);
    return true;
}

// ─── Slave ID Write Convenience ────────────────────────────────
// Writes new_id to the specified register on the current slave,
// then waits for the device to reconfigure, and verifies by
// reading a register from the NEW address.
bool modbus_write_slave_id(uint8_t slave_addr, uint16_t id_register_addr, uint8_t new_id)
{
    if (new_id < 1 || new_id > 247)
    {
        LOG_E("[MODBUS] Invalid new slave ID: %d (must be 1-247)\n", new_id);
        return false;
    }

    if (new_id == slave_addr)
    {
        LOG_E("[MODBUS] New ID %d same as current — no change needed\n", new_id);
        return false;
    }

    LOG_I("[MODBUS] Writing slave ID: S%d → S%d (reg 0x%04X)\n",
          slave_addr, new_id, id_register_addr);

    // Step 1: Write new ID to the register
    if (!modbus_write_register(slave_addr, id_register_addr, new_id))
    {
        LOG_E("[MODBUS] Failed to write new slave ID to S%d\n", slave_addr);
        return false;
    }

    // Step 2: Wait for device to apply new address
    // Most Modbus devices need 100-500ms to reconfigure
    LOG_I("[MODBUS] Waiting 500ms for device to apply new address...\n");
    delay(500);

    // Step 3: Verify by reading a register from the NEW address
    // Try up to 3 times with increasing delay
    for (uint8_t attempt = 0; attempt < 3; attempt++)
    {
        uint16_t readback = 0;
        if (modbus_read_register(new_id, REG_HOLDING, id_register_addr, &readback))
        {
            if (readback == new_id)
            {
                LOG_I("[MODBUS] Slave ID change verified: S%d now at S%d (readback=%u)\n",
                      slave_addr, new_id, readback);
                return true;
            }
            else
            {
                LOG_E("[MODBUS] Slave ID readback mismatch: expected %d, got %u\n",
                      new_id, readback);
                return false;
            }
        }

        // Device didn't respond at new address yet — wait and retry
        LOG_I("[MODBUS] Retry %d/3: no response at new address S%d\n",
              attempt + 1, new_id);
        delay(500);
    }

    LOG_E("[MODBUS] Slave ID verification FAILED — no response at new address S%d\n", new_id);
    return false;
}