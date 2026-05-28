/**
 * modbus_handler.cpp — Modbus RTU Master for Modbus-MQTT Bridge
 * 
 * Handles RS485 communication, auto-detection, and polling.
 * Uses the ModbusMaster library for RTU framing.
 * Supports KC868-HA V2 and Generic Modbus profiles.
 */

#include <Arduino.h>
#include <ModbusMaster.h>
#include "modbus_mqtt_ha_bridge.h"

// ─── RS485 Transceiver Control ──────────────────────────────────
static ModbusMaster node;

// ─── Modbus Statistics ─────────────────────────────────────────
ModbusStats mb_stats = {0, 0, 0, 0, 0, 0};

void modbus_stats_reset() {
    memset(&mb_stats, 0, sizeof(mb_stats));
}

// Pre/post transmission callbacks for RS485 DE/RE control
static void pre_transmission() {
    if (cfg.pin_rs485_de >= 0) digitalWrite(cfg.pin_rs485_de, HIGH);  // Enable transmit
    delayMicroseconds(200);           // Settling time
}

static void post_transmission() {
    delayMicroseconds(200);
    if (cfg.pin_rs485_de >= 0) digitalWrite(cfg.pin_rs485_de, LOW);   // Back to receive
}

// ─── Initialize ─────────────────────────────────────────────────
void modbus_init() {
    // Serial2 for RS485
    Serial2.begin(MODBUS_BAUD, MODBUS_PARITY, cfg.pin_rs485_rx, cfg.pin_rs485_tx);
    
    // Initialize ModbusMaster on Serial2
    node.begin(1, Serial2);  // Temporary slave addr, we change it per-call
    
    // RS485 DE pin init
    if (cfg.pin_rs485_de >= 0) {
        pinMode(cfg.pin_rs485_de, OUTPUT);
        digitalWrite(cfg.pin_rs485_de, LOW);
    }
    
    // Set RS485 control callbacks
    node.preTransmission(pre_transmission);
    node.postTransmission(post_transmission);
    
    // Reset stats
    modbus_stats_reset();
    
    LOG_I("[MODBUS] Initialized: %d baud, 8N1, RX=%d, TX=%d, DE=%d, Profile=%d, Poll=%dms\n",
        MODBUS_BAUD, cfg.pin_rs485_rx, cfg.pin_rs485_tx, cfg.pin_rs485_de, cfg.mb_profile, cfg.mb_poll_ms);
}

// ─── Change active slave address (cached — skip re-init if same) ──
static uint8_t current_slave = 0;
static void set_slave(uint8_t addr) {
    if (addr == current_slave) return;  // Already addressing this slave
    node.begin(addr, Serial2);
    current_slave = addr;
    mb_stats.last_slave_addr = addr;
}

// ─── Set adaptive timeout based on module online state ────────
void modbus_set_timeout_for_module(bool online) {
    ModbusMaster::ku16MBResponseTimeout = online ? MODBUS_TIMEOUT_ONLINE : MODBUS_TIMEOUT_OFFLINE;
}

// ─── Raw Modbus request for TCP gateway ────────────────────────
ModbusRawResult modbus_raw_request(uint8_t slave, uint8_t func,
    uint16_t start, uint16_t count_or_value,
    uint16_t count2, const uint16_t *values) {
    
    ModbusRawResult result;
    result.status = 0xFF;
    result.resp_len = 0;
    
    node.begin(slave, Serial2);
    modbus_set_timeout_for_module(true);
    
    switch (func) {
        case 0x01: result.status = node.readCoils(start, count_or_value); break;
        case 0x02: result.status = node.readDiscreteInputs(start, count_or_value); break;
        case 0x03: result.status = node.readHoldingRegisters(start, count_or_value); break;
        case 0x04: result.status = node.readInputRegisters(start, count_or_value); break;
        case 0x05: result.status = node.writeSingleCoil(start, count_or_value); break;
        case 0x06: result.status = node.writeSingleRegister(start, count_or_value); break;
        case 0x0F: {
            // Write Multiple Coils: set transmit buffer, then call 2-arg version
            node.clearTransmitBuffer();
            for (uint16_t i = 0; i < count_or_value && i < 2000; i++) {
                bool coil_on = values && (values[i / 16] & (1 << (i % 16)));
                node.setTransmitBuffer(i, coil_on ? 0xFF00 : 0x0000);
            }
            result.status = node.writeMultipleCoils(start, count_or_value);
            break;
        }
        case 0x10: {
            // Write Multiple Registers
            for (uint16_t i = 0; i < count_or_value && result.status == node.ku8MBSuccess; i++) {
                result.status = node.writeSingleRegister(start + i, values ? values[i] : 0);
            }
            break;
        }
        default:
            result.status = node.ku8MBIllegalFunction;
            return result;
    }
    
    // Extract response data for read functions
    if (result.status == node.ku8MBSuccess && (func <= 0x04)) {
        uint16_t words = (func <= 0x02) ? (count_or_value + 15) / 16 : count_or_value;
        result.resp_len = words;
        for (uint16_t i = 0; i < words && i < 125; i++) {
            result.resp_buf[i] = node.getResponseBuffer(i);
        }
    }
    
    return result;
}

// Set Modbus response timeout (ms)
void modbus_set_timeout(uint16_t ms) {
    ModbusMaster::ku16MBResponseTimeout = ms;
}

// ─── Get effective profile for a module ────────────────────────
uint8_t effective_profile(Slave_Module *mod) {
    if (mod && mod->profile != 0) return mod->profile;
    return cfg.mb_profile;
}

// ─── Read Model Identification (KC868-HA V2 only) ─────────────
bool modbus_read_identification(uint8_t slave, HA_Model &model) {
    // Only KC868-HA V2 profile supports identification registers
    if (cfg.mb_profile == MB_PROFILE_GENERIC) {
        // For generic profile, use default model info
        model.model_id = 0;
        model.firmware_ver = 0;
        model.serial_number = 0;
        strncpy(model.model_name, "Generic", sizeof(model.model_name));
        return true;
    }
    
    set_slave(slave);
    mb_stats.tx_count++;
    
    // Read 4 holding registers starting at REG_MODEL_ID (0x0064)
    uint8_t result = node.readHoldingRegisters(REG_MODEL_ID, 4);
    
    if (result != node.ku8MBSuccess) {
        mb_stats.err_count++;
        mb_stats.last_err_code = result;
        mb_stats.last_err_time = millis();
        return false;
    }
    
    mb_stats.rx_count++;
    
    // KC868-HA V2: always 6 DI + 6 Relay — read model_id & firmware only
    model.model_id = node.getResponseBuffer(0);        // REG_MODEL_ID
    model.firmware_ver = node.getResponseBuffer(1);   // REG_FW_VERSION
    // relay_count and di_count are fixed constants (HA_V2_RELAY_COUNT, HA_V2_DI_COUNT)
    
    // Derive model name from model_id
    switch (model.model_id) {
        case 1:  strncpy(model.model_name, "KC868-HA1",  sizeof(model.model_name)); break;
        case 2:  strncpy(model.model_name, "KC868-HA2",  sizeof(model.model_name)); break;
        case 4:  strncpy(model.model_name, "KC868-HA4",  sizeof(model.model_name)); break;
        case 8:  strncpy(model.model_name, "KC868-HA8",  sizeof(model.model_name)); break;
        case 16: strncpy(model.model_name, "KC868-HA16", sizeof(model.model_name)); break;
        default:
            snprintf(model.model_name, sizeof(model.model_name), "HA-UNKNOWN-%d", model.model_id);
            // KC868-HA V2: always 6 DI + 6 Relay — no fallback needed
            strlcpy(model.model_name, "KC868-HA V2", sizeof(model.model_name));
            break;
    }
    
    // Read serial number (2 registers)
    mb_stats.tx_count++;
    result = node.readHoldingRegisters(REG_SERIAL_LO, 2);
    if (result == node.ku8MBSuccess) {
        mb_stats.rx_count++;
        model.serial_number = node.getResponseBuffer(1);
        model.serial_number = (model.serial_number << 16) | node.getResponseBuffer(0);
    } else {
        mb_stats.err_count++;
        mb_stats.last_err_code = result;
        mb_stats.last_err_time = millis();
        model.serial_number = 0;
    }
    
    return true;
}

// ─── Read Discrete Inputs (FC02) ───────────────────────────────
bool modbus_read_discrete_inputs(uint8_t slave, uint8_t count, bool *states) {
    set_slave(slave);
    mb_stats.tx_count++;
    
    uint16_t start_addr = (cfg.mb_profile == MB_PROFILE_GENERIC) ? cfg.mb_reg_di_start : 0x0000;
    uint8_t result = node.readDiscreteInputs(start_addr, count);
    
    if (result != node.ku8MBSuccess) {
        mb_stats.err_count++;
        mb_stats.last_err_code = result;
        mb_stats.last_err_time = millis();
        return false;
    }
    
    mb_stats.rx_count++;
    
    // Response buffer contains packed bits
    uint8_t byte_count = node.getResponseBuffer(0);
    for (uint8_t i = 0; i < count && i < 16; i++) {
        uint8_t byte_idx = 1 + (i / 8);
        uint8_t bit_idx = i % 8;
        if (byte_idx < (1 + (count + 7) / 8)) {
            states[i] = (node.getResponseBuffer(byte_idx) >> bit_idx) & 0x01;
        }
    }
    
    return true;
}

// ─── Read Coils (FC01) ─────────────────────────────────────────
bool modbus_read_coils(uint8_t slave, uint8_t count, bool *states) {
    set_slave(slave);
    mb_stats.tx_count++;
    
    uint16_t start_addr = (cfg.mb_profile == MB_PROFILE_GENERIC) ? cfg.mb_reg_coil_start : 0x0000;
    uint8_t result = node.readCoils(start_addr, count);
    
    if (result != node.ku8MBSuccess) {
        mb_stats.err_count++;
        mb_stats.last_err_code = result;
        mb_stats.last_err_time = millis();
        return false;
    }
    
    mb_stats.rx_count++;
    
    uint8_t byte_count = node.getResponseBuffer(0);
    for (uint8_t i = 0; i < count && i < 16; i++) {
        uint8_t byte_idx = 1 + (i / 8);
        uint8_t bit_idx = i % 8;
        if (byte_idx < (1 + (count + 7) / 8)) {
            states[i] = (node.getResponseBuffer(byte_idx) >> bit_idx) & 0x01;
        }
    }
    
    return true;
}

// ─── Write Single Coil (FC05) ──────────────────────────────────
bool modbus_write_coil(uint8_t slave, uint8_t coil, bool state) {
    set_slave(slave);
    mb_stats.tx_count++;
    
    uint16_t coil_addr = (cfg.mb_profile == MB_PROFILE_GENERIC) ? (cfg.mb_reg_coil_start + coil) : coil;
    
    uint8_t result;
    if (state) {
        result = node.writeSingleCoil(coil_addr, 1);
    } else {
        result = node.writeSingleCoil(coil_addr, 0);
    }
    
    if (result != node.ku8MBSuccess) {
        mb_stats.err_count++;
        mb_stats.last_err_code = result;
        mb_stats.last_err_time = millis();
        return false;
    }
    
    mb_stats.rx_count++;
    return true;
}

// ─── Write Multiple Coils (FC15) ───────────────────────────────
bool modbus_write_coils(uint8_t slave, uint8_t count, const bool *states) {
    set_slave(slave);
    mb_stats.tx_count++;
    
    uint16_t start_addr = (cfg.mb_profile == MB_PROFILE_GENERIC) ? cfg.mb_reg_coil_start : 0x0000;
    
    // Use setTransmitBuffer + writeMultipleCoils (2-arg version)
    // Pack bools into transmit buffer words
    for (uint8_t i = 0; i < count && i < 16; i++) {
        uint8_t word_idx = i / 16;
        uint16_t bit_mask = 0;
        for (uint8_t b = 0; b < 16 && (i + b) < count; b++) {
            if (states[i + b]) {
                bit_mask |= (1 << b);
            }
        }
        node.setTransmitBuffer(word_idx, bit_mask);
        i += 15;  // Skip ahead
    }
    
    uint8_t result = node.writeMultipleCoils(start_addr, count);
    if (result != node.ku8MBSuccess) {
        mb_stats.err_count++;
        mb_stats.last_err_code = result;
        mb_stats.last_err_time = millis();
        return false;
    }
    
    mb_stats.rx_count++;
    return true;
}

// ─── Read DI + DO Combined (FC03 @ 0x00C8) — KC868-HA V2 ────
// Single FC03 transaction reads 2 holding registers:
//   Reg 0 (0x00C8): DI bitmask — bit i = DI(i+1) state
//   Reg 1 (0x00C9): DO bitmask — bit i = Relay(i+1) state
// Returns true on success, fills di_states[] and do_states[] arrays.
bool modbus_read_di_do_combined(uint8_t slave, uint8_t di_count, uint8_t do_count, bool *di_states, bool *do_states) {
    // Only KC868-HA V2 profile supports this register
    if (cfg.mb_profile == MB_PROFILE_GENERIC) {
        // Generic profile: no 0xC8 register, fall back to separate reads
        return false;
    }
    
    set_slave(slave);
    mb_stats.tx_count++;
    
    // Read 2 holding registers starting at REG_DI_DO_STATUS (0x00C8)
    uint8_t result = node.readHoldingRegisters(REG_DI_DO_STATUS, 2);
    
    if (result != node.ku8MBSuccess) {
        mb_stats.err_count++;
        mb_stats.last_err_code = result;
        mb_stats.last_err_time = millis();
        return false;
    }
    
    mb_stats.rx_count++;
    
    // Register 0: DI bitmask
    uint16_t di_mask = node.getResponseBuffer(0);
    for (uint8_t i = 0; i < di_count && i < 16; i++) {
        di_states[i] = (di_mask >> i) & 0x01;
    }
    
    // Register 1: DO (relay) bitmask
    uint16_t do_mask = node.getResponseBuffer(1);
    for (uint8_t i = 0; i < do_count && i < 16; i++) {
        do_states[i] = (do_mask >> i) & 0x01;
    }
    
    LOG_D("[MODBUS] Combined DI+DO S%d: DI=0x%04X DO=0x%04X\n", slave, di_mask, do_mask);
    
    return true;
}

// ─── Polling Loop ──────────────────────────────────────────────
void modbus_poll_loop() {
    // Called from main loop — currently polling is handled in poll_all_modules()
    // This function is here for future expansion (e.g., async Modbus)
}