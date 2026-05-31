/**
 * ws_handler.cpp — WebSocket real-time status updates
 *
 * Port 81 (separate from web server on port 80).
 * Pushes: relay/DI state changes (immediate), heartbeat (10s), MQTT status
 */

#include "modbus_mqtt_ha_bridge.h"
#include <WebSocketsServer.h>
#include <ArduinoJson.h>

#define WS_PORT 81
#define WS_HEARTBEAT 10000

static WebSocketsServer ws(WS_PORT);
static uint32_t ws_last_heartbeat = 0;
static uint8_t ws_client_count = 0;

// Helper: broadcast a named String (WebSockets lib requires non-const ref)
static void ws_broadcast(String &msg)
{
    ws.broadcastTXT(msg);
}
static void ws_send(uint8_t num, String &msg)
{
    ws.sendTXT(num, msg);
}

// ─── JSON builders ──────────────────────────────────────────────

static String ws_relay_json(Slave_Module *mod, uint8_t r)
{
    JsonDocument doc;
    doc["type"] = "relay";
    doc["addr"] = mod->slave_addr;
    doc["idx"] = r;
    doc["name"] = config_get_relay_name(mod->slave_addr, r);
    doc["state"] = mod->relays[r].state ? "ON" : "OFF";
    String out;
    serializeJson(doc, out);
    return out;
}

static String ws_di_json(Slave_Module *mod, uint8_t d, bool state)
{
    JsonDocument doc;
    doc["type"] = "di";
    doc["addr"] = mod->slave_addr;
    doc["idx"] = d;
    doc["name"] = config_get_di_name(mod->slave_addr, d);
    doc["state"] = state ? "ON" : "OFF";
    String out;
    serializeJson(doc, out);
    return out;
}

static String ws_heartbeat_json()
{
    JsonDocument doc;
    doc["type"] = "heartbeat";
    doc["uptime"] = millis() / 1000;
    doc["heap_kb"] = ESP.getFreeHeap() / 1024;
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
        JsonArray relays = m["relays"].to<JsonArray>();
        for (uint8_t r = 0; r < modules[i].model.RELAY_COUNT; r++)
            relays.add(modules[i].relays[r].state);
    }
    String out;
    serializeJson(doc, out);
    return out;
}

static String ws_mqtt_json(bool connected)
{
    JsonDocument doc;
    doc["type"] = "mqtt";
    doc["connected"] = connected;
    String out;
    serializeJson(doc, out);
    return out;
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
        String hb = ws_heartbeat_json();
        ws_send(num, hb);
        break;
    }
    case WStype_TEXT:
    {
        String msg = String(reinterpret_cast<const char *>(payload)).substring(0, length);
        if (msg == "status" || msg.indexOf("\"cmd\":\"status\"") >= 0)
        {
            String hb = ws_heartbeat_json();
            ws_send(num, hb);
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
        String hb = ws_heartbeat_json();
        ws_broadcast(hb);
        ws_last_heartbeat = millis();
    }
}

void ws_notify_relay(Slave_Module *mod, uint8_t relay_idx)
{
    if (ws_client_count == 0)
        return;
    String msg = ws_relay_json(mod, relay_idx);
    ws_broadcast(msg);
}

void ws_notify_di(Slave_Module *mod, uint8_t di_idx, bool state)
{
    if (ws_client_count == 0)
        return;
    String msg = ws_di_json(mod, di_idx, state);
    ws_broadcast(msg);
}

void ws_notify_mqtt(bool connected)
{
    if (ws_client_count == 0)
        return;
    String msg = ws_mqtt_json(connected);
    ws_broadcast(msg);
}

uint8_t ws_client_count_get()
{
    return ws_client_count;
}