/**
 * ws_handler.cpp — WebSocket real-time status updates
 *
 * Port 81 (separate from web server on port 80).
 * Pushes: relay/DI state changes (immediate), heartbeat (10s), MQTT status
 * PSRAM-backed JsonDocument + psram_malloc serialize buffer for large payloads.
 */

#include "modbus_mqtt_ha_bridge.h"
#include <WebSocketsServer.h>
#include <ArduinoJson.h>

#define WS_PORT 81
#define WS_HEARTBEAT 10000

static WebSocketsServer ws(WS_PORT);
static uint32_t ws_last_heartbeat = 0;
static uint8_t ws_client_count = 0;

// ─── PSRAM-backed JSON serialize + broadcast ─────────────────
// Builds JSON into a PSRAM buffer and broadcasts/writes it.
// Avoids String heap allocation for each WS message.

static void ws_broadcast_doc(JsonDocument &doc)
{
    size_t len = measureJson(doc) + 1;
    char *buf = reinterpret_cast<char *>(psram_malloc(len));
    if (!buf)
    {
        // Fallback to stack-allocated String (small messages only)
        String fallback;
        serializeJson(doc, fallback);
        ws.broadcastTXT(fallback);
        return;
    }
    serializeJson(doc, buf, len);
    ws.broadcastTXT(buf);
    free(buf);
}

static void ws_send_doc(uint8_t num, JsonDocument &doc)
{
    size_t len = measureJson(doc) + 1;
    char *buf = reinterpret_cast<char *>(psram_malloc(len));
    if (!buf)
    {
        String fallback;
        serializeJson(doc, fallback);
        ws.sendTXT(num, fallback);
        return;
    }
    serializeJson(doc, buf, len);
    ws.sendTXT(num, buf);
    free(buf);
}

// ─── JSON builders ──────────────────────────────────────────────

static void ws_relay_json(Slave_Module *mod, uint8_t r, JsonDocument &doc)
{
    doc["type"] = "relay";
    doc["addr"] = mod->slave_addr;
    doc["idx"] = r;
    doc["name"] = config_get_relay_name(mod->slave_addr, r);
    doc["state"] = mod->relays[r].state ? "ON" : "OFF";
}

static void ws_di_json(Slave_Module *mod, uint8_t d, bool state, JsonDocument &doc)
{
    doc["type"] = "di";
    doc["addr"] = mod->slave_addr;
    doc["idx"] = d;
    doc["name"] = config_get_di_name(mod->slave_addr, d);
    doc["state"] = state ? "ON" : "OFF";
}

static void ws_heartbeat_json(JsonDocument &doc)
{
    doc["type"] = "heartbeat";
    doc["uptime"] = millis() / 1000;
    doc["heap_kb"] = ESP.getFreeHeap() / 1024;

    // PSRAM metrics
    doc["psram_free_kb"] = psram_free() / 1024;
    doc["psram_total_kb"] = psram_total() / 1024;

    doc["mqtt"] = mqtt_is_connected();
    doc["interface"] = cfg.active_if == NET_IF_LAN ? "LAN" : cfg.active_if == NET_IF_WIFI ? "WiFi" : "NONE";
    doc["lan"] = cfg.lan_enabled && eth_is_connected();
    doc["wifi_rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;

    JsonArray mods = doc["modules"].to<JsonArray>();
    for (uint16_t i = 0; i < module_count; i++)
    {
        JsonObject m = mods.add<JsonObject>();
        m["addr"] = modules[i].slave_addr;
        m["online"] = modules[i].online;
        m["model"] = modules[i].model.model_name;
        JsonArray relays = m["relays"].to<JsonArray>();
        for (uint8_t r = 0; r < modules[i].model.RELAY_COUNT; r++)
            relays.add(modules[i].relays[r].state);
        JsonArray dis = m["dis"].to<JsonArray>();
        for (uint8_t d = 0; d < modules[i].model.DI_COUNT; d++)
            dis.add(modules[i].inputs[d].current);
    }
}

static void ws_mqtt_json(bool connected, JsonDocument &doc)
{
    doc["type"] = "mqtt";
    doc["connected"] = connected;
}

// ─── Event handler ──────────────────────────────────────────────

static void ws_event(uint8_t num, WStype_t type, uint8_t *payload, size_t length)
{
    switch (type)
    {
    case WStype_DISCONNECTED:
        if (ws_client_count > 0)
            ws_client_count--;
        LOG_D("[WS] Client %u disconnected (%u total)\n", num, ws_client_count);
        break;
    case WStype_CONNECTED:
    {
        ws_client_count++;
        IPAddress ip = ws.remoteIP(num);
        LOG_D("[WS] Client %u from %d.%d.%d.%d (%u total)\n", num, ip[0], ip[1], ip[2], ip[3], ws_client_count);
        JsonDocument doc(PsramAllocator::instance());
        ws_heartbeat_json(doc);
        ws_send_doc(num, doc);
        break;
    }
    case WStype_TEXT:
    {
        // Use stack comparison, no String allocation for simple command check
        if (length >= 6 && memcmp(payload, "status", 6) == 0)
        {
            JsonDocument doc(PsramAllocator::instance());
            ws_heartbeat_json(doc);
            ws_send_doc(num, doc);
        }
        else if (length > 0)
        {
            // Check for {"cmd":"status"} JSON command
            if (memchr(payload, '"', length))
            {
                String msg = String(reinterpret_cast<const char *>(payload)).substring(0, length);
                if (msg.indexOf("\"cmd\":\"status\"") >= 0)
                {
                    JsonDocument doc(PsramAllocator::instance());
                    ws_heartbeat_json(doc);
                    ws_send_doc(num, doc);
                }
            }
        }
        break;
    }
    default:
        break;
    }
}

// ─── Public API ─────────────────────────────────────────────────

void ws_init()
{
    ws.begin();
    ws.onEvent(ws_event);
    ws.enableHeartbeat(30000, 10000, 3);
    ws_last_heartbeat = millis();
    LOG_I("[WS] WebSocket server started on port %d\n", WS_PORT);
}

void ws_loop()
{
    ws.loop();
    if (ws_client_count > 0 && millis() - ws_last_heartbeat >= WS_HEARTBEAT)
    {
        JsonDocument doc(PsramAllocator::instance());
        ws_heartbeat_json(doc);
        ws_broadcast_doc(doc);
        ws_last_heartbeat = millis();
    }
}

void ws_notify_relay(Slave_Module *mod, uint8_t relay_idx)
{
    if (ws_client_count == 0)
        return;
    JsonDocument doc;
    ws_relay_json(mod, relay_idx, doc);
    ws_broadcast_doc(doc);
}

void ws_notify_di(Slave_Module *mod, uint8_t di_idx, bool state)
{
    if (ws_client_count == 0)
        return;
    JsonDocument doc;
    ws_di_json(mod, di_idx, state, doc);
    ws_broadcast_doc(doc);
}

void ws_notify_mqtt(bool connected)
{
    if (ws_client_count == 0)
        return;
    JsonDocument doc;
    ws_mqtt_json(connected, doc);
    ws_broadcast_doc(doc);
}

uint8_t ws_client_count_get()
{
    return ws_client_count;
}