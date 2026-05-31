/**
 * mqtt_handler.cpp — MQTT Publishing & HA Discovery v2.0
 * Uses global cfg struct instead of NVRAM reads.
 * Supports hostname-based object_id, custom friendly names,
 * birth message, stats publishing, and DI click events.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "modbus_mqtt_ha_bridge.h"

#ifdef USE_W5500
#include <Ethernet.h>
extern EthernetClient eth_tcp_client; // defined in eth_handler.cpp
#endif

// Client instances for different transport options
static WiFiClientSecure mqtt_wifi_secure;
static WiFiClient mqtt_wifi_plain;
static PubSubClient mqtt;
static void mqtt_callback(char *topic, byte *payload, unsigned int length);

// Track which transport MQTT is using (for logging + reconnect switching)
static bool mqtt_on_lan = false;
bool mqtt_is_on_lan() { return mqtt_on_lan; }

static String topic_base(uint8_t slave, const String &suffix = "")
{
    String t = String(cfg.mqtt_prefix) + "/ha_v2/";
    if (slave > 0)
        t += String(slave) + "/";
    if (suffix.length() > 0)
        t += suffix;
    return t;
}

static String discovery_topic(const char *domain, const String &obj_id)
{
    return String("homeassistant/") + domain + "/" + String(cfg.hostname) + "_" + obj_id + "/config";
}

static String unique_id(uint8_t slave, const char *type, uint8_t idx)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%s_s%d_%s_%d", cfg.hostname, slave, type, idx);
    return String(buf);
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

    LOG_I("[MQTT] Connecting to %s:%d (TLS=%s)...\n", cfg.mqtt_host, cfg.mqtt_port, cfg.mqtt_tls ? "ON" : "OFF");

    if (mqtt.connect((String(cfg.hostname) + "_bridge").c_str(),
                     cfg.mqtt_user[0] ? cfg.mqtt_user : NULL,
                     cfg.mqtt_pass[0] ? cfg.mqtt_pass : NULL,
                     topic_base(0, "status").c_str(),
                     MQTT_QOS,
                     true,
                     "offline"))
    {
        LOG_ILN("[MQTT] Connected");
        // Birth message — ensures HA sees device online even if LWT missed
        mqtt.publish(topic_base(0, "status").c_str(), "online", true);
        // Ensure HA MQTT integration processes discovery topics
        // (HA won't process retained config topics without this)
        if (cfg.ha_discovery)
            mqtt.publish("homeassistant/status", "online", true);
        // Publish bridge system device discovery
        mqtt_publish_bridge_discovery();
    }
    else
    {
        LOG_E("[MQTT] Failed, state=%d\n", mqtt.state());
    }
}

// ─── MQTT Loop with Reconnect (exponential backoff) ───────────
void mqtt_loop()
{
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

            if (mqtt.connect((String(cfg.hostname) + "_bridge").c_str(),
                             cfg.mqtt_user[0] ? cfg.mqtt_user : NULL,
                             cfg.mqtt_pass[0] ? cfg.mqtt_pass : NULL,
                             topic_base(0, "status").c_str(),
                             MQTT_QOS,
                             true,
                             "offline"))
            {
                LOG_ILN("[MQTT] Reconnected");
                reconnect_attempts = 0; // Reset backoff on success
                // Birth message on reconnect
                mqtt.publish(topic_base(0, "status").c_str(), "online", true);
                // Ensure HA MQTT integration processes discovery topics
                if (cfg.ha_discovery)
                    mqtt.publish("homeassistant/status", "online", true);
                // Publish bridge system device discovery
                mqtt_publish_bridge_discovery();
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
            }
            last_reconnect = millis();
        }
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
    char msg[32] = {0};
    if (length >= sizeof(msg))
        length = sizeof(msg) - 1;
    memcpy(msg, payload, length);
    msg[length] = '\0';

    LOG_D("[MQTT-CB] topic='%s' msg='%s'\n", topic, msg);

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
        String cmd_topic = topic_base(mod->slave_addr, "relay/" + String(r) + "/set");
        bool ok = mqtt.subscribe(cmd_topic.c_str(), MQTT_QOS);
        LOG_D("[MQTT] Subscribe %s → %s\n", cmd_topic.c_str(), ok ? "OK" : "FAIL");
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
        String uid = unique_id(mod->slave_addr, "relay", r);
        mqtt.publish(discovery_topic("switch", uid).c_str(), "", true);
    }

    // DI binary sensors
    for (uint8_t d = 0; d < mod->model.DI_COUNT; d++)
    {
        String uid = unique_id(mod->slave_addr, "di", d);
        mqtt.publish(discovery_topic("binary_sensor", uid).c_str(), "", true);

        // Click sensors
        String click_uid = unique_id(mod->slave_addr, "click", d);
        mqtt.publish(discovery_topic("sensor", click_uid).c_str(), "", true);
    }

    // Module status sensor
    String status_uid = unique_id(mod->slave_addr, "status", 0);
    String uid_avail = status_uid + "_avail";
    mqtt.publish(discovery_topic("binary_sensor", uid_avail).c_str(), "", true);
    mqtt.publish(discovery_topic("sensor", status_uid).c_str(), "", true);

    // Slave address sensor
    String addr_uid = unique_id(mod->slave_addr, "addr", 0);
    mqtt.publish(discovery_topic("sensor", addr_uid).c_str(), "", true);

    LOG_I("[MQTT] Cleaned up discovery for module S%d\n", mod->slave_addr);
}

// ─── Set common device block for HA Discovery ────────────────────
// Builds the "device" JSON object with identifiers, name, manufacturer, model, sw, area.
// Reused across all entities in mqtt_publish_discovery() to avoid repeated string allocs.
static void set_device_block(JsonDocument &doc, Slave_Module *mod, const String &area)
{
    doc["device"]["identifiers"] = String(cfg.hostname) + "_ha_v2_" + String(mod->slave_addr);
    doc["device"]["name"] = get_device_name(mod);
    doc["device"]["mf"] = get_manufacturer(mod);
    doc["device"]["mdl"] = mod->model.model_name;
    doc["device"]["sw"] = FIRMWARE_VERSION;
    if (mod->model.serial_number > 0)
        doc["device"]["sn"] = String(mod->model.serial_number);
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
    size_t json_size = measureJson(doc) + 1;
    // Allocate buffer in PSRAM if available, otherwise heap
    char *buf = reinterpret_cast<char *>(psram_malloc(json_size));
    if (!buf)
    {
        LOG_E("[MQTT] Discovery publish: OOM (%u bytes)\n", json_size);
        return false;
    }
    serializeJson(doc, buf, json_size);
    bool ok = mqtt.publish(topic, buf, retain);
    free(buf);
    return ok;
}

void mqtt_publish_discovery(Slave_Module *mod)
{
    if (!cfg.ha_discovery)
    {
        LOG_I("[MQTT] Discovery disabled, skipping module %d\n", mod->slave_addr);
        return;
    }

    JsonDocument doc(PsramAllocator::instance());

    // Common area for all entities of this module (computed once)
    String area = config_get_module_area(mod->slave_addr);

    // Relay Switches
    for (uint8_t r = 0; r < mod->model.RELAY_COUNT; r++)
    {
        doc.clear();
        String uid = unique_id(mod->slave_addr, "relay", r);
        String obj_id = String(cfg.hostname) + "_s" + String(mod->slave_addr) + "_r" + String(r);

        // Note: do NOT set doc["platform"] = "mqtt" — HA auto-detects platform from topic path
        doc["name"] = get_relay_entity_name(mod, r);
        doc["unique_id"] = uid;
        doc["object_id"] = obj_id;
        doc["state_topic"] = topic_base(mod->slave_addr, "relay/" + String(r) + "/state");
        doc["command_topic"] = topic_base(mod->slave_addr, "relay/" + String(r) + "/set");
        doc["payload_on"] = "ON";
        doc["payload_off"] = "OFF";
        doc["state_on"] = "ON";
        doc["state_off"] = "OFF";
        doc["qos"] = MQTT_QOS;
        doc["retain"] = MQTT_RETAIN_STATE;
        set_availability(doc);
        set_device_block(doc, mod, area);

        discovery_publish(discovery_topic("switch", uid).c_str(), doc);
    }

    // DI Binary Sensors (state ON/OFF)
    for (uint8_t d = 0; d < mod->model.DI_COUNT; d++)
    {
        doc.clear();
        String uid = unique_id(mod->slave_addr, "di", d);
        String obj_id = String(cfg.hostname) + "_s" + String(mod->slave_addr) + "_di" + String(d);

        // Note: do NOT set doc["platform"] = "mqtt" — HA auto-detects platform from topic path
        doc["name"] = get_di_entity_name(mod, d);
        doc["unique_id"] = uid;
        doc["object_id"] = obj_id;
        doc["state_topic"] = topic_base(mod->slave_addr, "di/" + String(d) + "/state");
        doc["payload_on"] = "ON";
        doc["payload_off"] = "OFF";
        doc["qos"] = MQTT_QOS;
        doc["device_class"] = "opening";
        doc["entity_category"] = "diagnostic";
        doc["icon"] = "mdi:light-switch";
        set_availability(doc);
        set_device_block(doc, mod, area);

        discovery_publish(discovery_topic("binary_sensor", uid).c_str(), doc);
    }

    // DI Click Event Sensors (for automation triggers)
    for (uint8_t d = 0; d < mod->model.DI_COUNT; d++)
    {
        doc.clear();
        String click_uid = unique_id(mod->slave_addr, "di_click", d);

        String di_custom = config_get_di_name(mod->slave_addr, d);
        String click_name = (di_custom.length() > 0) ? di_custom + " Kattintás"
                                                     : get_entity_name(mod, "DI " + String(d + 1) + " Kattintás");

        // Note: do NOT set doc["platform"] = "mqtt" — HA auto-detects platform from topic path
        doc["name"] = click_name;
        doc["unique_id"] = click_uid;
        doc["object_id"] = String(cfg.hostname) + "_s" + String(mod->slave_addr) + "_di" + String(d) + "_click";
        doc["state_topic"] = topic_base(mod->slave_addr, "di/" + String(d) + "/click");
        doc["icon"] = "mdi:gesture-tap";
        doc["entity_category"] = "diagnostic";
        set_availability(doc);
        set_device_block(doc, mod, area);

        discovery_publish(discovery_topic("sensor", click_uid).c_str(), doc);
    }

    // Module Status Binary Sensor
    doc.clear();
    String uid_avail = unique_id(mod->slave_addr, "avail", 0);
    // Note: do NOT set doc["platform"] = "mqtt" — HA auto-detects platform from topic path
    doc["name"] = get_entity_name(mod, "Státusz");
    doc["unique_id"] = uid_avail;
    doc["entity_category"] = "diagnostic";
    doc["state_topic"] = topic_base(mod->slave_addr, "status");
    doc["payload_on"] = "online";
    doc["payload_off"] = "offline";
    set_device_block(doc, mod, area);

    discovery_publish(discovery_topic("binary_sensor", uid_avail).c_str(), doc);

    // Slave Address Sensor
    doc.clear();
    String uid_addr = unique_id(mod->slave_addr, "addr", 0);
    // Note: do NOT set doc["platform"] = "mqtt" — HA auto-detects platform from topic path
    doc["name"] = get_entity_name(mod, "Slave Cím");
    doc["unique_id"] = uid_addr;
    doc["object_id"] = String(cfg.hostname) + "_s" + String(mod->slave_addr) + "_addr";
    doc["state_topic"] = topic_base(mod->slave_addr, "address");
    doc["icon"] = "mdi:identifier";
    doc["entity_category"] = "diagnostic";
    set_availability(doc);
    set_device_block(doc, mod, area);

    discovery_publish(discovery_topic("sensor", uid_addr).c_str(), doc);

    // Publish slave address value
    mqtt.publish(topic_base(mod->slave_addr, "address").c_str(), String(mod->slave_addr).c_str(), true);

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
        mqtt.publish(topic_base(mod->slave_addr, "name").c_str(), mqtt_name.c_str(), true);
    }
    if (mod_area.length() > 0)
    {
        mqtt.publish(topic_base(mod->slave_addr, "area").c_str(), mod_area.c_str(), true);
    }

    // Publish all relay/DI names as retained sub-topics
    for (uint8_t r = 0; r < mod->model.RELAY_COUNT; r++)
    {
        String rname = config_get_relay_name(mod->slave_addr, r);
        if (rname.length() > 0)
        {
            mqtt.publish(topic_base(mod->slave_addr, "relay/" + String(r) + "/name").c_str(), rname.c_str(), true);
        }
    }
    for (uint8_t d = 0; d < mod->model.DI_COUNT; d++)
    {
        String dname = config_get_di_name(mod->slave_addr, d);
        if (dname.length() > 0)
        {
            mqtt.publish(topic_base(mod->slave_addr, "di/" + String(d) + "/name").c_str(), dname.c_str(), true);
        }
    }
}

// ─── Publish Relay State ────────────────────────────────────────
void mqtt_publish_relay_state(Slave_Module *mod, uint8_t relay_idx)
{
    String state_topic = topic_base(mod->slave_addr, "relay/" + String(relay_idx) + "/state");
    mqtt.publish(state_topic.c_str(), mod->relays[relay_idx].state ? "ON" : "OFF", MQTT_RETAIN_STATE);

    // Publish relay name as retained sub-topic (for MQTT Explorer visibility)
    String name = config_get_relay_name(mod->slave_addr, relay_idx);
    if (name.length() > 0)
    {
        String name_topic = topic_base(mod->slave_addr, "relay/" + String(relay_idx) + "/name");
        mqtt.publish(name_topic.c_str(), name.c_str(), true);
    }

    // Notify WebSocket clients
    ws_notify_relay(mod, relay_idx);
}

// ─── Publish DI State ──────────────────────────────────────────
void mqtt_publish_di_state(Slave_Module *mod, uint8_t di_idx, bool state)
{
    String state_topic = topic_base(mod->slave_addr, "di/" + String(di_idx) + "/state");
    mqtt.publish(state_topic.c_str(), state ? "ON" : "OFF", MQTT_RETAIN_STATE);

    // Publish DI name as retained sub-topic (for MQTT Explorer visibility)
    String name = config_get_di_name(mod->slave_addr, di_idx);
    if (name.length() > 0)
    {
        String name_topic = topic_base(mod->slave_addr, "di/" + String(di_idx) + "/name");
        mqtt.publish(name_topic.c_str(), name.c_str(), true);
    }

    // Notify WebSocket clients
    ws_notify_di(mod, di_idx, state);
}

// ─── Publish Click Event (from callback) ────────────────────────
void mqtt_publish_click_event(Slave_Module *mod, uint8_t di_idx, ClickType ct)
{
    String click_topic = topic_base(mod->slave_addr, "di/" + String(di_idx) + "/click");
    const char *click_str = click_type_str(ct);
    mqtt.publish(click_topic.c_str(), click_str, false);

    LOG_D("[MQTT] Click: S%d DI%d: %s\n", mod->slave_addr, di_idx + 1, click_str);
}

// ─── Publish Module Online Status ──────────────────────────────
void mqtt_publish_module_online(Slave_Module *mod, bool online)
{
    mqtt.publish(topic_base(mod->slave_addr, "status").c_str(), online ? "online" : "offline", true);
}

// ─── Publish Modbus Stats ──────────────────────────────────────
void mqtt_publish_stats()
{
    JsonDocument doc;
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

    String payload;
    serializeJson(doc, payload);

    String stats_topic = String(cfg.mqtt_prefix) + "/ha_v2/stats";
    mqtt.publish(stats_topic.c_str(), payload.c_str(), false);
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

    JsonDocument doc;
    String bridge_id = String(cfg.hostname) + "_bridge";
    String bridge_name = String(cfg.hostname) + " Bridge";
    String avail_topic = topic_base(0, "status");

    // Helper: common device block
    auto set_device = [&]() {
        doc["device"]["identifiers"] = bridge_id;
        doc["device"]["name"] = bridge_name;
        doc["device"]["mf"] = "Waveshare";
        doc["device"]["mdl"] = "ESP32-S3-ETH V1.0";
        doc["device"]["sw"] = "Modbus-MQTT Bridge v2.0";
    };

    // 1. WiFi Jel (RSSI)
    {
        doc.clear();
        String uid = bridge_id + "_rssi";
        // Note: do NOT set doc["platform"] = "mqtt" — HA auto-detects platform from topic path
        doc["name"] = "WiFi Jel";
        doc["unique_id"] = uid;
        doc["object_id"] = uid;
        doc["state_topic"] = String(cfg.mqtt_prefix) + "/bridge/rssi";
        doc["device_class"] = "signal_strength";
        doc["unit_of_measurement"] = "dBm";
        doc["icon"] = "mdi:wifi";
        doc["availability_topic"] = avail_topic;
        doc["payload_available"] = "online";
        doc["payload_not_available"] = "offline";
        set_device();
        String payload;
        serializeJson(doc, payload);
        mqtt.publish(discovery_topic("sensor", uid).c_str(), payload.c_str(), true);
    }

    // 2. Szabad Memória (Heap)
    {
        doc.clear();
        String uid = bridge_id + "_heap";
        // Note: do NOT set doc["platform"] = "mqtt" — HA auto-detects platform from topic path
        doc["name"] = "Szabad Memória";
        doc["unique_id"] = uid;
        doc["object_id"] = uid;
        doc["state_topic"] = String(cfg.mqtt_prefix) + "/bridge/heap";
        doc["unit_of_measurement"] = "B";
        doc["icon"] = "mdi:memory";
        doc["availability_topic"] = avail_topic;
        doc["payload_available"] = "online";
        doc["payload_not_available"] = "offline";
        set_device();
        String payload;
        serializeJson(doc, payload);
        mqtt.publish(discovery_topic("sensor", uid).c_str(), payload.c_str(), true);
    }

    // 3. Uptime
    {
        doc.clear();
        String uid = bridge_id + "_uptime";
        // Note: do NOT set doc["platform"] = "mqtt" — HA auto-detects platform from topic path
        doc["name"] = "Uptime";
        doc["unique_id"] = uid;
        doc["object_id"] = uid;
        doc["state_topic"] = String(cfg.mqtt_prefix) + "/bridge/uptime";
        doc["unit_of_measurement"] = "s";
        doc["icon"] = "mdi:clock";
        doc["availability_topic"] = avail_topic;
        doc["payload_available"] = "online";
        doc["payload_not_available"] = "offline";
        set_device();
        String payload;
        serializeJson(doc, payload);
        mqtt.publish(discovery_topic("sensor", uid).c_str(), payload.c_str(), true);
    }

    // 4. Hálózat (Interface)
    {
        doc.clear();
        String uid = bridge_id + "_if";
        // Note: do NOT set doc["platform"] = "mqtt" — HA auto-detects platform from topic path
        doc["name"] = "Hálózat";
        doc["unique_id"] = uid;
        doc["object_id"] = uid;
        doc["state_topic"] = String(cfg.mqtt_prefix) + "/bridge/interface";
        doc["icon"] = "mdi:lan";
        doc["availability_topic"] = avail_topic;
        doc["payload_available"] = "online";
        doc["payload_not_available"] = "offline";
        set_device();
        String payload;
        serializeJson(doc, payload);
        mqtt.publish(discovery_topic("sensor", uid).c_str(), payload.c_str(), true);
    }

    // 5. IP Cím
    {
        doc.clear();
        String uid = bridge_id + "_ip";
        // Note: do NOT set doc["platform"] = "mqtt" — HA auto-detects platform from topic path
        doc["name"] = "IP Cím";
        doc["unique_id"] = uid;
        doc["object_id"] = uid;
        doc["state_topic"] = String(cfg.mqtt_prefix) + "/bridge/ip";
        doc["icon"] = "mdi:ip-network";
        doc["availability_topic"] = avail_topic;
        doc["payload_available"] = "online";
        doc["payload_not_available"] = "offline";
        set_device();
        String payload;
        serializeJson(doc, payload);
        mqtt.publish(discovery_topic("sensor", uid).c_str(), payload.c_str(), true);
    }

    LOG_ILN("[MQTT] Bridge system discovery published (5 sensors)");
}

// ─── Bridge System State Publishing ────────────────────────────
void mqtt_publish_bridge_state()
{
    if (!mqtt.connected())
        return;

    String prefix = String(cfg.mqtt_prefix) + "/bridge/";

    // RSSI — only valid when WiFi connected
    if (WiFi.status() == WL_CONNECTED)
    {
        mqtt.publish((prefix + "rssi").c_str(), String(WiFi.RSSI()).c_str(), false);
    }
    else
    {
        mqtt.publish((prefix + "rssi").c_str(), "unavailable", false);
    }

    // Free heap
    mqtt.publish((prefix + "heap").c_str(), String(ESP.getFreeHeap()).c_str(), false);

    // Firmware version
    mqtt.publish((prefix + "fw_version").c_str(), FIRMWARE_VERSION, true);

    // Uptime in seconds
    mqtt.publish((prefix + "uptime").c_str(), String(millis() / 1000).c_str(), false);

    // Active interface
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
    mqtt.publish((prefix + "interface").c_str(), if_name, false);

    // IP address
    if (net_connected)
    {
        mqtt.publish((prefix + "ip").c_str(), active_ip.c_str(), false);
    }
    else
    {
        mqtt.publish((prefix + "ip").c_str(), "unavailable", false);
    }

    // Notify watchdog that MQTT is alive
    wdt_notify_publish();
}