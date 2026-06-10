/**
 * mqtt_handler.cpp — MQTT Publishing & HA Discovery v2.1
 * Zero-alloc topic helpers (static char buffers, no String heap in hot paths).
 * PSRAM-backed JsonDocument for discovery, PSRAM malloc for serialize buffer.
 * Uses global cfg struct instead of NVRAM reads.
 * Supports hostname-based object_id, custom friendly names,
 * birth message, stats publishing, and DI click events.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include "modbus_mqtt_ha_bridge.h"

#ifdef USE_W5500
#include <Ethernet.h>
extern EthernetClient eth_tcp_client; // defined in eth_handler.cpp (Arduino Ethernet lib)
#endif

// Client instances for different transport options
static WiFiClientSecure mqtt_wifi_secure;
static WiFiClient mqtt_wifi_plain;
static PubSubClient mqtt;
static void mqtt_callback(char *topic, byte *payload, unsigned int length);

// Track which transport MQTT is using (for logging + reconnect switching)
static bool mqtt_on_lan = false;

// ─── MQTT reconnect counter (for HA sensor) ──────────────────────
static uint32_t mqtt_reconnect_total = 0;
uint32_t mqtt_get_reconnects() { return mqtt_reconnect_total; }

// ─── Safe MQTT publish — skips and logs if not connected ────────
static bool mqtt_pub(const char *topic, const char *payload, bool retained = false)
{
    if (!mqtt.connected())
        return false;
    return mqtt.publish(topic, payload, retained);
}

static bool mqtt_pub(const char *topic, const char *payload, unsigned int len, bool retained = false)
{
    if (!mqtt.connected())
        return false;
    return mqtt.publish(topic, (const uint8_t *)payload, len, retained);
}
bool mqtt_is_on_lan() { return mqtt_on_lan; }

// ─── Zero-alloc topic helpers (static char buffers, no String heap) ──
// Each function returns a pointer to its own static buffer.
// Thread-safety: single-threaded Arduino loop — safe.
// Callers must use the result before the next call to the same function.

static const char *topic_base(uint8_t slave, const char *suffix = "")
{
    static char buf[128];
    if (slave > 0 && suffix[0] != '\0')
        snprintf(buf, sizeof(buf), "%s/ha_v2/%u/%s", cfg.mqtt_prefix, slave, suffix);
    else if (slave > 0)
        snprintf(buf, sizeof(buf), "%s/ha_v2/%u", cfg.mqtt_prefix, slave);
    else if (suffix[0] != '\0')
        snprintf(buf, sizeof(buf), "%s/ha_v2/%s", cfg.mqtt_prefix, suffix);
    else
        snprintf(buf, sizeof(buf), "%s/ha_v2", cfg.mqtt_prefix);
    return buf;
}

static const char *discovery_topic(const char *domain, const char *obj_id)
{
    static char buf[192];
    snprintf(buf, sizeof(buf), "homeassistant/%s/%s_%s/config", domain, cfg.hostname, obj_id);
    return buf;
}

static const char *unique_id(uint8_t slave, const char *type, uint8_t idx)
{
    static char buf[48];
    snprintf(buf, sizeof(buf), "%s_s%d_%s_%u", cfg.hostname, slave, type, idx);
    return buf;
}

// Suffixed topic helper for relay/DI sub-topics (local buffer, no String heap)
static const char *topic_suffix(uint8_t slave, const char *fmt, uint8_t idx, const char *sub = "")
{
    static char buf[160];
    if (sub[0] != '\0')
        snprintf(buf, sizeof(buf), "%s/ha_v2/%u/%s%u/%s", cfg.mqtt_prefix, slave, fmt, idx, sub);
    else
        snprintf(buf, sizeof(buf), "%s/ha_v2/%u/%s%u", cfg.mqtt_prefix, slave, fmt, idx);
    return buf;
}

// ─── Get display name for a module entity ──────────────────────
// For relays: per-entity name (rn<addr>_<idx>) > ha_name + "Relé N" > fallback
// For DIs: per-entity name (dn<addr>_<idx>) > ha_name + "DI N" > fallback
static String get_entity_name(Slave_Module *mod, const String &suffix)
{
    String ha_name = config_get_ha_name(mod->slave_addr);
    String mqtt_name = config_get_mqtt_name(mod->slave_addr);

    if (ha_name.length() > 0)
    {
        return ha_name + " " + suffix;
    }
    else if (mqtt_name.length() > 0)
    {
        return mqtt_name + " " + suffix;
    }
    else
    {
        return String(mod->model.model_name) + " S" + String(mod->slave_addr) + " " + suffix;
    }
}

// Get per-entity name for a relay (returns empty if not set)
static String get_relay_entity_name(Slave_Module *mod, uint8_t idx)
{
    String custom = config_get_relay_name(mod->slave_addr, idx);
    if (custom.length() > 0)
        return custom;
    return get_entity_name(mod, "Relé " + String(idx + 1));
}

// Get per-entity name for a DI (returns empty if not set)
static String get_di_entity_name(Slave_Module *mod, uint8_t idx)
{
    String custom = config_get_di_name(mod->slave_addr, idx);
    if (custom.length() > 0)
        return custom;
    return get_entity_name(mod, "DI " + String(idx + 1));
}

// ─── Get device name for HA discovery ──────────────────────────
static String get_device_name(Slave_Module *mod)
{
    String ha_name = config_get_ha_name(mod->slave_addr);
    String mqtt_name = config_get_mqtt_name(mod->slave_addr);

    if (ha_name.length() > 0)
    {
        return ha_name;
    }
    else if (mqtt_name.length() > 0)
    {
        return mqtt_name + " (S" + String(mod->slave_addr) + ")";
    }
    else
    {
        return String(mod->model.model_name) + " (S" + String(mod->slave_addr) + ")";
    }
}

// ─── Get manufacturer name based on model ──────────────────────
static String get_manufacturer(Slave_Module *mod)
{
    String model_name = String(mod->model.model_name);
    if (model_name.indexOf("KC868") >= 0)
        return "KinCony";
    if (model_name.indexOf("Waveshare") >= 0)
        return "Waveshare";
    return "Modbus Device";
}

// ─── Init ──────────────────────────────────────────────────────
bool mqtt_is_connected()
{
    return mqtt.connected();
}

// Public wrapper — allows other modules (led_handler, etc.) to publish
bool mqtt_publish_topic(const char *topic, const char *payload, bool retained)
{
    return mqtt_pub(topic, payload, retained);
}

void mqtt_force_disconnect()
{
    if (mqtt.connected())
    {
        mqtt.disconnect();
        LOG_I("[MQTT] Force disconnected by watchdog\n");
    }
}

void mqtt_init()
{
    if (strlen(cfg.mqtt_host) == 0)
    {
        LOG_ELN("[MQTT] No broker configured!");
        return;
    }

    // Select transport: LAN preferred (lower latency, no WDT issues), WiFi fallback
    // TLS only works over WiFi (EthernetClient has no TLS support)
    if (cfg.mqtt_tls)
    {
        mqtt_wifi_secure.setInsecure(); // Skip cert validation (LAN/self-signed broker)
        mqtt.setClient(mqtt_wifi_secure);
        mqtt_on_lan = false;
        LOG_I("[MQTT] Transport: WiFi (TLS enabled)");
    }
#ifdef USE_W5500
    else if (cfg.lan_enabled && eth_is_connected())
    {
        mqtt.setClient(eth_tcp_client);
        mqtt_on_lan = true;
        LOG_I("[MQTT] Transport: LAN/Ethernet (W5500)");
    }
#endif
    else
    {
        mqtt.setClient(mqtt_wifi_plain);
        mqtt_on_lan = false;
        LOG_I("[MQTT] Transport: WiFi (plain)");
    }

    mqtt.setServer(cfg.mqtt_host, cfg.mqtt_port);
    mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
    mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT);
    mqtt.setBufferSize(MQTT_MAX_PACKET);
    mqtt.setCallback(mqtt_callback);

    static char client_id[64];
    snprintf(client_id, sizeof(client_id), "%s_bridge", cfg.hostname);

    LOG_I("[MQTT] Connecting to %s:%d (TLS=%s)...\n", cfg.mqtt_host, cfg.mqtt_port, cfg.mqtt_tls ? "ON" : "OFF");

    if (mqtt.connect(client_id,
                     cfg.mqtt_user[0] ? cfg.mqtt_user : NULL,
                     cfg.mqtt_pass[0] ? cfg.mqtt_pass : NULL,
                     topic_base(0, "status"),
                     MQTT_QOS,
                     true,
                     "offline"))
    {
        LOG_ILN("[MQTT] Connected");
        // Birth message — ensures HA sees device online even if LWT missed
        mqtt_pub(topic_base(0, "status"), "online", true);
        // Ensure HA MQTT integration processes discovery topics
        // (HA won't process retained config topics without this)
        if (cfg.ha_discovery)
            mqtt_pub("homeassistant/status", "online", true);
        // Publish bridge system device discovery
        mqtt_publish_bridge_discovery();
        // Re-publish register discovery for all configured registers
        // (HA may have restarted and needs fresh config topics)
        for (uint8_t i = 0; i < register_count; i++)
        {
            if (registers[i].enabled)
                mqtt_publish_register_discovery(&registers[i]);
            // Feed task WDT during bulk discovery publish (25+ registers)
            esp_task_wdt_reset();
            yield();
        }
#ifdef USE_WS2812
        // ── LED: discovery, subscribe, state ──
        led_setup_discovery();
        mqtt.subscribe("modbusmqtt/led/set", MQTT_QOS);
        led_publish_state();
#endif
    }
    else
    {
        LOG_E("[MQTT] Failed, state=%d\n", mqtt.state());
    }
}

// ─── MQTT Loop with Reconnect (exponential backoff) ───────────
void mqtt_loop()
{
    static uint32_t mqtt_fail_since = 0; // 0 = connected or never failed
    
    if (!mqtt.connected() && strlen(cfg.mqtt_host) > 0)
    {
        static uint32_t last_reconnect = 0;
        static uint8_t reconnect_attempts = 0;
        // Exponential backoff: 2s → 4s → 8s → 16s → 30s (max)
        uint32_t backoff = 2000 << min((uint8_t)reconnect_attempts, (uint8_t)4);
        if (backoff > 30000)
            backoff = 30000;
        if (millis() - last_reconnect > backoff)
        {
            // Re-evaluate transport on each reconnect attempt
            bool want_lan = false;
#ifdef USE_W5500
            want_lan = !cfg.mqtt_tls && cfg.lan_enabled && eth_is_connected();
#endif
            if (want_lan != mqtt_on_lan)
            {
                if (want_lan)
                {
#ifdef USE_W5500
                    mqtt.setClient(eth_tcp_client);
#endif
                    mqtt_on_lan = true;
                    LOG_ILN("[MQTT] Switching to LAN/Ethernet transport");
                }
                else
                {
                    mqtt.setClient(cfg.mqtt_tls ? (Client &)mqtt_wifi_secure : (Client &)mqtt_wifi_plain);
                    mqtt_on_lan = false;
                    LOG_ILN("[MQTT] Switching to WiFi transport");
                }
            }

            static char client_id[64];
            snprintf(client_id, sizeof(client_id), "%s_bridge", cfg.hostname);

            if (mqtt.connect(client_id,
                             cfg.mqtt_user[0] ? cfg.mqtt_user : NULL,
                             cfg.mqtt_pass[0] ? cfg.mqtt_pass : NULL,
                             topic_base(0, "status"),
                             MQTT_QOS,
                             true,
                             "offline"))
            {
                LOG_ILN("[MQTT] Reconnected");
                reconnect_attempts = 0; // Reset backoff on success
                mqtt_reconnect_total++;
                // Birth message on reconnect
                mqtt_pub(topic_base(0, "status"), "online", true);
                // Ensure HA MQTT integration processes discovery topics
                if (cfg.ha_discovery)
                    mqtt_pub("homeassistant/status", "online", true);
                // Publish bridge system device discovery
                mqtt_publish_bridge_discovery();
                // Re-publish register discovery for all configured registers
                for (uint8_t i = 0; i < register_count; i++)
                {
                    if (registers[i].enabled)
                        mqtt_publish_register_discovery(&registers[i]);
                    // Subscribe to writable register set commands
                    if (registers[i].enabled && registers[i].writable)
                    {
                        static char reg_cmd_topic[128];
                        snprintf(reg_cmd_topic, sizeof(reg_cmd_topic),
                                 "%s/reg/%u/%u/set", cfg.mqtt_prefix,
                                 registers[i].slave_addr, registers[i].addr);
                        mqtt.subscribe(reg_cmd_topic, MQTT_QOS);
                    }
                }
#ifdef USE_WS2812
                // ── LED: discovery, subscribe, state ──
                led_setup_discovery();
                mqtt.subscribe("modbusmqtt/led/set", MQTT_QOS);
                led_publish_state();
#endif
                for (uint16_t i = 0; i < module_count; i++)
                {
                    mqtt_subscribe_commands(&modules[i]);
                    // Re-publish last known states to avoid stale data after reconnect
                    mqtt_publish_module_online(&modules[i], modules[i].online);
                    for (uint8_t r = 0; r < modules[i].model.RELAY_COUNT; r++)
                        mqtt_publish_relay_state(&modules[i], r);
                    for (uint8_t d = 0; d < modules[i].model.DI_COUNT; d++)
                        mqtt_publish_di_state(&modules[i], d, modules[i].inputs[d].current);
                }
            }
            else
            {
                reconnect_attempts = min((uint8_t)(reconnect_attempts + 1), (uint8_t)8);
                // Track first failure time for auto-restart
                if (mqtt_fail_since == 0)
                    mqtt_fail_since = millis();
                // Auto-restart if MQTT unreachable for >5 minutes
                if (millis() - mqtt_fail_since > 300000)
                {
                    LOG_E("[MQTT] Unreachable >5min — restarting ESP\n");
                    delay(100);
                    eth_hard_reset_and_restart();
                }
            }
            last_reconnect = millis();
        }
    }
    else
    {
        // Connected — reset failure timer
        mqtt_fail_since = 0;
    }
    mqtt.loop();

    // ─── Periodic bridge state publish (every 30s) ───────────────
    static uint32_t last_bridge_state = 0;
    if (mqtt.connected() && millis() - last_bridge_state > 30000)
    {
        mqtt_publish_bridge_state();
        last_bridge_state = millis();
    }
}

// ─── MQTT Callback (relay commands) ────────────────────────────
static void mqtt_callback(char *topic, byte *payload, unsigned int length)
{
    char msg[256] = {0};  // Larger buffer for LED JSON commands
    if (length >= sizeof(msg))
        length = sizeof(msg) - 1;
    memcpy(msg, payload, length);
    msg[length] = '\0';

    LOG_D("[MQTT-CB] topic='%s' msg='%s'\n", topic, msg);

    // ── LED command (modbusmqtt/led/set) ──
#ifdef USE_WS2812
    if (led_handle_command(topic, msg, length))
        return;  // Handled
#endif

    // ── Register write command: {prefix}/reg/{slave}/{addr}/set ──
    {
        String reg_prefix = String(cfg.mqtt_prefix) + "/reg/";
        String t = String(topic);
        if (t.startsWith(reg_prefix) && t.endsWith("/set"))
        {
            // Parse: reg/{slave}/{addr}/set
            int p1 = reg_prefix.length();
            int p2 = t.indexOf('/', p1);
            int p3 = t.indexOf("/set", p2);
            if (p2 > 0 && p3 > 0)
            {
                int slave = t.substring(p1, p2).toInt();
                int addr = t.substring(p2 + 1, p3).toInt();
                float value = atof(msg);

                // Find matching register
                for (uint8_t i = 0; i < register_count; i++)
                {
                    if (registers[i].slave_addr == slave &&
                        registers[i].addr == addr &&
                        registers[i].writable &&
                        registers[i].enabled)
                    {
                        uint16_t raw = (registers[i].scale > 0)
                            ? (uint16_t)(value * registers[i].scale + 0.5f)
                            : (uint16_t)value;

                        LOG_I("[MQTT] Register write: S%d R%04X = %u (raw from %.2f)\n",
                              slave, addr, raw, value);

                        if (modbus_write_register(slave, addr, raw))
                        {
                            // Update local state immediately
                            registers[i].last_value = value;
                            registers[i].published = true;
                            mqtt_publish_register_value(&registers[i]);
                        }
                        else
                        {
                            // Write failed — republish current value so HA reverts
                            mqtt_publish_register_value(&registers[i]);
                            LOG_E("[MQTT] Write failed: S%d R%04X, reverting state\n", slave, addr);
                        }
                        return; // Handled
                    }
                }
                LOG_E("[MQTT] No writable register found: S%d R%04X\n", slave, addr);
            }
            return; // Register topic, but parse failed
        }
    }

    String t = String(topic);
    String prefix = String(cfg.mqtt_prefix) + "/ha_v2/";
    if (!t.startsWith(prefix))
    {
        LOG_E("[MQTT-CB] prefix mismatch: '%s'\n", prefix.c_str());
        return;
    }

    int p1 = t.indexOf('/', prefix.length());
    int p2 = t.indexOf("/relay/", prefix.length());
    int p3 = t.indexOf("/set", p2);
    LOG_D("[MQTT-CB] p1=%d p2=%d p3=%d\n", p1, p2, p3);
    if (p1 < 0 || p2 < 0 || p3 < 0)
        return;

    int slave = t.substring(prefix.length(), p1).toInt();
    int relay_idx = t.substring(p2 + 7, p3).toInt();

    Slave_Module *mod = nullptr;
    for (uint16_t i = 0; i < module_count; i++)
    {
        if (modules[i].slave_addr == slave)
        {
            mod = &modules[i];
            break;
        }
    }
    if (!mod || relay_idx >= mod->model.RELAY_COUNT)
        return;

    bool state = (strcmp(msg, "ON") == 0 || strcmp(msg, "1") == 0);
    LOG_D("[MQTT] Relay: S%d R%d -> %s\n", slave, relay_idx, state ? "ON" : "OFF");

    if (mod->is_virtual)
    {
        // Virtual module: just update internal state, no Modbus write
        mod->relays[relay_idx].state = state;
        mod->relays[relay_idx].pending = false;
        mqtt_publish_relay_state(mod, relay_idx);
        LOG_D("[MQTT] Virtual relay S%d R%d -> %s (simulated)\n", slave, relay_idx, state ? "ON" : "OFF");
    }
    else if (modbus_write_coil(slave, relay_idx, state))
    {
        // Read-back verification: confirm relay actually switched
        modbus_set_timeout_for_module(true);
        bool coil_states[16] = {0};
        bool verified = false;
        if (modbus_read_coils(slave, mod->model.RELAY_COUNT, coil_states))
        {
            verified = (coil_states[relay_idx] == state);
            if (!verified)
            {
                LOG_E("[MQTT] ⚠ Relay S%d R%d read-back mismatch! Wrote %s, read %s\n",
                      slave,
                      relay_idx,
                      state ? "ON" : "OFF",
                      coil_states[relay_idx] ? "ON" : "OFF");
            }
        }
        mod->relays[relay_idx].state = verified ? coil_states[relay_idx] : state; // Use read-back if available
        mod->relays[relay_idx].pending = false;
        mqtt_publish_relay_state(mod, relay_idx);
    }
}

// ─── Subscribe to relay command topics ─────────────────────────
void mqtt_subscribe_commands(Slave_Module *mod)
{
    for (uint8_t r = 0; r < mod->model.RELAY_COUNT; r++)
    {
        const char *cmd_topic = topic_suffix(mod->slave_addr, "relay/", r, "set");
        bool ok = mqtt.subscribe(cmd_topic, MQTT_QOS);
        LOG_D("[MQTT] Subscribe %s → %s\n", cmd_topic, ok ? "OK" : "FAIL");
    }
}

// ─── HA Discovery ──────────────────────────────────────────────
// ─── Remove HA discovery entities for a module ──────────────────
void mqtt_cleanup_discovery(Slave_Module *mod)
{
    if (!cfg.ha_discovery)
        return;

    // Relays
    for (uint8_t r = 0; r < mod->model.RELAY_COUNT; r++)
    {
        mqtt_pub(discovery_topic("switch", unique_id(mod->slave_addr, "relay", r)), "", true);
    }

    // DI binary sensors
    for (uint8_t d = 0; d < mod->model.DI_COUNT; d++)
    {
        mqtt_pub(discovery_topic("binary_sensor", unique_id(mod->slave_addr, "di", d)), "", true);
        // Click sensors
        mqtt_pub(discovery_topic("sensor", unique_id(mod->slave_addr, "click", d)), "", true);
    }

    // Module status sensor
    static char uid_buf[64];
    snprintf(uid_buf, sizeof(uid_buf), "%s_avail", unique_id(mod->slave_addr, "status", 0));
    mqtt_pub(discovery_topic("binary_sensor", uid_buf), "", true);
    mqtt_pub(discovery_topic("sensor", unique_id(mod->slave_addr, "status", 0)), "", true);

    // Slave address sensor
    mqtt_pub(discovery_topic("sensor", unique_id(mod->slave_addr, "addr", 0)), "", true);

    LOG_I("[MQTT] Cleaned up discovery for module S%d\n", mod->slave_addr);
}

// ─── Set common device block for HA Discovery ────────────────────
// Builds the "device" JSON object with identifiers, name, manufacturer, model, sw, area.
// Reused across all entities in mqtt_publish_discovery() to avoid repeated string allocs.
static void set_device_block(JsonDocument &doc, Slave_Module *mod, const String &area)
{
    static char ident[64];
    snprintf(ident, sizeof(ident), "%s_ha_v2_%d", cfg.hostname, mod->slave_addr);
    doc["device"]["identifiers"] = ident;
    doc["device"]["name"] = get_device_name(mod);
    doc["device"]["mf"] = get_manufacturer(mod);
    doc["device"]["mdl"] = mod->model.model_name;
    doc["device"]["sw"] = FIRMWARE_VERSION;
    if (mod->model.serial_number > 0)
    {
        static char sn_buf[24];
        snprintf(sn_buf, sizeof(sn_buf), "%lu", mod->model.serial_number);
        doc["device"]["sn"] = sn_buf;
    }
    if (area.length() > 0)
        doc["device"]["suggested_area"] = area;
}

// ─── Set common availability block ──────────────────────────────
static void set_availability(JsonDocument &doc)
{
    doc["availability_topic"] = topic_base(0, "status");
    doc["payload_available"] = "online";
    doc["payload_not_available"] = "offline";
}

// ─── Serialize doc and publish to MQTT ──────────────────────────
// Uses PSRAM buffer when available to reduce heap pressure on large payloads.
static bool discovery_publish(const char *topic, JsonDocument &doc, bool retain = true)
{
    if (!mqtt.connected())
    {
        LOG_I("[MQTT] Discovery skip: not connected\n");
        return false;
    }
    size_t json_size = measureJson(doc) + 1;
    // Allocate buffer in PSRAM if available, otherwise heap
    char *buf = reinterpret_cast<char *>(psram_malloc(json_size));
    if (!buf)
    {
        LOG_E("[MQTT] Discovery publish: OOM (%u bytes)\n", json_size);
        return false;
    }
    serializeJson(doc, buf, json_size);
    bool ok = mqtt_pub(topic, buf, retain);
    free(buf);
    return ok;
}

void mqtt_publish_discovery(Slave_Module *mod)
{
    if (!mqtt.connected())
    {
        LOG_I("[MQTT] Discovery skip: not connected (S%d)\n", mod->slave_addr);
        return;
    }
    if (!cfg.ha_discovery)
    {
        LOG_I("[MQTT] Discovery disabled, skipping module %d\n", mod->slave_addr);
        return;
    }

    JsonDocument doc(PsramAllocator::instance());

    // Common area for all entities of this module (computed once)
    String area = config_get_module_area(mod->slave_addr);

    // Static buffers for object_id construction (avoid repeated String concatenation)
    static char obj_id_buf[80];

    // Relay Switches
    for (uint8_t r = 0; r < mod->model.RELAY_COUNT; r++)
    {
        doc.clear();
        const char *uid = unique_id(mod->slave_addr, "relay", r);
        snprintf(obj_id_buf, sizeof(obj_id_buf), "%s_s%d_r%d", cfg.hostname, mod->slave_addr, r);

        doc["name"] = get_relay_entity_name(mod, r);
        doc["unique_id"] = uid;
        doc["object_id"] = obj_id_buf;
        doc["state_topic"] = topic_suffix(mod->slave_addr, "relay/", r, "state");
        doc["command_topic"] = topic_suffix(mod->slave_addr, "relay/", r, "set");
        doc["payload_on"] = "ON";
        doc["payload_off"] = "OFF";
        doc["state_on"] = "ON";
        doc["state_off"] = "OFF";
        doc["qos"] = MQTT_QOS;
        doc["retain"] = MQTT_RETAIN_STATE;
        set_availability(doc);
        set_device_block(doc, mod, area);

        discovery_publish(discovery_topic("switch", uid), doc);

        // Feed task WDT during bulk discovery publish
        esp_task_wdt_reset();
    }

    // DI Binary Sensors (state ON/OFF)
    for (uint8_t d = 0; d < mod->model.DI_COUNT; d++)
    {
        doc.clear();
        const char *uid = unique_id(mod->slave_addr, "di", d);
        snprintf(obj_id_buf, sizeof(obj_id_buf), "%s_s%d_di%d", cfg.hostname, mod->slave_addr, d);

        doc["name"] = get_di_entity_name(mod, d);
        doc["unique_id"] = uid;
        doc["object_id"] = obj_id_buf;
        doc["state_topic"] = topic_suffix(mod->slave_addr, "di/", d, "state");
        doc["payload_on"] = "ON";
        doc["payload_off"] = "OFF";
        doc["qos"] = MQTT_QOS;
        doc["device_class"] = "opening";
        doc["entity_category"] = "diagnostic";
        doc["icon"] = "mdi:light-switch";
        set_availability(doc);
        set_device_block(doc, mod, area);

        discovery_publish(discovery_topic("binary_sensor", uid), doc);
        esp_task_wdt_reset();
    }

    // DI Click Event Sensors (for automation triggers)
    for (uint8_t d = 0; d < mod->model.DI_COUNT; d++)
    {
        doc.clear();
        const char *click_uid = unique_id(mod->slave_addr, "di_click", d);

        String di_custom = config_get_di_name(mod->slave_addr, d);
        String click_name = (di_custom.length() > 0) ? di_custom + " Kattintás"
                                                     : get_entity_name(mod, "DI " + String(d + 1) + " Kattintás");

        snprintf(obj_id_buf, sizeof(obj_id_buf), "%s_s%d_di%d_click", cfg.hostname, mod->slave_addr, d);

        doc["name"] = click_name;
        doc["unique_id"] = click_uid;
        doc["object_id"] = obj_id_buf;
        doc["state_topic"] = topic_suffix(mod->slave_addr, "di/", d, "click");
        doc["icon"] = "mdi:gesture-tap";
        doc["entity_category"] = "diagnostic";
        set_availability(doc);
        set_device_block(doc, mod, area);

        discovery_publish(discovery_topic("sensor", click_uid), doc);
        esp_task_wdt_reset();
    }

    // Module Status Binary Sensor
    {
        doc.clear();
        static char uid_avail[64];
        snprintf(uid_avail, sizeof(uid_avail), "%s_avail", unique_id(mod->slave_addr, "avail", 0));

        doc["name"] = get_entity_name(mod, "Státusz");
        doc["unique_id"] = uid_avail;
        doc["entity_category"] = "diagnostic";
        doc["state_topic"] = topic_base(mod->slave_addr, "status");
        doc["payload_on"] = "online";
        doc["payload_off"] = "offline";
        set_device_block(doc, mod, area);

        discovery_publish(discovery_topic("binary_sensor", uid_avail), doc);
        esp_task_wdt_reset();
    }

    // Slave Address Sensor
    {
        doc.clear();
        const char *uid_addr = unique_id(mod->slave_addr, "addr", 0);
        snprintf(obj_id_buf, sizeof(obj_id_buf), "%s_s%d_addr", cfg.hostname, mod->slave_addr);

        doc["name"] = get_entity_name(mod, "Slave Cím");
        doc["unique_id"] = uid_addr;
        doc["object_id"] = obj_id_buf;
        doc["state_topic"] = topic_base(mod->slave_addr, "address");
        doc["icon"] = "mdi:identifier";
        doc["entity_category"] = "diagnostic";
        set_availability(doc);
        set_device_block(doc, mod, area);

        discovery_publish(discovery_topic("sensor", uid_addr), doc);
        esp_task_wdt_reset();
    }

    // Publish slave address value
    {
        static char addr_val[8];
        snprintf(addr_val, sizeof(addr_val), "%d", mod->slave_addr);
        mqtt_pub(topic_base(mod->slave_addr, "address"), addr_val, true);
    }

    LOG_I("[MQTT] Discovery: S%d (%dR, %dDI, %dDI-Click, 1Addr)\n",
          mod->slave_addr,
          mod->model.RELAY_COUNT,
          mod->model.DI_COUNT,
          mod->model.DI_COUNT);

    // ── Publish module name and area as retained MQTT sub-topics ──
    String mqtt_name = config_get_mqtt_name(mod->slave_addr);
    String mod_area = config_get_module_area(mod->slave_addr);
    if (mqtt_name.length() > 0)
    {
        mqtt_pub(topic_base(mod->slave_addr, "name"), mqtt_name.c_str(), true);
    }
    if (mod_area.length() > 0)
    {
        mqtt_pub(topic_base(mod->slave_addr, "area"), mod_area.c_str(), true);
    }

    // Publish all relay/DI names as retained sub-topics
    for (uint8_t r = 0; r < mod->model.RELAY_COUNT; r++)
    {
        String rname = config_get_relay_name(mod->slave_addr, r);
        if (rname.length() > 0)
        {
            mqtt_pub(topic_suffix(mod->slave_addr, "relay/", r, "name"), rname.c_str(), true);
        }
    }
    for (uint8_t d = 0; d < mod->model.DI_COUNT; d++)
    {
        String dname = config_get_di_name(mod->slave_addr, d);
        if (dname.length() > 0)
        {
            mqtt_pub(topic_suffix(mod->slave_addr, "di/", d, "name"), dname.c_str(), true);
        }
    }
}

// ─── Publish Relay State ────────────────────────────────────────
void mqtt_publish_relay_state(Slave_Module *mod, uint8_t relay_idx)
{
    mqtt_pub(topic_suffix(mod->slave_addr, "relay/", relay_idx, "state"),
                 mod->relays[relay_idx].state ? "ON" : "OFF", MQTT_RETAIN_STATE);

    // Publish relay name as retained sub-topic (for MQTT Explorer visibility)
    String name = config_get_relay_name(mod->slave_addr, relay_idx);
    if (name.length() > 0)
    {
        mqtt_pub(topic_suffix(mod->slave_addr, "relay/", relay_idx, "name"), name.c_str(), true);
    }

    // Notify WebSocket clients
    ws_notify_relay(mod, relay_idx);
}

// ─── Publish DI State ──────────────────────────────────────────
void mqtt_publish_di_state(Slave_Module *mod, uint8_t di_idx, bool state)
{
    mqtt_pub(topic_suffix(mod->slave_addr, "di/", di_idx, "state"),
                 state ? "ON" : "OFF", MQTT_RETAIN_STATE);

    // Publish DI input type (latch/momentary) when detected
    if (mod->inputs[di_idx].detected_type != DI_TYPE_UNKNOWN)
    {
        mqtt_pub(topic_suffix(mod->slave_addr, "di/", di_idx, "type"),
                 di_input_type_str(mod->inputs[di_idx].detected_type), true);
    }

    // Publish DI name as retained sub-topic (for MQTT Explorer visibility)
    String name = config_get_di_name(mod->slave_addr, di_idx);
    if (name.length() > 0)
    {
        mqtt_pub(topic_suffix(mod->slave_addr, "di/", di_idx, "name"), name.c_str(), true);
    }

    // Notify WebSocket clients
    ws_notify_di(mod, di_idx, state);
}

// ─── Publish Click Event (from callback) ────────────────────────
void mqtt_publish_click_event(Slave_Module *mod, uint8_t di_idx, ClickType ct)
{
    const char *click_str = click_type_str(ct);
    mqtt_pub(topic_suffix(mod->slave_addr, "di/", di_idx, "click"), click_str, false);

    LOG_D("[MQTT] Click: S%d DI%d: %s\n", mod->slave_addr, di_idx + 1, click_str);
}

// ─── Publish Module Online Status ──────────────────────────────
void mqtt_publish_module_online(Slave_Module *mod, bool online)
{
    mqtt_pub(topic_base(mod->slave_addr, "status"), online ? "online" : "offline", true);
}

// ─── Publish Modbus Stats ──────────────────────────────────────
void mqtt_publish_stats()
{
    JsonDocument doc(PsramAllocator::instance());
    doc["tx"] = mb_stats.tx_count;
    doc["rx"] = mb_stats.rx_count;
    doc["err"] = mb_stats.err_count;
    doc["last_err_code"] = mb_stats.last_err_code;
    if (mb_stats.last_err_time > 0)
    {
        doc["last_err_s"] = (uint32_t)((millis() - mb_stats.last_err_time) / 1000);
    }
    else
    {
        doc["last_err_s"] = 0;
    }
    doc["uptime_s"] = (uint32_t)(millis() / 1000);

    size_t json_size = measureJson(doc) + 1;
    char *buf = reinterpret_cast<char *>(psram_malloc(json_size));
    if (buf)
    {
        serializeJson(doc, buf, json_size);
        static char stats_topic[128];
        snprintf(stats_topic, sizeof(stats_topic), "%s/ha_v2/stats", cfg.mqtt_prefix);
        mqtt_pub(stats_topic, buf, false);
        free(buf);
    }
}

// ─── Click callback handler for MQTT ────────────────────────────
static Slave_Module *click_current_mod = nullptr;

static void mqtt_click_callback(uint8_t di_idx, uint8_t click_type)
{
    if (!click_current_mod)
        return;
    if (!mqtt.connected())
        return;

    // Map click_type (1=single, 2=double, 3=triple, 4=long) to ClickType
    ClickType ct;
    switch (click_type)
    {
    case 1:
        ct = CLICK_SINGLE;
        break;
    case 2:
        ct = CLICK_DOUBLE;
        break;
    case 3:
        ct = CLICK_TRIPLE;
        break;
    case 4:
        ct = CLICK_HOLD;
        break;
    default:
        ct = CLICK_SINGLE;
        break;
    }
    mqtt_publish_click_event(click_current_mod, di_idx, ct);
}

// ─── Set current module context for click callback ──────────────
void mqtt_set_click_module(Slave_Module *mod)
{
    click_current_mod = mod;
}

// ─── Bridge System Device Discovery ────────────────────────────
void mqtt_publish_bridge_discovery()
{
    if (!cfg.ha_discovery)
        return;

    JsonDocument doc(PsramAllocator::instance());
    static char bridge_id[64];
    snprintf(bridge_id, sizeof(bridge_id), "%s_bridge", cfg.hostname);
    const char *avail_topic = topic_base(0, "status");

    // Helper: common device block
    auto set_device = [&]() {
        doc["device"]["identifiers"] = bridge_id;
        doc["device"]["name"] = String(cfg.hostname) + " Bridge";
        doc["device"]["mf"] = "Waveshare";
        doc["device"]["mdl"] = "ESP32-S3-ETH V1.0";
        doc["device"]["sw"] = FIRMWARE_VERSION;
    };

    // Helper: publish one bridge sensor discovery
    auto publish_sensor = [&](const char *name, const char *uid_suffix,
                              const char *state_topic, const char *device_class = nullptr,
                              const char *unit = nullptr, const char *icon = nullptr) {
        doc.clear();
        static char uid[96];
        snprintf(uid, sizeof(uid), "%s_%s", bridge_id, uid_suffix);
        doc["name"] = name;
        doc["unique_id"] = uid;
        doc["object_id"] = uid;
        doc["state_topic"] = state_topic;
        if (device_class)
            doc["device_class"] = device_class;
        if (unit)
            doc["unit_of_measurement"] = unit;
        if (icon)
            doc["icon"] = icon;
        doc["availability_topic"] = avail_topic;
        doc["payload_available"] = "online";
        doc["payload_not_available"] = "offline";
        set_device();
        discovery_publish(discovery_topic("sensor", uid), doc);
    };

    static char topic_buf[128];

    // 1. WiFi Jel (RSSI)
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/rssi", cfg.mqtt_prefix);
    publish_sensor("WiFi Jel", "rssi", topic_buf, "signal_strength", "dBm", "mdi:wifi");

    // 2. Szabad Memória (Heap)
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/heap", cfg.mqtt_prefix);
    publish_sensor("Szabad Memória", "heap", topic_buf, nullptr, "B", "mdi:memory");

    // 3. Uptime
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/uptime", cfg.mqtt_prefix);
    publish_sensor("Uptime", "uptime", topic_buf, nullptr, "s", "mdi:clock");

    // 4. Hálózat (Interface)
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/interface", cfg.mqtt_prefix);
    publish_sensor("Hálózat", "if", topic_buf, nullptr, nullptr, "mdi:lan");

    // 5. IP Cím
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/ip", cfg.mqtt_prefix);
    publish_sensor("IP Cím", "ip", topic_buf, nullptr, nullptr, "mdi:ip-network");

    // 6. PSRAM Free
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/psram_free", cfg.mqtt_prefix);
    publish_sensor("PSRAM Szabad", "psram_free", topic_buf, nullptr, "B", "mdi:memory");

    // 7. Firmware Version (non-numeric, use as diagnostic)
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/fw_version", cfg.mqtt_prefix);
    {
        doc.clear();
        static char uid[96];
        snprintf(uid, sizeof(uid), "%s_fw", bridge_id);
        doc["name"] = "Firmware";
        doc["unique_id"] = uid;
        doc["object_id"] = uid;
        doc["state_topic"] = topic_buf;
        doc["icon"] = "mdi:chip";
        doc["entity_category"] = "diagnostic";
        doc["availability_topic"] = avail_topic;
        doc["payload_available"] = "online";
        doc["payload_not_available"] = "offline";
        set_device();
        discovery_publish(discovery_topic("sensor", uid), doc);
    }

    // 8. WDT Reboots
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/wdt_reboots", cfg.mqtt_prefix);
    publish_sensor("WDT Újraindítás", "wdt_reboots", topic_buf, nullptr, nullptr, "mdi:restart-alert");

    // 9. WiFi Reconnects
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/wifi_reconnects", cfg.mqtt_prefix);
    publish_sensor("WiFi Újrakapcs.", "wifi_reconnects", topic_buf, nullptr, nullptr, "mdi:wifi-off");

    // 10. TCP Bridge Requests (if TCP enabled)
    if (cfg.tcp_enabled)
    {
        snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/tcp_req_count", cfg.mqtt_prefix);
        publish_sensor("TCP Kérések", "tcp_req", topic_buf, nullptr, nullptr, "mdi:lan-connect");
    }

    LOG_I("[MQTT] Bridge system discovery published (8-10 sensors)\n");
}

// ─── Bridge System State Publishing ────────────────────────────
void mqtt_publish_bridge_state()
{
    if (!mqtt.connected())
        return;

    static char topic_buf[128];

    // RSSI — only valid when WiFi connected
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/rssi", cfg.mqtt_prefix);
    if (WiFi.status() == WL_CONNECTED)
    {
        static char rssi_val[12];
        snprintf(rssi_val, sizeof(rssi_val), "%d", WiFi.RSSI());
        mqtt_pub(topic_buf, rssi_val, false);
    }
    else
    {
        mqtt_pub(topic_buf, "unavailable", false);
    }

    // Free heap
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/heap", cfg.mqtt_prefix);
    {
        static char heap_val[16];
        snprintf(heap_val, sizeof(heap_val), "%u", ESP.getFreeHeap());
        mqtt_pub(topic_buf, heap_val, false);
    }

    // Firmware version
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/fw_version", cfg.mqtt_prefix);
    mqtt_pub(topic_buf, FIRMWARE_VERSION, true);

    // Uptime in seconds
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/uptime", cfg.mqtt_prefix);
    {
        static char uptime_val[16];
        snprintf(uptime_val, sizeof(uptime_val), "%lu", millis() / 1000);
        mqtt_pub(topic_buf, uptime_val, false);
    }

    // Active interface
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/interface", cfg.mqtt_prefix);
    const char *if_name;
    switch (cfg.active_if)
    {
    case NET_IF_LAN:
        if_name = "LAN";
        break;
    case NET_IF_WIFI:
        if_name = "WiFi";
        break;
    default:
        if_name = "NONE";
        break;
    }
    mqtt_pub(topic_buf, if_name, false);

    // IP address
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/ip", cfg.mqtt_prefix);
    if (net_connected)
    {
        mqtt_pub(topic_buf, active_ip.c_str(), false);
    }
    else
    {
        mqtt_pub(topic_buf, "unavailable", false);
    }

    // PSRAM free
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/psram_free", cfg.mqtt_prefix);
    {
        static char psram_val[16];
        snprintf(psram_val, sizeof(psram_val), "%u", psram_free());
        mqtt_pub(topic_buf, psram_val, false);
    }

    // WDT reboots
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/wdt_reboots", cfg.mqtt_prefix);
    {
        static char wdt_val[16];
        snprintf(wdt_val, sizeof(wdt_val), "%u", wdt_get_reboots());
        mqtt_pub(topic_buf, wdt_val, false);
    }

    // WiFi reconnects
    snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/wifi_reconnects", cfg.mqtt_prefix);
    {
        static char wrc_val[16];
        snprintf(wrc_val, sizeof(wrc_val), "%u", wifi_get_reconnects());
        mqtt_pub(topic_buf, wrc_val, false);
    }

    // TCP bridge stats
    if (cfg.tcp_enabled)
    {
        snprintf(topic_buf, sizeof(topic_buf), "%s/bridge/tcp_req_count", cfg.mqtt_prefix);
        static char tcp_val[16];
        snprintf(tcp_val, sizeof(tcp_val), "%u", tcp_get_req_count());
        mqtt_pub(topic_buf, tcp_val, false);
    }

    // Notify watchdog that MQTT is alive
    wdt_notify_publish();
}

// ─── Register value MQTT publish ──────────────────────────────
void mqtt_publish_register_value(RegisterConfig *reg)
{
    if (!mqtt.connected() || !reg)
        return;

    static char topic[128];
    snprintf(topic, sizeof(topic), "%s/reg/%u/%u/state", cfg.mqtt_prefix, reg->slave_addr, reg->addr);

    static char val[24];
    if (reg->scale > 1)
    {
        float fv = (float)reg->last_value / (float)reg->scale;
        // For COP, show 1 decimal; otherwise 2 decimals
        int dec = (reg->ha_class == HAC_COP) ? 1 : 2;
        snprintf(val, sizeof(val), "%.*f", dec, fv);
    }
    else
    {
        snprintf(val, sizeof(val), "%.2f", reg->last_value);
    }

    mqtt_pub(topic, val, false);
    reg->published = true;
}

// ─── Register HA auto-discovery ───────────────────────────────
void mqtt_publish_register_discovery(RegisterConfig *reg)
{
    if (!cfg.ha_discovery || !mqtt.connected() || !reg)
        return;

    JsonDocument doc(PsramAllocator::instance());

    // Unique ID and object ID: hostname_sN_rNNNN
    static char uid[96];
    snprintf(uid, sizeof(uid), "%s_s%u_r%u", cfg.hostname, reg->slave_addr, reg->addr);

    doc["name"] = reg->name;
    doc["unique_id"] = uid;
    doc["object_id"] = uid;

    // State topic
    static char st_topic[128];
    snprintf(st_topic, sizeof(st_topic), "%s/reg/%u/%u/state", cfg.mqtt_prefix, reg->slave_addr, reg->addr);
    doc["state_topic"] = st_topic;

    // Map RegHAClass to HA device_class + unit_of_measurement + icon
    const char *device_class = nullptr;
    const char *unit = reg->unit; // default: use config unit
    const char *icon = nullptr;

    switch (reg->ha_class)
    {
    case HAC_TEMPERATURE:
        device_class = "temperature";
        unit = "°C";
        break;
    case HAC_HUMIDITY:
        device_class = "humidity";
        unit = "%";
        break;
    case HAC_POWER:
        device_class = "power";
        unit = "W";
        break;
    case HAC_ENERGY:
        device_class = "energy";
        unit = "kWh";
        break;
    case HAC_VOLTAGE:
        device_class = "voltage";
        unit = "V";
        break;
    case HAC_CURRENT:
        device_class = "current";
        unit = "A";
        break;
    case HAC_FREQUENCY:
        device_class = "frequency";
        unit = "Hz";
        break;
    case HAC_PRESSURE:
        device_class = "pressure";
        unit = "hPa";
        break;
    case HAC_COP:
        device_class = nullptr;
        unit = "";
        icon = "mdi:heat-pump";
        break;
    case HAC_SENSOR:
    default:
        device_class = nullptr;
        // unit already set from reg->unit
        break;
    }

    if (device_class)
        doc["device_class"] = device_class;
    if (unit && unit[0] != '\0')
        doc["unit_of_measurement"] = unit;
    if (icon)
        doc["icon"] = icon;

    // Availability
    set_availability(doc);

    // ── Writable registers: HA "number" entity with command topic ──
    if (reg->writable)
    {
        // Command topic: MQTT → Modbus write
        static char cmd_topic[128];
        snprintf(cmd_topic, sizeof(cmd_topic), "%s/reg/%u/%u/set", cfg.mqtt_prefix, reg->slave_addr, reg->addr);
        doc["command_topic"] = cmd_topic;

        // Min/max defaults for number entity
        doc["min"] = 0;
        doc["max"] = 65535;
        doc["step"] = (reg->scale > 0) ? (1.0 / reg->scale) : 1;

        // Device block
        static char bridge_id[64];
        snprintf(bridge_id, sizeof(bridge_id), "%s_bridge", cfg.hostname);
        doc["device"]["identifiers"] = bridge_id;
        doc["device"]["name"] = String(cfg.hostname) + " Bridge";
        doc["device"]["mf"] = "Waveshare";
        doc["device"]["mdl"] = "ESP32-S3-ETH V1.0";
        doc["device"]["sw"] = FIRMWARE_VERSION;

        // Discovery topic: homeassistant/number/hostname_sN_rNNNN/config
        static char num_topic[192];
        snprintf(num_topic, sizeof(num_topic), "homeassistant/number/%s/config", uid);
        discovery_publish(num_topic, doc, true);
    }
    else
    {
        // ── Read-only registers: HA "sensor" entity ──
        // Device block — register belongs to the bridge device
        static char bridge_id[64];
        snprintf(bridge_id, sizeof(bridge_id), "%s_bridge", cfg.hostname);
        doc["device"]["identifiers"] = bridge_id;
        doc["device"]["name"] = String(cfg.hostname) + " Bridge";
        doc["device"]["mf"] = "Waveshare";
        doc["device"]["mdl"] = "ESP32-S3-ETH V1.0";
        doc["device"]["sw"] = FIRMWARE_VERSION;

        // Discovery topic: homeassistant/sensor/hostname_sN_rNNNN/config
        static char disc_topic[192];
        snprintf(disc_topic, sizeof(disc_topic), "homeassistant/sensor/%s/config", uid);
        discovery_publish(disc_topic, doc, true);
    }
}