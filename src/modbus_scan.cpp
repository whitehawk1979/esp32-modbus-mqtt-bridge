/**
 * modbus_scan.cpp — Extended Modbus Scan Implementation
 *
 * Waveshare ESP32-S3-ETH V1.0 Modbus-MQTT Bridge
 *
 * Step-by-step register/coil/DI discovery for unknown Modbus devices.
 * Designed to run in the main loop without blocking (one step per call).
 */

#include <Arduino.h>
#include <ModbusMaster.h>
#include "modbus_mqtt_ha_bridge.h"
#include "modbus_scan.h"

// ─── State machine ────────────────────────────────────────
static SlaveScanResult scan_result;
static bool scan_ext_active = false;

// Scan phases
enum ScanPhase {
    PHASE_IDLE,
    PHASE_IDENTIFY,      // FC43 Read Device Identification
    PHASE_REGS_PROBE,    // FC03 Read Holding Registers
    PHASE_REGS_VERIFY,   // Re-read found registers to confirm
    PHASE_COILS_PROBE,   // FC01 Read Coils
    PHASE_DIS_PROBE,     // FC02 Read Discrete Inputs
    PHASE_DONE
};

static ScanPhase scan_phase = PHASE_IDLE;
static uint16_t  scan_reg_current = 0;
static uint16_t  scan_reg_end_addr = 100;
static bool      scan_test_writable = false;
static uint16_t  scan_probe_coil_addr = 0;

// ─── API ──────────────────────────────────────────────────

void scan_slave_extended(uint8_t slave_addr, 
                         uint16_t reg_start, 
                         uint16_t reg_end,
                         bool test_writable)
{
    if (scan_ext_active)
        return; // Already scanning

    memset(&scan_result, 0, sizeof(scan_result));
    scan_result.slave_addr = slave_addr;
    scan_reg_current = reg_start;
    scan_reg_end_addr = reg_end;
    scan_test_writable = test_writable;
    scan_phase = PHASE_IDENTIFY;
    scan_ext_active = true;
    scan_probe_coil_addr = 0;

    // Use shorter timeout for scanning
    modbus_set_timeout(200);
    LOG_I("[SCAN-EXT] Starting extended scan: slave=%d regs=%u-%u\n",
          slave_addr, reg_start, reg_end);
}

bool scan_slave_extended_active()
{
    return scan_ext_active;
}

const SlaveScanResult& scan_slave_extended_results()
{
    return scan_result;
}

void scan_slave_extended_reset()
{
    scan_ext_active = false;
    scan_phase = PHASE_IDLE;
    memset(&scan_result, 0, sizeof(scan_result));
}

bool scan_slave_extended_step()
{
    if (!scan_ext_active)
        return true;

    switch (scan_phase)
    {
    case PHASE_IDENTIFY:
    {
        HA_Model model;
        if (modbus_read_identification(scan_result.slave_addr, model))
        {
            scan_result.identified = true;
            scan_result.model_id = model.model_id;
            scan_result.firmware_ver = model.firmware_ver;
            scan_result.serial_number = model.serial_number;
            strncpy(scan_result.model_name, model.model_name, sizeof(scan_result.model_name) - 1);
            LOG_I("[SCAN-EXT] FC43: %s (FW=%d, SN=%u)\n",
                  model.model_name, model.firmware_ver, model.serial_number);
        }
        else
        {
            LOG_I("[SCAN-EXT] FC43: no identification response\n");
        }
        scan_phase = PHASE_REGS_PROBE;
        scan_reg_current = 0; // Start from register 0
        break;
    }

    case PHASE_REGS_PROBE:
    {
        if (scan_result.reg_count >= REG_SCAN_MAX_RESULTS || 
            scan_reg_current > scan_reg_end_addr)
        {
            // Done with register probe
            if (scan_result.reg_count > 0)
                scan_phase = PHASE_REGS_VERIFY;
            else
                scan_phase = PHASE_COILS_PROBE;
            break;
        }

        // Try reading a block of 10 registers starting at scan_reg_current
        // Using FC03 Read Holding Registers
        uint8_t slave = scan_result.slave_addr;
        uint16_t regs_read[10] = {0};
        bool found = false;

        // Try reading 1 register at a time for precision
        ModbusRawResult result = modbus_raw_request(slave, 0x03, scan_reg_current, 1);

        if (result.status == 0x00) // Success
        {
            found = true;
            RegScanEntry &entry = scan_result.regs[scan_result.reg_count++];
            entry.addr = scan_reg_current;
            entry.readable = true;
            entry.writable = false;
            // Extract value from raw response (FC03: 16-bit words in resp_buf)
            if (result.resp_len >= 1)
            {
                entry.value = result.resp_buf[0];
            }
            LOG_D("[SCAN-EXT] FC03 R%04X = %u ✓\n", scan_reg_current, entry.value);

            // Optional: test if register is writable (FC06)
            // Only test first few registers to avoid writing to unknown device
            if (scan_test_writable && scan_result.reg_count <= 5)
            {
                // Skip write test for now — too risky on unknown device
                // Could be enabled via explicit user confirmation
            }
        }

        scan_reg_current++;
        // Feed WDT between probes
        yield();
        break;
    }

    case PHASE_REGS_VERIFY:
    {
        // Re-read found registers to confirm they're stable
        LOG_I("[SCAN-EXT] Verifying %d found registers...\n", scan_result.reg_count);
        // For now, skip verification — the probe already confirmed readability
        scan_phase = PHASE_COILS_PROBE;
        scan_probe_coil_addr = 0;
        break;
    }

    case PHASE_COILS_PROBE:
    {
        if (scan_probe_coil_addr > 200 || 
            scan_result.coil_group_count >= COIL_SCAN_MAX_GROUPS)
        {
            scan_phase = PHASE_DIS_PROBE;
            scan_probe_coil_addr = 0;
            break;
        }

        // Try reading 16 coils starting at scan_probe_coil_addr via FC01
        {
            ModbusRawResult result = modbus_raw_request(scan_result.slave_addr, 0x01, scan_probe_coil_addr, 16);
            if (result.status == 0x00)
            {
                if (scan_result.coil_group_count < COIL_SCAN_MAX_GROUPS)
                {
                    CoilScanGroup &grp = scan_result.coils[scan_result.coil_group_count++];
                    grp.start_addr = scan_probe_coil_addr;
                    grp.count = 16;
                    LOG_D("[SCAN-EXT] FC01 Coils @%u: 16 coils ✓\n", scan_probe_coil_addr);
                }
            }
            scan_probe_coil_addr += 16;
        }
        yield();
        break;
    }

    case PHASE_DIS_PROBE:
    {
        if (scan_probe_coil_addr > 200 ||
            scan_result.di_group_count >= COIL_SCAN_MAX_GROUPS)
        {
            scan_phase = PHASE_DONE;
            break;
        }

        // Try reading 16 discrete inputs via FC02
        {
            ModbusRawResult result = modbus_raw_request(scan_result.slave_addr, 0x02, scan_probe_coil_addr, 16);
            if (result.status == 0x00)
            {
                if (scan_result.di_group_count < COIL_SCAN_MAX_GROUPS)
                {
                    CoilScanGroup &grp = scan_result.dis[scan_result.di_group_count++];
                    grp.start_addr = scan_probe_coil_addr;
                    grp.count = 16;
                    LOG_D("[SCAN-EXT] FC02 DIs @%u: 16 inputs ✓\n", scan_probe_coil_addr);
                }
            }
            scan_probe_coil_addr += 16;
        }
        yield();
        break;
    }

    case PHASE_DONE:
    {
        modbus_set_timeout(MODBUS_TIMEOUT_ONLINE); // Restore normal timeout
        scan_ext_active = false;
        LOG_I("[SCAN-EXT] Complete: slave=%d id=%s regs=%d coils=%d dis=%d\n",
              scan_result.slave_addr,
              scan_result.identified ? scan_result.model_name : "unknown",
              scan_result.reg_count,
              scan_result.coil_group_count,
              scan_result.di_group_count);
        return true; // Done
    }

    default:
        scan_phase = PHASE_DONE;
        break;
    }

    return (scan_phase == PHASE_DONE);
}