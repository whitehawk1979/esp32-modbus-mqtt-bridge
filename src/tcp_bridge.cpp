/**
 * tcp_bridge.cpp — Modbus TCP ↔ RTU Gateway (v2.1)
 * 
 * Transparent Modbus TCP gateway using modbus_raw_request().
 * External TCP clients (HA Modbus integration, SCADA, debug tools)
 * communicate with RS485 slaves through the ESP32-S3.
 * 
 * Uses ModbusMaster library via modbus_raw_request():
 * - No race condition with poll loop (bus_busy mutex)
 * - RE/DE transceiver handled by pre/post callbacks
 * - CRC calculated by library
 * - Consistent timeout handling
 * 
 * Supported function codes:
 * - FC01 Read Coils
 * - FC02 Read Discrete Inputs
 * - FC03 Read Holding Registers
 * - FC04 Read Input Registers
 * - FC05 Write Single Coil
 * - FC06 Write Single Register
 * - FC15 Write Multiple Coils
 * - FC16 Write Multiple Registers
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include "modbus_mqtt_ha_bridge.h"

// ─── TCP Server & Clients ──────────────────────────────────────
static WiFiServer tcp_server(TCP_PORT);
static WiFiClient tcp_clients[TCP_MAX_CLIENTS];

// ─── MBAP Header (Modbus TCP) ──────────────────────────────────
struct MBAP_Header {
    uint16_t transaction_id;
    uint16_t protocol_id;    // Always 0x0000 for Modbus
    uint16_t length;         // Following bytes count
    uint8_t  unit_id;        // Modbus slave address
} __attribute__((packed));

// ─── Stats ─────────────────────────────────────────────────────
static uint32_t tcp_req_count = 0;
static uint32_t tcp_err_count = 0;

// ─── Mutex for Modbus bus access ──────────────────────────────
volatile bool bus_busy = false;

// ─── Initialize ─────────────────────────────────────────────────
void tcp_init() {
    if (!cfg.tcp_enabled) {
        LOG_ILN("[TCP] Bridge disabled in config");
        return;
    }
    
    tcp_server.begin();
    LOG_I("[TCP] Modbus TCP gateway started on port %d (max %d clients)\n", 
        TCP_PORT, TCP_MAX_CLIENTS);
}

// ─── Send Modbus TCP Error Response ────────────────────────────
static void send_tcp_error(WiFiClient &client, MBAP_Header &header, 
                           uint8_t func, uint8_t exc_code) {
    uint16_t tid = header.transaction_id;
    client.write((char*)&tid, 2);
    uint16_t zero = 0;
    client.write((char*)&zero, 2);     // protocol_id
    uint16_t len = htons(3);            // unit_id + exception response
    client.write((char*)&len, 2);
    client.write(header.unit_id);
    uint8_t exc[] = {(uint8_t)(func | 0x80), exc_code};
    client.write(exc, 2);
    tcp_err_count++;
}

// ─── Process TCP→RTU Frame via modbus_raw_request ──────────────
static bool process_tcp_frame(WiFiClient &client) {
    // Read MBAP header (7 bytes)
    if (client.available() < 7) return false;
    
    MBAP_Header header;
    client.readBytes((char*)&header, 7);
    
    // Verify protocol ID
    if (ntohs(header.protocol_id) != 0x0000) {
        LOG_ELN("[TCP] Invalid protocol ID, dropping client");
        client.stop();
        return false;
    }
    
    uint16_t frame_len = ntohs(header.length) - 1;
    uint8_t slave_addr = header.unit_id;
    
    if (frame_len > 252 || frame_len == 0) {
        LOG_E("[TCP] Invalid frame length: %d\n", frame_len);
        send_tcp_error(client, header, 0, 0x02);
        return true;
    }
    
    // Read PDU
    uint8_t pdu[252];
    if (client.available() < frame_len) return false;
    client.readBytes((char*)pdu, frame_len);
    
    uint8_t func = pdu[0];
    tcp_req_count++;
    
    // ── Virtual module shortcut: serve from memory, no RTU ────
    for (uint16_t i = 0; i < module_count; i++) {
        if (modules[i].slave_addr == slave_addr && modules[i].is_virtual) {
            // Build response directly from module state
            uint8_t resp_buf[260];
            uint16_t resp_idx = 0;
            resp_buf[resp_idx++] = func;
            
            bool ok = true;
            switch (func) {
                case 0x01: {  // Read Coils (relays as coils)
                    uint16_t start = (frame_len >= 5) ? ((pdu[1] << 8) | pdu[2]) : 0;
                    uint16_t count = (frame_len >= 5) ? ((pdu[3] << 8) | pdu[4]) : 0;
                    if (count < 1 || count > 2000) { ok = false; break; }
                    uint8_t byte_count = (count + 7) / 8;
                    resp_buf[resp_idx++] = byte_count;
                    uint8_t accum = 0;
                    for (uint16_t c = 0; c < count; c++) {
                        bool val = (start + c < modules[i].model.RELAY_COUNT) 
                            ? modules[i].relays[start + c].state : false;
                        if (val) accum |= (1 << (c % 8));
                        if ((c % 8) == 7 || c == count - 1) {
                            resp_buf[resp_idx++] = accum;
                            accum = 0;
                        }
                    }
                    break;
                }
                case 0x02: {  // Read Discrete Inputs
                    uint16_t start = (frame_len >= 5) ? ((pdu[1] << 8) | pdu[2]) : 0;
                    uint16_t count = (frame_len >= 5) ? ((pdu[3] << 8) | pdu[4]) : 0;
                    if (count < 1 || count > 2000) { ok = false; break; }
                    uint8_t byte_count = (count + 7) / 8;
                    resp_buf[resp_idx++] = byte_count;
                    uint8_t accum = 0;
                    for (uint16_t c = 0; c < count; c++) {
                        bool val = (start + c < modules[i].model.DI_COUNT)
                            ? modules[i].inputs[start + c].current : false;
                        if (val) accum |= (1 << (c % 8));
                        if ((c % 8) == 7 || c == count - 1) {
                            resp_buf[resp_idx++] = accum;
                            accum = 0;
                        }
                    }
                    break;
                }
                case 0x03: {  // Read Holding Registers
                    uint16_t start = (frame_len >= 5) ? ((pdu[1] << 8) | pdu[2]) : 0;
                    uint16_t count = (frame_len >= 5) ? ((pdu[3] << 8) | pdu[4]) : 0;
                    if (count < 1 || count > 125) { ok = false; break; }
                    // Special: model identification registers
                    if (start == 0x0001) {
                        resp_buf[resp_idx++] = (uint8_t)(count * 2);
                        for (uint16_t r = 0; r < count; r++) {
                            uint16_t val = 0;
                            if (r == 0) val = modules[i].model.model_id;
                            else if (r == 1) val = 0x4B43; // "KC"
                            resp_buf[resp_idx++] = (val >> 8) & 0xFF;
                            resp_buf[resp_idx++] = val & 0xFF;
                        }
                    } else if (start == 0x00C8) {
                        // DI+DO combined
                        resp_buf[resp_idx++] = (uint8_t)(count * 2);
                        for (uint16_t r = 0; r < count; r++) {
                            uint16_t val = 0;
                            if (r == 0) {
                                for (uint8_t d = 0; d < modules[i].model.DI_COUNT; d++)
                                    if (modules[i].inputs[d].current) val |= (1 << d);
                            } else if (r == 1) {
                                for (uint8_t d = 0; d < modules[i].model.RELAY_COUNT; d++)
                                    if (modules[i].relays[d].state) val |= (1 << d);
                            }
                            resp_buf[resp_idx++] = (val >> 8) & 0xFF;
                            resp_buf[resp_idx++] = val & 0xFF;
                        }
                    } else {
                        ok = false;  // Unknown register range
                    }
                    break;
                }
                case 0x04: {  // Read Input Registers
                    uint16_t count = (frame_len >= 5) ? ((pdu[3] << 8) | pdu[4]) : 0;
                    if (count < 1 || count > 125) { ok = false; break; }
                    resp_buf[resp_idx++] = (uint8_t)(count * 2);
                    for (uint16_t r = 0; r < count; r++) {
                        resp_buf[resp_idx++] = 0;
                        resp_buf[resp_idx++] = 0;
                    }
                    break;
                }
                case 0x05: {  // Write Single Coil
                    uint16_t output = (frame_len >= 5) ? ((pdu[1] << 8) | pdu[2]) : 0;
                    uint16_t value = (frame_len >= 5) ? ((pdu[3] << 8) | pdu[4]) : 0;
                    if (output < modules[i].model.RELAY_COUNT) {
                        modules[i].relays[output].state = (value == 0xFF00);
                        resp_buf[resp_idx++] = pdu[1]; resp_buf[resp_idx++] = pdu[2];
                        resp_buf[resp_idx++] = pdu[3]; resp_buf[resp_idx++] = pdu[4];
                        // Publish new state via MQTT (HA sync)
                        mqtt_publish_relay_state(&modules[i], output);
                        LOG_D("[TCP] Virtual relay %d/%d → %s\n", 
                            slave_addr, output, value == 0xFF00 ? "ON" : "OFF");
                    } else {
                        ok = false;
                    }
                    break;
                }
                case 0x06: {  // Write Single Register - echo back
                    resp_buf[resp_idx++] = pdu[1]; resp_buf[resp_idx++] = pdu[2];
                    resp_buf[resp_idx++] = pdu[3]; resp_buf[resp_idx++] = pdu[4];
                    break;
                }
                default:
                    ok = false;
                    break;
            }
            
            if (!ok) {
                send_tcp_error(client, header, func, 0x02);
                return true;
            }
            
            // Send MBAP + response
            uint16_t tid = header.transaction_id;
            client.write((char*)&tid, 2);
            uint16_t zero = 0;
            client.write((char*)&zero, 2);
            uint16_t mbap_len = htons(resp_idx + 1);
            client.write((char*)&mbap_len, 2);
            client.write(header.unit_id);
            client.write((char*)resp_buf, resp_idx);
            return true;
        }
    }
    
    // ── Physical module: forward via ModbusMaster ─────────────
    
    // Wait for bus to be free (poll loop not using it)
    uint32_t wait_start = millis();
    while (bus_busy && millis() - wait_start < 200) {
        delay(1);
    }
    if (bus_busy) {
        LOG_ELN("[TCP] Bus busy timeout");
        send_tcp_error(client, header, func, 0x06);
        return true;
    }
    bus_busy = true;
    
    // Parse request parameters from PDU
    uint16_t start_addr = (frame_len >= 5) ? ((pdu[1] << 8) | pdu[2]) : 0;
    uint16_t quantity = (frame_len >= 5) ? ((pdu[3] << 8) | pdu[4]) : 0;
    
    // Validate ranges
    if (func == 0x01 || func == 0x02) {
        if (quantity < 1 || quantity > 2000) {
            send_tcp_error(client, header, func, 0x02);
            bus_busy = false;
            return true;
        }
    } else if (func == 0x03 || func == 0x04) {
        if (quantity < 1 || quantity > 125) {
            send_tcp_error(client, header, func, 0x02);
            bus_busy = false;
            return true;
        }
    }
    
    // For write multiple, extract values
    uint16_t values[125] = {0};
    if (func == 0x10 && frame_len >= 6) {
        uint8_t byte_count = pdu[5];
        uint16_t reg_count = quantity;
        for (uint16_t i = 0; i < reg_count && i < 125 && (6 + i*2 + 1) < frame_len; i++) {
            values[i] = (pdu[6 + i*2] << 8) | pdu[7 + i*2];
        }
    }
    
    // Execute via modbus_raw_request
    ModbusRawResult mb_result = modbus_raw_request(
        slave_addr, func, start_addr, quantity,
        0, (func == 0x10) ? values : nullptr
    );
    
    bus_busy = false;
    
    if (mb_result.status != 0x00) {  // ku8MBSuccess = 0
        // Map error codes
        uint8_t exc_code;
        switch (mb_result.status) {
            case 0x02: exc_code = 0x01; break;  // Illegal function
            case 0x03: exc_code = 0x02; break;  // Illegal data address
            case 0x04: exc_code = 0x03; break;  // Illegal data value
            case 0x05: exc_code = 0x04; break;  // Slave device failure
            case 0x06: exc_code = 0x04; break;  // Invalid slave ID
            case 0x07: exc_code = 0x0B; break;  // Response timeout → gateway target failure
            default:   exc_code = 0x04; break;
        }
        LOG_E("[TCP] Error FC%02X slave %d: status=%d exc=%02X\n", 
            func, slave_addr, mb_result.status, exc_code);
        send_tcp_error(client, header, func, exc_code);
        return true;
    }
    
    // ─── Build Modbus TCP Response ───────────────────────────
    uint8_t resp_buf[260];
    uint16_t resp_idx = 0;
    
    // Function code
    resp_buf[resp_idx++] = func;
    
    switch (func) {
        case 0x01: case 0x02: {  // Read Coils / Discrete Inputs
            uint8_t byte_count = (quantity + 7) / 8;
            resp_buf[resp_idx++] = byte_count;
            // resp_buf has 16-bit words, need to repack to byte bitmask
            for (uint8_t i = 0; i < byte_count; i++) {
                uint8_t byte_val = 0;
                for (uint8_t b = 0; b < 8 && (i*8+b) < quantity; b++) {
                    if (mb_result.resp_buf[(i*8+b)/16] & (1 << ((i*8+b) % 16))) {
                        byte_val |= (1 << b);
                    }
                }
                resp_buf[resp_idx++] = byte_val;
            }
            break;
        }
        case 0x03: case 0x04: {  // Read Holding / Input Registers
            resp_buf[resp_idx++] = (uint8_t)(quantity * 2);
            for (uint16_t i = 0; i < quantity && i < mb_result.resp_len; i++) {
                resp_buf[resp_idx++] = (mb_result.resp_buf[i] >> 8) & 0xFF;
                resp_buf[resp_idx++] = mb_result.resp_buf[i] & 0xFF;
            }
            break;
        }
        case 0x05: case 0x06: {  // Write Single - echo request
            resp_buf[resp_idx++] = pdu[1]; resp_buf[resp_idx++] = pdu[2];
            resp_buf[resp_idx++] = pdu[3]; resp_buf[resp_idx++] = pdu[4];
            break;
        }
        case 0x0F: case 0x10: {  // Write Multiple - echo start + count
            resp_buf[resp_idx++] = pdu[1]; resp_buf[resp_idx++] = pdu[2];
            resp_buf[resp_idx++] = pdu[3]; resp_buf[resp_idx++] = pdu[4];
            break;
        }
        default:
            break;
    }
    
    // MBAP header
    uint16_t tid = header.transaction_id;
    client.write((char*)&tid, 2);
    uint16_t zero = 0;
    client.write((char*)&zero, 2);     // protocol_id = 0
    uint16_t mbap_len = htons(resp_idx + 1);  // +1 for unit_id
    client.write((char*)&mbap_len, 2);
    client.write(header.unit_id);
    
    // PDU
    client.write((char*)resp_buf, resp_idx);
    
    return true;
}

// ─── Main Loop ─────────────────────────────────────────────────
void tcp_loop() {
    if (!cfg.tcp_enabled) return;
    
    // Accept new connections
    if (tcp_server.hasClient()) {
        bool accepted = false;
        for (uint8_t i = 0; i < TCP_MAX_CLIENTS; i++) {
            if (!tcp_clients[i] || !tcp_clients[i].connected()) {
                tcp_clients[i] = tcp_server.accept();
                LOG_I("[TCP] Client %d connected from %s\n",
                    i, tcp_clients[i].remoteIP().toString().c_str());
                accepted = true;
                break;
            }
        }
        if (!accepted) {
            WiFiClient reject = tcp_server.accept();
            reject.stop();
            LOG_ELN("[TCP] No free client slot, rejected connection");
        }
    }
    
    // Process each client
    for (uint8_t i = 0; i < TCP_MAX_CLIENTS; i++) {
        if (tcp_clients[i] && tcp_clients[i].connected()) {
            if (tcp_clients[i].available() >= 7) {
                process_tcp_frame(tcp_clients[i]);
            }
        } else {
            tcp_clients[i].stop();
        }
    }
}

// ─── Stats query (for web/API) ────────────────────────────────
uint32_t tcp_get_req_count() { return tcp_req_count; }
uint32_t tcp_get_err_count() { return tcp_err_count; }