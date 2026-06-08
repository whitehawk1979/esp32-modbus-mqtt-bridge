/**
 * modbus_scan.h — Extended Modbus Scan (Device + Register Discovery)
 *
 * Waveshare ESP32-S3-ETH V1.0 Modbus-MQTT Bridge
 *
 * Provides:
 *   - Device scan: FC43 Read Device Identification (existing)
 *   - Register scan: FC03 Read Holding Registers probe
 *   - Coil scan: FC01 Read Coils probe
 *   - DI scan: FC02 Read Discrete Inputs probe
 *
 * Used by web UI /scan endpoint and API /api/scan.
 */
#ifndef MODBUS_SCAN_H
#define MODBUS_SCAN_H

#include <Arduino.h>

// ─── Scan result limits ────────────────────────────────────
#define REG_SCAN_MAX_RESULTS 64
#define COIL_SCAN_MAX_GROUPS 32

// ─── Register scan result entry ────────────────────────────
struct RegScanEntry {
    uint16_t addr;       // Register address
    uint16_t value;      // Current value (if readable)
    bool     readable;   // FC03 responded
    bool     writable;   // FC06 write test succeeded (optional)
};

// ─── Coil/DI scan group result ──────────────────────────────
struct CoilScanGroup {
    uint16_t start_addr; // Starting address
    uint16_t count;      // Number of consecutive coils/DIs found
};

// ─── Full scan results for one slave ───────────────────────
struct SlaveScanResult {
    uint8_t          slave_addr;
    bool             identified;      // FC43 succeeded
    char             model_name[32];
    uint8_t          model_id;
    uint16_t         firmware_ver;
    uint32_t         serial_number;
    // Register scan
    RegScanEntry     regs[REG_SCAN_MAX_RESULTS];
    uint8_t          reg_count;
    // Coil scan
    CoilScanGroup   coils[COIL_SCAN_MAX_GROUPS];
    uint8_t          coil_group_count;
    // DI scan
    CoilScanGroup   dis[COIL_SCAN_MAX_GROUPS];
    uint8_t          di_group_count;
};

// ─── API ────────────────────────────────────────────────────

/**
 * Start an extended scan of a single slave address.
 * Probes FC43 (identification), FC03 (holding registers), 
 * FC01 (coils), FC02 (discrete inputs).
 * Results are available via scan_get_results() when done.
 */
void scan_slave_extended(uint8_t slave_addr, 
                         uint16_t reg_start = 0, 
                         uint16_t reg_end = 100,
                         bool test_writable = false);

/**
 * Check if extended scan is in progress.
 */
bool scan_slave_extended_active();

/**
 * Run one step of the extended scan (call from loop).
 * Returns true when scan is complete.
 */
bool scan_slave_extended_step();

/**
 * Get scan results (valid after scan_slave_extended_active() returns false).
 */
const SlaveScanResult& scan_slave_extended_results();

/**
 * Reset scan results.
 */
void scan_slave_extended_reset();

#endif // MODBUS_SCAN_H