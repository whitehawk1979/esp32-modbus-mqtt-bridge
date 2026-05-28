/**
 * web_server.cpp — Persistent Web Status & Configuration Server
 * 
 * Always running on port 80. Accessible at device IP (LAN or WiFi).
 * Two pages:
 *   /       — Status dashboard (network, modules, MQTT, uptime, Modbus stats)
 *   /config — Full configuration (LAN, WiFi, MQTT, HA, Modbus, TCP)
 *   /save   — POST handler for config save
 *   /restart — Restart device
 * 
 * Initial AP-mode portal (BOOT button) still works for first-time setup.
 * This server runs AFTER the device is connected to a network.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#ifdef USE_W5500
#include <SPI.h>
#include <Ethernet.h>
// eth_mac is defined static in eth_handler.cpp — use same MAC here
static byte lan_mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
#endif
#include <Preferences.h>
#include <ArduinoJson.h>
#include "modbus_mqtt_ha_bridge.h"

// ─── Forward declarations from ota_handler.cpp ──────────────
void handleOtaPage();
void handleOtaUpload();

WebServer web(80);

// ─── Web Authentication Helper ─────────────────────────────────
// Check if web authentication is required and validate
bool web_auth_ok() {
    if (!cfg.web_auth || strlen(cfg.web_pass) == 0) return true;  // Auth disabled
    if (!web.authenticate("admin", cfg.web_pass)) {
        web.requestAuthentication(DIGEST_AUTH, "ModbusMQTT");
        return false;
    }
    return true;
}

// ─── External declarations ─────────────────────────────────────
extern Slave_Module *modules;
extern uint16_t module_count;
extern bool scanning_done;
extern bool scan_active;
extern bool mqtt_is_connected();

// ─── Relay Control Handler ───────────────────────────────────
// GET /relay?addr=1&relay=0&state=1  (state: 1=ON, 0=OFF)
static void handleRelay() {
    if (!web_auth_ok()) return;
    if (!web.hasArg("addr") || !web.hasArg("relay") || !web.hasArg("state")) {
        web.send(400, "application/json", "{\"ok\":false,\"error\":\"missing params\"}");
        return;
    }
    uint8_t addr  = (uint8_t)web.arg("addr").toInt();
    uint8_t relay = (uint8_t)web.arg("relay").toInt();
    bool    state = web.arg("state").toInt() != 0;

    // Find module
    Slave_Module *mod = nullptr;
    for (uint16_t i = 0; i < module_count; i++) {
        if (modules[i].slave_addr == addr) { mod = &modules[i]; break; }
    }
    if (!mod) {
        web.send(404, "application/json", "{\"ok\":false,\"error\":\"module not found\"}");
        return;
    }

    bool ok;
    if (mod->is_virtual) {
        // Virtual module: just flip internal state + publish
        mod->relays[relay].state = state;
        mod->relays[relay].published = true;
        mqtt_publish_relay_state(mod, relay);
        LOG_I("[WEB] Virtual S%d R%d → %s\n", addr, relay + 1, state ? "ON" : "OFF");
        ok = true;
    } else if (!mod->online) {
        web.send(409, "application/json", "{\"ok\":false,\"error\":\"module offline\"}");
        return;
    } else {
        // Physical module: FC05 Modbus write + read-back + MQTT publish
        ok = modbus_write_coil(addr, relay, state);
        if (ok) {
            mqtt_publish_relay_state(mod, relay);
            LOG_I("[WEB] S%d R%d → %s (Modbus OK)\n", addr, relay + 1, state ? "ON" : "OFF");
        } else {
            LOG_E("[WEB] S%d R%d write FAILED\n", addr, relay + 1);
        }
    }

    if (ok) {
        String resp = "{\"ok\":true,\"addr\":" + String(addr) + ",\"relay\":" + String(relay) + ",\"state\":" + String(state ? 1 : 0) + "}";
        web.send(200, "application/json", resp);
    } else {
        web.send(500, "application/json", "{\"ok\":false,\"error\":\"modbus write failed\"}");
    }
}

// ─── Helpers ───────────────────────────────────────────────────
static String uptimeStr() {
    uint32_t s = millis() / 1000;
    uint16_t d = s / 86400; s %= 86400;
    uint16_t h = s / 3600; s %= 3600;
    uint16_t m = s / 60; s %= 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%dd %02dh %02dm %02ds", d, h, m, (uint16_t)s);
    return String(buf);
}

static String ifName(NetInterface n) {
    switch (n) {
        case NET_IF_LAN:  return "LAN &#9889;";
        case NET_IF_WIFI: return "WiFi &#128225;";
        default:          return "Nincs &#10060;";
    }
}

static String ifColor(NetInterface n) {
    switch (n) {
        case NET_IF_LAN:  return "#3fb950";  // green
        case NET_IF_WIFI: return "#f0883e";  // orange
        default:          return "#f85149";  // red
    }
}

static String profileName(uint8_t p) {
    switch (p) {
        case MB_PROFILE_KC868_HA: return "KC868-HA V2";
        case MB_PROFILE_GENERIC:  return "Generic";
        case MB_PROFILE_CUSTOM:   return "Egyedi";
        default:                   return "?";
    }
}

static String urlEncode(const String&);  // forward

// ─── Room list helpers ─────────────────────────────────────────
static const char* DEFAULT_ROOMS[] = {
    "Nappali","Hálószoba","Konyha","Fürdőszoba","Előszoba",
    "Gyerekszoba","Dolgozószoba","Mosókonyha","Garázs","Pince",
    "Terasz","Folyosó","Lépcsőház","Étterem","Raktár"
};
#define DEFAULT_ROOM_COUNT 15

// Build room <select> HTML for a module. cur_area = current saved value.
static String roomSelectHtml(const String& field_name, const String& cur_area) {
    String h = "<select name=\"" + field_name + "\" onchange=\"roomChanged(this)\">";
    h += "<option value=\"\"" + String(cur_area.length()==0 ? " selected" : "") + ">— nincs megadva —</option>";
    
    // Default rooms
    for (auto rm : DEFAULT_ROOMS) {
        h += "<option value=\"" + String(rm) + "\"" + String(cur_area == rm ? " selected" : "") + ">" + String(rm) + "</option>";
    }
    
    // Custom rooms from NVRAM
    Preferences nv;
    nv.begin(NV_NAMESPACE, true);
    String customRooms = nv.getString(NV_KEY_ROOMS, "");
    nv.end();
    
    if (customRooms.length() > 0) {
        int start = 0, end;
        do {
            end = customRooms.indexOf('\n', start);
            String rm = (end >= 0) ? customRooms.substring(start, end) : customRooms.substring(start);
            rm.trim();
            if (rm.length() > 0) {
                h += "<option value=\"" + rm + "\"" + String(cur_area == rm ? " selected" : "") + ">" + rm + "</option>";
            }
            start = end + 1;
        } while (end >= 0);
    }
    
    // "Egyéb" — selected if cur_area is non-empty but not in any list
    bool is_default = false;
    for (auto dr : DEFAULT_ROOMS) { if (cur_area == dr) { is_default = true; break; } }
    bool is_custom = false;
    if (!is_default && customRooms.length() > 0) {
        int s = 0, e;
        do {
            e = customRooms.indexOf('\n', s);
            String cr = (e >= 0) ? customRooms.substring(s, e) : customRooms.substring(s);
            cr.trim();
            if (cr == cur_area) { is_custom = true; break; }
            s = e + 1;
        } while (e >= 0);
    }
    bool show_other_selected = (cur_area.length() > 0 && !is_default && !is_custom);
    h += "<option value=\"_other\"" + String(show_other_selected ? " selected" : "") + ">Egyéb...</option>";
    h += "</select>";
    
    // Free text input shown only when "Egyéb" selected
    String other_val = show_other_selected ? cur_area : "";
    h += "<input class=\"room-other\" name=\"" + field_name + "_other\" value=\"" + other_val + "\" placeholder=\"Helyiség neve\" style=\"display:" + String(show_other_selected ? "block" : "none") + ";margin-top:4px\">";
    
    return h;
}

// ─── STATUS PAGE ───────────────────────────────────────────────
static void handleStatus() {
    String html;
    html.reserve(6000);  // Pre-allocate to reduce heap fragmentation
    html = R"rawliteral(<!DOCTYPE html><html lang="hu"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Modbus-MQTT Bridge — Státusz</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#0d1117;color:#c9d1d9;padding:12px;font-size:15px}
h1{color:#58a6ff;font-size:1.3em;margin:8px 0}
h2{color:#f0883e;background:#161b22;padding:8px 12px;border-radius:6px;margin:16px 0 8px;font-size:1.05em}
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px;margin:6px 0}
.row{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #21262d}
.row:last-child{border:none}
.key{color:#8b949e}.val{color:#c9d1d9;font-weight:600}
.on{color:#3fb950}.off{color:#f85149}.warn{color:#f0883e}
.nav{display:flex;gap:8px;margin:8px 0}
.nav a{background:#21262d;color:#58a6ff;padding:8px 16px;border-radius:6px;text-decoration:none;font-size:14px}
.nav a:hover{background:#30363d}
.nav a.active{background:#238636;color:white}
</style></head><body>)rawliteral";

    html += "<h1>&#9889; " + String(cfg.hostname) + "</h1>";
    html += "<div class=\"nav\"><a class=\"active\" href=\"/\">Státusz</a><a href=\"/config\">Beállítások</a><a href=\"/pins\">Pinek</a><a href=\"/modules\">Modulok</a><a href=\"/ota\">OTA</a><a href=\"/admin\">Admin</a></div>";
    
    // Network — Dual stack display (both interfaces always visible)
    html += "<h2>&#128279; Hálózat</h2><div class=\"card\">";
    
    // LAN status  
    bool lan_up = eth_is_connected();
    bool lan_on = eth_is_started();
    html += "<div class=\"row\"><span class=\"key\">LAN</span><span class=\"val " + String(lan_up?"on":"off") + "\">" + String(lan_up?"CSATLAKOZVA ✅": lan_on?"Keresés... ⏳":"LEÁLLÍTVA ⚫") + "</span></div>";
    if (lan_on) {
        html += "<div class=\"row\"><span class=\"key\">LAN IP</span><span class=\"val\">" + eth_get_ip() + "</span></div>";
    }
    html += "<div class=\"row\"><span class=\"key\">LAN DHCP</span><span class=\"val\">" + String(cfg.lan_dhcp?"Igen":"Nem (statikus)") + "</span></div>";
    
    // WiFi status
    bool wifi_up = (WiFi.status() == WL_CONNECTED);
    html += "<div class=\"row\"><span class=\"key\">WiFi</span><span class=\"val " + String(wifi_up?"on":"off") + "\">" + String(wifi_up?"CSATLAKOZVA ✅":"LECSATLAKOZVA ❌") + "</span></div>";
    if (wifi_up) {
        html += "<div class=\"row\"><span class=\"key\">WiFi IP</span><span class=\"val\">" + WiFi.localIP().toString() + "</span></div>";
        html += "<div class=\"row\"><span class=\"key\">RSSI</span><span class=\"val\">" + String(WiFi.RSSI()) + " dBm</span></div>";
    }
    html += "<div class=\"row\"><span class=\"key\">WiFi DHCP</span><span class=\"val\">" + String(cfg.wifi_dhcp?"Igen":"Nem (statikus)") + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">WiFi mód</span><span class=\"val\">" + String(cfg.wifi_mode==0?"AP+STA (mindig elérhető)":"Csak STA") + "</span></div>";
    
    // Active interface indicator
    html += "<div class=\"row\"><span class=\"key\">Aktív elsődleges</span><span class=\"val\" style=\"color:" + ifColor(cfg.active_if) + "\">" + ifName(cfg.active_if) + "</span></div>";
    
    // AP status
    bool ap_active = (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA);
    html += "<div class=\"row\"><span class=\"key\">WiFi AP</span><span class=\"val " + String(ap_active?"on":"off") + "\">" + String(ap_active?"AKTÍV ✅":"KI ❌") + "</span></div>";
    if (ap_active) {
        String live_ap = cfg.ap_name[0] ? String(cfg.ap_name) : String(cfg.hostname) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
        html += "<div class=\"row\"><span class=\"key\">AP Név</span><span class=\"val\">" + live_ap + "</span></div>";
        html += "<div class=\"row\"><span class=\"key\">AP IP</span><span class=\"val\">" + WiFi.softAPIP().toString() + "</span></div>";
    }
    html += "</div>";
    
    // Strategy info
    html += "<div class=\"card\" style=\"border-color:#f0883e\"><div class=\"row\"><span class=\"key\">&#128279; Stratégia</span><span class=\"val warn\">LAN elsődleges → WiFi auto-fallback → LAN auto-visszaváltás</span></div></div>";
    
    // MQTT
    html += "<h2>&#128172; MQTT</h2><div class=\"card\">";
    bool mc = mqtt_is_connected();
    html += "<div class=\"row\"><span class=\"key\">Állapot</span><span class=\"val " + String(mc?"on":"off") + "\">" + String(mc?"CSATLAKOZVA ✅":"NEM CSATLAKOZOTT ❌") + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Broker</span><span class=\"val\">" + String(cfg.mqtt_host) + ":" + String(cfg.mqtt_port) + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Prefix</span><span class=\"val\">" + String(cfg.mqtt_prefix) + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">HA Discovery</span><span class=\"val " + String(cfg.ha_discovery?"on":"off") + "\">" + String(cfg.ha_discovery?"BE ✅":"KI ❌") + "</span></div>";
    html += "</div>";
    
    // Modbus modules
    html += "<h2>&#128268; Modbus Modulok</h2><div class=\"card\">";
    html += "<div class=\"row\"><span class=\"key\">Modulok száma</span><span class=\"val\">" + String(module_count) + "</span></div>";
    // Show saved module list indicator
    {
        Preferences nv;
        nv.begin(NV_NAMESPACE, true);
        uint8_t saved_n = nv.getUChar(NV_KEY_MOD_LIST_N, 0);
        nv.end();
        if (saved_n > 0) {
            html += "<div class=\"row\"><span class=\"key\">Mentett modullista</span><span class=\"val on\">✅ " + String(saved_n) + " modul (gyors boot)</span></div>";
        } else {
            html += "<div class=\"row\"><span class=\"key\">Mentett modullista</span><span class=\"val off\">❌ Nincs (scan minden bootnál)</span></div>";
        }
    }
    html += "<div class=\"row\"><span class=\"key\">Cím tartomány</span><span class=\"val\">" + String(cfg.mb_scan_start) + " - " + String(cfg.mb_scan_end) + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Baud / Paritás</span><span class=\"val\">" + String(cfg.mb_baud) + " / " + String(cfg.mb_parity==0?"8N1":cfg.mb_parity==1?"8E1":"8O1") + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Profil</span><span class=\"val\">" + profileName(cfg.mb_profile) + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Polling időköz</span><span class=\"val\">" + String(cfg.mb_poll_ms) + " ms</span></div>";
    
    // Per-module status
    for (uint16_t i = 0; i < module_count; i++) {
        Slave_Module &m = modules[i];
        String cls = m.online ? "on" : "off";
        String st = m.online ? "✅ ONLINE" : "❌ OFFLINE";
        String sim_tag = m.is_virtual ? " 🔹" : "";
        html += "<div class=\"row\" style=\"margin-top:6px;padding-top:6px;border-top:1px solid #30363d\"><span class=\"key\">S" + String(m.slave_addr) + " " + String(m.model.model_name) + sim_tag + "</span><span class=\"val " + cls + "\">" + st + "</span></div>";
        if (m.online) {
            html += "<div class=\"row\"><span class=\"key\">└ FW / SN</span><span class=\"val\">" + String(m.model.firmware_ver) + " / " + String(m.model.serial_number) + "</span></div>";
        }
    }
    html += "</div>";
    
    // Modbus Statistics
    html += "<h2>&#128202; Modbus Statisztika</h2><div class=\"card\">";
    html += "<div class=\"row\"><span class=\"key\">TX (kérés)</span><span class=\"val\">" + String(mb_stats.tx_count) + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">RX (válasz)</span><span class=\"val\">" + String(mb_stats.rx_count) + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Hibák</span><span class=\"val " + String(mb_stats.err_count > 0 ? "warn" : "") + "\">" + String(mb_stats.err_count) + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Utolsó hibakód</span><span class=\"val\">" + String(mb_stats.last_err_code) + "</span></div>";
    if (mb_stats.last_err_time > 0) {
        uint32_t since = (millis() - mb_stats.last_err_time) / 1000;
        html += "<div class=\"row\"><span class=\"key\">Utolsó hiba</span><span class=\"val\">" + String(since) + " mp ezelőtt</span></div>";
    } else {
        html += "<div class=\"row\"><span class=\"key\">Utolsó hiba</span><span class=\"val\">Nincs</span></div>";
    }
    html += "<div class=\"row\"><span class=\"key\">Utolsó címzett slave</span><span class=\"val\">S" + String(mb_stats.last_slave_addr) + "</span></div>";
    html += "</div>";
    
    // TCP Bridge
    html += "<h2>&#128272; TCP Bridge</h2><div class=\"card\">";
    html += "<div class=\"row\"><span class=\"key\">Állapot</span><span class=\"val " + String(cfg.tcp_enabled?"on":"off") + "\">" + String(cfg.tcp_enabled?"BE ✅":"KI ❌") + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Port</span><span class=\"val\">" + String(cfg.tcp_port) + "</span></div>";
    html += "</div>";
    
    // System
    html += "<h2>&#9881; Rendszer</h2><div class=\"card\">";
    html += "<div class=\"row\"><span class=\"key\">Hostname</span><span class=\"val\">" + String(cfg.hostname) + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Firmware</span><span class=\"val\">v" + String(FIRMWARE_VERSION) + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Uptime</span><span class=\"val\">" + uptimeStr() + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Heap free</span><span class=\"val\">" + String(ESP.getFreeHeap() / 1024) + " KB</span></div>";
    // Bridge system sensors
    {
        bool wifi_connected = (WiFi.status() == WL_CONNECTED);
        html += "<div class=\"row\"><span class=\"key\">WiFi RSSI</span><span class=\"val\">" + (wifi_connected ? String(WiFi.RSSI()) + " dBm" : String("—")) + "</span></div>";
        html += "<div class=\"row\"><span class=\"key\">Szabad memória</span><span class=\"val\">" + String(ESP.getFreeHeap()) + " B</span></div>";
        const char *net_if_str = cfg.active_if == NET_IF_LAN ? "LAN" : cfg.active_if == NET_IF_WIFI ? "WiFi" : "NONE";
        String net_detail = String(net_if_str);
        if (cfg.active_if == NET_IF_LAN && eth_is_started()) net_detail += " (" + eth_get_ip() + ")";
        else if (cfg.active_if == NET_IF_WIFI && WiFi.status() == WL_CONNECTED) net_detail += " (" + WiFi.localIP().toString() + ")";
        html += "<div class=\"row\"><span class=\"key\">Hálózat</span><span class=\"val\" style=\"color:" + ifColor(cfg.active_if) + "\">" + net_detail + "</span></div>";
        // Debug: raw update_network() values
        bool _wifi_ok = (WiFi.status() == WL_CONNECTED);
        html += "<div class=\"row\"><span class=\"key\">[DBG] net_connected</span><span class=\"val\">" + String(net_connected ? "true" : "false") + "</span></div>";
        html += "<div class=\"row\"><span class=\"key\">[DBG] wifi_ok</span><span class=\"val\">" + String(_wifi_ok ? "true" : "false") + "</span></div>";
        html += "<div class=\"row\"><span class=\"key\">[DBG] lan_enabled</span><span class=\"val\">" + String(cfg.lan_enabled ? "true" : "false") + "</span></div>";
        html += "<div class=\"row\"><span class=\"key\">[DBG] active_if</span><span class=\"val\">" + String(cfg.active_if) + "</span></div>";
        // MQTT debug
        html += "<div class=\"row\"><span class=\"key\">[DBG] mqtt_host</span><span class=\"val\">" + String(cfg.mqtt_host[0] ? cfg.mqtt_host : "empty") + "</span></div>";
        html += "<div class=\"row\"><span class=\"key\">[DBG] mqtt_connected</span><span class=\"val\">" + String(mqtt_is_connected() ? "true" : "false") + "</span></div>";
    }
    html += "<div class=\"row\"><span class=\"key\">Flash használat</span><span class=\"val\">61%</span></div>";
    html += "</div>";
    
    // Restart button
    html += "<div style=\"margin-top:12px;text-align:center\"><a href=\"/restart\" style=\"background:#da3633;color:white;padding:10px 20px;border-radius:6px;text-decoration:none;display:inline-block\" onclick=\"return confirm('Biztosan újraindítod az eszközt?')\">&#128260; Újraindítás</a></div>";
    
    html += "<p style=\"text-align:center;color:#484f58;font-size:11px;margin-top:16px\">Modbus-MQTT Bridge v2.0 — ESP32-S3-ETH (6DI+6R)</p>";
    html += "</body></html>";
    
    web.send(200, "text/html", html);
}

// ─── CONFIG PAGE ───────────────────────────────────────────────
static void handleConfig() {
    // Build form with current values pre-filled
    String html;
    html.reserve(8000);
    html = R"rawliteral(<!DOCTYPE html><html lang="hu"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Modbus-MQTT Bridge — Beállítások</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#0d1117;color:#c9d1d9;padding:12px;font-size:15px}
h1{color:#58a6ff;font-size:1.3em;margin:8px 0}
h2{color:#f0883e;background:#161b22;padding:8px 12px;border-radius:6px;margin:16px 0 8px;font-size:1.05em}
.fm{margin-bottom:8px}
label{display:block;font-weight:600;color:#7ee787;margin-bottom:2px;font-size:14px}
input,select{width:100%;padding:8px;border:1px solid #30363d;border-radius:4px;background:#0d1117;color:#c9d1d9;font-size:15px}
input:focus,select:focus{outline:none;border-color:#58a6ff}
.row{display:flex;gap:8px}
.row .fm{flex:1}
.radio{display:flex;gap:16px;margin:6px 0;padding:8px;background:#161b22;border-radius:6px;border:1px solid #30363d}
.radio label{display:flex;align-items:center;gap:6px;font-weight:normal;color:#c9d1d9;cursor:pointer;margin:0;padding:4px 12px;border-radius:4px}
.radio label:has(input:checked){background:#238636;color:white}
.radio input{width:auto;accent-color:#58a6ff}
.chk{display:flex;align-items:center;gap:6px;margin:6px 0}
.chk label{margin:0;font-weight:normal;color:#c9d1d9;cursor:pointer}
.chk input{width:auto;accent-color:#58a6ff}
button{background:#238636;color:white;border:none;padding:10px 20px;border-radius:6px;font-size:16px;cursor:pointer;margin:10px 0}
button:hover{background:#2ea043}
.note{color:#8b949e;font-size:12px;margin:2px 0 8px}
.pri{background:#161b22;border:1px solid #f0883e;border-radius:6px;padding:12px;margin:8px 0}
.pri b{color:#f0883e}
.nav{display:flex;gap:8px;margin:8px 0}
.nav a{background:#21262d;color:#58a6ff;padding:8px 16px;border-radius:6px;text-decoration:none;font-size:14px}
.nav a:hover{background:#30363d}
.nav a.active{background:#238636;color:white}
.foot{text-align:center;color:#484f58;font-size:11px;margin-top:16px;border-top:1px solid #21262d;padding-top:8px}
</style>
</head><body>
<h1>&#9889; Beállítások</h1>
<div class="nav"><a href="/">Státusz</a><a class="active" href="/config">Beállítások</a><a href="/pins">Pinek</a><a href="/modules">Modulok</a><a href="/ota">OTA</a><a href="/admin">Admin</a></div>
<div class="pri"><b>&#128279; LAN elsődleges — WiFi automatikus fallback</b><br><span class="note">LAN mindig preferált. Ha leáll, WiFi átveszi. Ha LAN visszajön, automatikusan visszaáll.</span></div>
<form action="/save" method="POST">)rawliteral";

    // ── Device Identity ──────────────────────────────────
    html += "<h2>&#128100; Eszközazonosító</h2>";
    html += "<div class=\"fm\"><label>Hostname (mDNS, AP név, MQTT ID)</label><input name=\"hostname\" value=\"" + String(cfg.hostname) + "\"></div>";
    html += "<p class=\"note\">Több eszköznél egyedi hostname szükséges! Pl: modbusmqtt-garazs, modbusmqtt-pince</p>";
    
    // ── LAN Section ─────────────────────────────────────
    html += "<h2>&#128279; LAN / Ethernet (Elsődleges)</h2>";
    html += "<div class=\"chk\"><input type=\"checkbox\" id=\"lanen\" name=\"lanen\" value=\"1\" " + String(cfg.lan_enabled?"checked":"") + " onchange=\"toggleLan()\"><label for=\"lanen\">LAN engedélyezése</label></div>";
    html += "<div id=\"lanSection\" style=\"display:" + String(cfg.lan_enabled?"block":"none") + "\">";
    
    html += "<div class=\"fm\"><label>Chip típus</label><select name=\"lantype\">";
    html += "<option value=\"0\"" + String(cfg.lan_type==0?" selected":"") + ">W5500 (SPI)</option>";
    html += "<option value=\"1\"" + String(cfg.lan_type==1?" selected":"") + ">LAN8720 (RMII)</option>";
    html += "<option value=\"2\"" + String(cfg.lan_type==2?" selected":"") + ">IP101 (Waveshare beépített)</option></select></div>";
    
    html += "<div class=\"radio\"><label><input type=\"radio\" name=\"landhcp\" value=\"1\" " + String(cfg.lan_dhcp?"checked":"") + " onchange=\"toggleLanStatic()\"> DHCP (automatikus)</label>";
    html += "<label><input type=\"radio\" name=\"landhcp\" value=\"0\" " + String(!cfg.lan_dhcp?"checked":"") + " onchange=\"toggleLanStatic()\"> Kézi (statikus IP)</label></div>";
    
    html += "<div id=\"lanStatic\" style=\"display:" + String(cfg.lan_dhcp?"none":"block") + "\">";
    html += "<div class=\"row\"><div class=\"fm\"><label>IP cím</label><input name=\"lanip\" value=\"" + String(cfg.lan_ip) + "\" placeholder=\"192.168.1.100\"></div>";
    html += "<div class=\"fm\"><label>Alhálózati maszk</label><input name=\"lanmask\" value=\"" + String(cfg.lan_mask) + "\" placeholder=\"255.255.255.0\"></div></div>";
    html += "<div class=\"row\"><div class=\"fm\"><label>Átjáró</label><input name=\"langw\" value=\"" + String(cfg.lan_gw) + "\" placeholder=\"192.168.1.1\"></div>";
    html += "<div class=\"fm\"><label>DNS</label><input name=\"landns\" value=\"" + String(cfg.lan_dns) + "\" placeholder=\"192.168.1.1\"></div></div></div></div>";
    
    // ── WiFi Section ─────────────────────────────────────
    html += "<h2>&#128225; WiFi (Fallback + AP)</h2>";
    html += "<div class=\"radio\"><label><input type=\"radio\" name=\"wifimode\" value=\"0\" " + String(cfg.wifi_mode==0?"checked":"") + "> AP+STA (mindig elérhető AP-n)</label>";
    html += "<label><input type=\"radio\" name=\"wifimode\" value=\"1\" " + String(cfg.wifi_mode==1?"checked":"") + "> Csak STA</label></div>";
    html += "<p class=\"note\">AP+STA: saját WiFi hálózatot is sugároz, így elérhető ha nincs router. Csak STA: kevesebb áram, de router nélkül elérhetetlen.</p>";
    html += "<div class=\"fm\"><label>SSID</label><input name=\"ssid\" value=\"" + String(cfg.wifi_ssid) + "\" placeholder=\"WiFi neve\"></div>";
    html += "<div class=\"fm\"><label>Jelszó</label><input type=\"password\" name=\"wpass\" value=\"" + String(cfg.wifi_pass) + "\" placeholder=\"WiFi jelszó\"></div>";
    html += "<div class=\"fm\"><label>AP Név</label><input name=\"apn\" value=\"" + String(cfg.ap_name) + "\" placeholder=\"Üres = automatikus (hostname-MAC)\"></div>";
    html += "<div class=\"fm\"><label>AP Jelszó</label><input type=\"password\" name=\"appass\" value=\"" + String(cfg.ap_pass) + "\" placeholder=\"12345678\"><p class=\"note\">Min. 8 karakter. Mentés után az AP azonnal újraindul az új jelszóval.</p></div>";
    
    html += "<div class=\"radio\"><label><input type=\"radio\" name=\"wdhcp\" value=\"1\" " + String(cfg.wifi_dhcp?"checked":"") + " onchange=\"toggleWifiStatic()\"> DHCP (automatikus)</label>";
    html += "<label><input type=\"radio\" name=\"wdhcp\" value=\"0\" " + String(!cfg.wifi_dhcp?"checked":"") + " onchange=\"toggleWifiStatic()\"> Kézi (statikus IP)</label></div>";
    
    html += "<div id=\"wifiStatic\" style=\"display:" + String(cfg.wifi_dhcp?"none":"block") + "\">";
    html += "<div class=\"row\"><div class=\"fm\"><label>IP cím</label><input name=\"wip\" value=\"" + String(cfg.wifi_ip) + "\" placeholder=\"192.168.1.101\"></div>";
    html += "<div class=\"fm\"><label>Alhálózati maszk</label><input name=\"wmask\" value=\"" + String(cfg.wifi_mask) + "\" placeholder=\"255.255.255.0\"></div></div>";
    html += "<div class=\"row\"><div class=\"fm\"><label>Átjáró</label><input name=\"wgw\" value=\"" + String(cfg.wifi_gw) + "\" placeholder=\"192.168.1.1\"></div>";
    html += "<div class=\"fm\"><label>DNS</label><input name=\"wdns\" value=\"" + String(cfg.wifi_dns) + "\" placeholder=\"192.168.1.1\"></div></div></div>";
    
    // ── MQTT Section ─────────────────────────────────────
    html += "<h2>&#128172; MQTT Broker</h2>";
    html += "<div class=\"row\"><div class=\"fm\"><label>Host</label><input name=\"mhost\" value=\"" + String(cfg.mqtt_host) + "\"></div>";
    html += "<div class=\"fm\"><label>Port</label><input type=\"number\" name=\"mport\" value=\"" + String(cfg.mqtt_port) + "\"></div></div>";
    html += "<div class=\"row\"><div class=\"fm\"><label>Felhasználó</label><input name=\"muser\" value=\"" + String(cfg.mqtt_user) + "\"></div>";
    html += "<div class=\"fm\"><label>Jelszó</label><input type=\"password\" name=\"mpass\" value=\"" + String(cfg.mqtt_pass) + "\"></div></div>";
    html += "<div class=\"fm\"><label>MQTT Prefix</label><input name=\"mpfx\" value=\"" + String(cfg.mqtt_prefix) + "\"></div>";
    
    // ── Auth Section ─────────────────────────────────────
    html += "<h2>&#128272; Web hitelesítés</h2>";
    html += "<div class=\"chk\"><input type=\"checkbox\" id=\"wauth\" name=\"wauth\" value=\"1\" " + String(cfg.web_auth?"checked":"") + "><label for=\"wauth\">Web hitelesítés bekapcsolása</label></div>";
    html += "<div class=\"fm\"><label>Auth jelszó</label><input type=\"password\" name=\"wauthp\" value=\"" + String(cfg.web_pass) + "\" placeholder=\"Üres = kikapcsolva\"></div>";
    html += "<p class=\"note\">A hitelesítés védi az író műveleteket (relé, konfig, OTA). Olvasás (státusz) nyitva marad. Felhasználónév: admin</p>";
    
    // ── HA Section ───────────────────────────────────────
    html += "<h2>&#127968; Home Assistant</h2>";
    html += "<div class=\"chk\"><input type=\"checkbox\" id=\"hadisc\" name=\"hadisc\" value=\"1\" " + String(cfg.ha_discovery?"checked":"") + "><label for=\"hadisc\">Auto-discovery (MQTT automatikus eszközfelvétel)</label></div>";
    html += "<p class=\"note\">6 relé + 6 DI automatikusan megjelenik a HA-ban. Kikapcsolva csak nyers MQTT.</p>";
    
    // ── Modbus Section ───────────────────────────────────
    html += "<h2>&#128268; Modbus RS485</h2>";
    html += "<div class=\"row\"><div class=\"fm\"><label>Baud rate</label><select name=\"mbaud\">";
    for (int b : {9600, 19200, 38400, 115200}) {
        html += "<option value=\"" + String(b) + "\"" + String(cfg.mb_baud==(uint32_t)b?" selected":"") + ">" + String(b) + "</option>";
    }
    html += "</select></div>";
    html += "<div class=\"fm\"><label>Paritás</label><select name=\"mpar\">";
    html += "<option value=\"0\"" + String(cfg.mb_parity==0?" selected":"") + ">8N1</option>";
    html += "<option value=\"1\"" + String(cfg.mb_parity==1?" selected":"") + ">8E1</option>";
    html += "<option value=\"2\"" + String(cfg.mb_parity==2?" selected":"") + ">8O1</option></select></div></div>";
    html += "<div class=\"row\"><div class=\"fm\"><label>Cím kezdete</label><input type=\"number\" name=\"mstart\" value=\"" + String(cfg.mb_scan_start) + "\" min=\"1\" max=\"247\"></div>";
    html += "<div class=\"fm\"><label>Cím vége</label><input type=\"number\" name=\"mend\" value=\"" + String(cfg.mb_scan_end) + "\" min=\"1\" max=\"247\"></div></div>";
    
    // Profile dropdown
    html += "<div class=\"fm\"><label>Modbus profil</label><select name=\"mbprof\" id=\"mbprof\" onchange=\"toggleGeneric()\">";
    html += "<option value=\"1\"" + String(cfg.mb_profile==MB_PROFILE_KC868_HA?" selected":"") + ">KC868-HA V2</option>";
    html += "<option value=\"2\"" + String(cfg.mb_profile==MB_PROFILE_GENERIC?" selected":"") + ">Generic (konfigurálható)</option>";
    html += "<option value=\"0\"" + String(cfg.mb_profile==MB_PROFILE_CUSTOM?" selected":"") + ">Egyedi</option>";
    html += "</select></div>";
    
    // Generic profile register settings (only visible when Generic selected)
    html += "<div id=\"genericSection\" style=\"display:" + String(cfg.mb_profile==MB_PROFILE_GENERIC?"block":"none") + "\">";
    html += "<div class=\"row\"><div class=\"fm\"><label>Coil kezdő regiszter</label><input type=\"number\" name=\"mbcoil\" value=\"" + String(cfg.mb_reg_coil_start) + "\" min=\"0\" max=\"65535\"></div>";
    html += "<div class=\"fm\"><label>DI kezdő regiszter</label><input type=\"number\" name=\"mbdi\" value=\"" + String(cfg.mb_reg_di_start) + "\" min=\"0\" max=\"65535\"></div></div>";
    html += "</div>";
    
    // Poll rate
    html += "<div class=\"fm\"><label>Polling időköz (ms)</label><input type=\"number\" name=\"mbpoll\" value=\"" + String(cfg.mb_poll_ms) + "\" min=\"500\" max=\"30000\"></div>";
    html += "<p class=\"note\">Modbus lekérdezés gyakorisága. Alacsonyabb = gyorsabb válasz, de nagyobb hálózati forgalom. Ajánlott: 1000-5000 ms.</p>";
    
    // ── Virtual Module ──────────────────────────────
    html += "<h2>&#128187; Virtuális Modul (Teszt)</h2>";
    html += "<div class=\"chk\"><input type=\"checkbox\" id=\"vmod\" name=\"vmod\" value=\"1\" " + String(cfg.virtual_module?"checked":"") + "><label for=\"vmod\">Virtuális HA V2 modul bekapcsolása</label></div>";
    html += "<p class=\"note\">Cím 200. Szimulált 6 relé + 6 DI fizikai hardver nélkül. Tesztelésre és HA dashboard beállításra.</p>";
    
    // ── TCP Section ──────────────────────────────────────
    html += "<h2>&#128272; Modbus TCP Bridge</h2>";
    html += "<div class=\"chk\"><input type=\"checkbox\" id=\"tcpen\" name=\"tcpen\" value=\"1\" " + String(cfg.tcp_enabled?"checked":"") + " onchange=\"toggleTcp()\"><label for=\"tcpen\">TCP bridge engedélyezése</label></div>";
    html += "<div id=\"tcpSection\" style=\"display:" + String(cfg.tcp_enabled?"block":"none") + "\"><div class=\"fm\"><label>TCP port</label><input type=\"number\" name=\"tcpp\" value=\"" + String(cfg.tcp_port) + "\"></div></div>";
    
    // Save button
    html += "<button type=\"submit\">&#128190; Mentés & Újraindítás</button></form>";
    
    // Back to status
    html += "<div style=\"text-align:center;margin:12px 0\"><a href=\"/\" style=\"color:#58a6ff\">← Vissza a státusz oldalra</a></div>";
    
    html += "<div class=\"foot\">Modbus-MQTT Bridge v2.0 — ESP32-S3-ETH (6DI+6R) — ESP32-S3</div>";
    
    // JavaScript
    html += R"rawliteral(<script>
function toggleLan(){document.getElementById('lanSection').style.display=document.getElementById('lanen').checked?'block':'none'}
function toggleLanStatic(){var d=document.querySelector('input[name=landhcp]:checked').value==='0';document.getElementById('lanStatic').style.display=d?'block':'none'}
function toggleWifiStatic(){var d=document.querySelector('input[name=wdhcp]:checked').value==='0';document.getElementById('wifiStatic').style.display=d?'block':'none'}
function toggleTcp(){document.getElementById('tcpSection').style.display=document.getElementById('tcpen').checked?'block':'none'}
function toggleGeneric(){var p=document.getElementById('mbprof').value;document.getElementById('genericSection').style.display=(p==='2')?'block':'none'}
</script></body></html>)rawliteral";
    
    web.send(200, "text/html", html);
}

// ─── SAVE Handler ──────────────────────────────────────────────
static void handleSave() {
    if (!web_auth_ok()) return;
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    
    // Only write keys that are present in the POST body (partial update).
    // Missing keys are NOT overwritten — prevents D4 bug (empty field → NVRAM zeroed)
    
    if (web.hasArg("ssid"))       nv.putString(NV_KEY_WIFI_SSID, web.arg("ssid"));
    if (web.hasArg("wpass"))      nv.putString(NV_KEY_WIFI_PASS, web.arg("wpass"));
    if (web.hasArg("apn"))        nv.putString(NV_KEY_AP_NAME, web.arg("apn"));
    if (web.hasArg("appass"))    nv.putString(NV_KEY_AP_PASS, web.arg("appass"));
    if (web.hasArg("wifimode"))   nv.putUChar(NV_KEY_WIFI_MODE, web.arg("wifimode").toInt());
    if (web.hasArg("wdhcp"))      nv.putBool(NV_KEY_WIFI_DHCP, web.arg("wdhcp") == "1");
    if (web.hasArg("wip"))        nv.putString(NV_KEY_WIFI_IP, web.arg("wip"));
    if (web.hasArg("wgw"))        nv.putString(NV_KEY_WIFI_GW, web.arg("wgw"));
    if (web.hasArg("wmask"))      nv.putString(NV_KEY_WIFI_MASK, web.arg("wmask"));
    if (web.hasArg("wdns"))       nv.putString(NV_KEY_WIFI_DNS, web.arg("wdns"));
    
    if (web.hasArg("lanen"))      nv.putBool(NV_KEY_ETH_EN, web.arg("lanen") == "1");
    if (web.hasArg("landhcp"))    nv.putBool(NV_KEY_ETH_DHCP, web.arg("landhcp") == "1");
    if (web.hasArg("lanip"))      nv.putString(NV_KEY_ETH_IP, web.arg("lanip"));
    if (web.hasArg("langw"))      nv.putString(NV_KEY_ETH_GW, web.arg("langw"));
    if (web.hasArg("lanmask"))    nv.putString(NV_KEY_ETH_MASK, web.arg("lanmask"));
    if (web.hasArg("landns"))     nv.putString(NV_KEY_ETH_DNS, web.arg("landns"));
    if (web.hasArg("lantype"))    nv.putUChar(NV_KEY_ETH_TYPE, web.arg("lantype").toInt());
    
    if (web.hasArg("mhost"))      nv.putString(NV_KEY_MQTT_HOST, web.arg("mhost"));
    if (web.hasArg("mport"))      nv.putUShort(NV_KEY_MQTT_PORT, web.arg("mport").toInt());
    if (web.hasArg("muser"))      nv.putString(NV_KEY_MQTT_USER, web.arg("muser"));
    if (web.hasArg("mpass"))      nv.putString(NV_KEY_MQTT_PASS, web.arg("mpass"));
    if (web.hasArg("mpfx"))       nv.putString(NV_KEY_MQTT_PREFIX, web.arg("mpfx"));
    if (web.hasArg("mtls"))       nv.putBool(NV_KEY_MQTT_TLS, web.arg("mtls") == "1");
    
    if (web.hasArg("hadisc"))     nv.putBool(NV_KEY_HA_DISC, web.arg("hadisc") == "1");
    
    if (web.hasArg("mbaud"))      nv.putUInt(NV_KEY_MB_BAUD, web.arg("mbaud").toInt());
    if (web.hasArg("mstart"))     nv.putUChar(NV_KEY_MB_START, web.arg("mstart").toInt());
    if (web.hasArg("mend"))       nv.putUChar(NV_KEY_MB_END, web.arg("mend").toInt());
    if (web.hasArg("mpar"))       nv.putUChar(NV_KEY_MB_PARITY, web.arg("mpar").toInt());
    if (web.hasArg("mbprof"))     nv.putUChar(NV_KEY_MB_PROFILE, web.arg("mbprof").toInt());
    if (web.hasArg("mbcoil"))     nv.putUShort(NV_KEY_MB_REG_COIL, web.arg("mbcoil").toInt());
    if (web.hasArg("mbdi"))       nv.putUShort(NV_KEY_MB_REG_DI, web.arg("mbdi").toInt());
    if (web.hasArg("mbpoll"))     nv.putUShort(NV_KEY_MB_POLL_MS, web.arg("mbpoll").toInt());
    if (web.hasArg("vmod"))       nv.putBool(NV_KEY_VIRTUAL_MOD, web.arg("vmod") == "1");
    
    if (web.hasArg("tcpen"))      nv.putBool(NV_KEY_TCP_EN, web.arg("tcpen") == "1");
    if (web.hasArg("tcpp"))       nv.putUShort(NV_KEY_TCP_PORT, web.arg("tcpp").toInt());
    
    if (web.hasArg("hostname"))   nv.putString(NV_KEY_HOSTNAME, web.arg("hostname"));
    
    if (web.hasArg("wauth"))       nv.putBool(NV_KEY_WEB_AUTH, web.arg("wauth") == "1");
    if (web.hasArg("wauthp"))      nv.putString(NV_KEY_WEB_PASS, web.arg("wauthp"));
    
    nv.end();
    LOG_ILN("[WEB] Settings saved");
    
    web.send(200, "text/html", R"rawliteral(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Mentve</title>
<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;text-align:center;padding:40px}
.ok{color:#3fb950;font-size:28px}p{margin:12px 0;color:#8b949e}</style></head>
<body><h1 class="ok">&#10004; Beállítások elmentve!</h1>
<p>Az eszköz újraindul 3 másodperc múlva...</p>
<script>setTimeout(function(){window.location='/'},5000)</script>
</body></html>)rawliteral");

    delay(3000);
    ESP.restart();
}

// ─── RESTART Handler ───────────────────────────────────────────
static void handleRestart() {
    if (!web_auth_ok()) return;
    web.send(200, "text/html", R"rawliteral(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Újraindítás</title>
<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;text-align:center;padding:40px}
.ok{color:#f0883e;font-size:28px}p{margin:12px 0;color:#8b949e}</style></head>
<body><h1 class="ok">&#128260; Újraindítás...</h1><p>Az eszköz újraindul.</p>
<script>setTimeout(function(){window.location='/'},10000)</script>
</body></html>)rawliteral");
    delay(1000);
    ESP.restart();
}

// ─── PINS PAGE ─────────────────────────────────────────────────
static void handlePins() {
    String html;
    html.reserve(6000);
    html = R"rawliteral(<!DOCTYPE html><html lang="hu"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Modbus-MQTT Bridge — Pinek</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#0d1117;color:#c9d1d9;padding:12px;font-size:15px}
h1{color:#58a6ff;font-size:1.3em;margin:8px 0}
h2{color:#f0883e;background:#161b22;padding:8px 12px;border-radius:6px;margin:16px 0 8px;font-size:1.05em}
.fm{margin-bottom:8px}
label{display:block;font-weight:600;color:#7ee787;margin-bottom:2px;font-size:14px}
input,select{width:100%;padding:8px;border:1px solid #30363d;border-radius:4px;background:#0d1117;color:#c9d1d9;font-size:15px}
input:focus,select:focus{outline:none;border-color:#58a6ff}
.row{display:flex;gap:8px}
.row .fm{flex:1}
button{background:#238636;color:white;border:none;padding:10px 20px;border-radius:6px;font-size:16px;cursor:pointer;margin:10px 0}
button:hover{background:#2ea043}
.note{color:#8b949e;font-size:12px;margin:2px 0 8px}
.warn-box{background:#161b22;border:1px solid #f0883e;border-radius:6px;padding:12px;margin:8px 0}
.warn-box b{color:#f0883e}
.nav{display:flex;gap:8px;margin:8px 0}
.nav a{background:#21262d;color:#58a6ff;padding:8px 16px;border-radius:6px;text-decoration:none;font-size:14px}
.nav a:hover{background:#30363d}
.nav a.active{background:#238636;color:white}
.foot{text-align:center;color:#484f58;font-size:11px;margin-top:16px;border-top:1px solid #21262d;padding-top:8px}
</style>
</head><body>
<h1>&#128295; GPIO Pinek</h1>
<div class="nav"><a href="/">Státusz</a><a href="/config">Beállítások</a><a class="active" href="/pins">Pinek</a><a href="/modules">Modulok</a><a href="/ota">OTA</a><a href="/admin">Admin</a></div>
<p class="note">-1 = nem használt / letiltott. A pinek megváltoztatása után újraindítás szükséges!</p>
<form action="/savepins" method="POST">)rawliteral";

    // RS485 section
    html += "<h2>&#128268; RS485 / Modbus bus</h2>";
    html += "<div class=\"row\">";
    html += "<div class=\"fm\"><label>UART2 RX</label><input type=\"number\" name=\"prx\" value=\"" + String(cfg.pin_rs485_rx) + "\" min=\"-1\" max=\"48\"></div>";
    html += "<div class=\"fm\"><label>UART2 TX</label><input type=\"number\" name=\"ptx\" value=\"" + String(cfg.pin_rs485_tx) + "\" min=\"-1\" max=\"48\"></div>";
    html += "<div class=\"fm\"><label>DE/RE (Driver Enable)</label><input type=\"number\" name=\"pde\" value=\"" + String(cfg.pin_rs485_de) + "\" min=\"-1\" max=\"48\"></div>";
    html += "</div>";

    // Status LED & Button
    html += "<h2>&#128161; Státusz LED és gomb</h2>";
    html += "<div class=\"row\">";
    html += "<div class=\"fm\"><label>Status LED</label><input type=\"number\" name=\"pled\" value=\"" + String(cfg.pin_status_led) + "\" min=\"-1\" max=\"48\"></div>";
    html += "<div class=\"fm\"><label>Konfig gomb (BOOT)</label><input type=\"number\" name=\"pbtn\" value=\"" + String(cfg.pin_config_btn) + "\" min=\"-1\" max=\"48\"></div>";
    html += "</div>";

    // W5500 Ethernet
    html += "<h2>&#128279; W5500 SPI Ethernet</h2>";
    html += "<div class=\"warn-box\"><b>&#9888; Figyelem!</b> Waveshare ESP32-S3-ETH V1.0 beépített IP101 Ethernet — ezek a pinek csak W5500 SPI modulhoz!</div>";
    html += "<div class=\"row\">";
    html += "<div class=\"fm\"><label>SPI MOSI</label><input type=\"number\" name=\"pemosi\" value=\"" + String(cfg.pin_eth_mosi) + "\" min=\"-1\" max=\"48\"></div>";
    html += "<div class=\"fm\"><label>SPI MISO</label><input type=\"number\" name=\"pemiso\" value=\"" + String(cfg.pin_eth_miso) + "\" min=\"-1\" max=\"48\"></div>";
    html += "<div class=\"fm\"><label>SPI CLK</label><input type=\"number\" name=\"pesclk\" value=\"" + String(cfg.pin_eth_sclk) + "\" min=\"-1\" max=\"48\"></div>";
    html += "</div>";
    html += "<div class=\"row\">";
    html += "<div class=\"fm\"><label>SPI CS</label><input type=\"number\" name=\"pecs\" value=\"" + String(cfg.pin_eth_cs) + "\" min=\"-1\" max=\"48\"></div>";
    html += "<div class=\"fm\"><label>INT (Interrupt)</label><input type=\"number\" name=\"peint\" value=\"" + String(cfg.pin_eth_int) + "\" min=\"-1\" max=\"48\"></div>";
    html += "<div class=\"fm\"><label>RST (Reset)</label><input type=\"number\" name=\"perst\" value=\"" + String(cfg.pin_eth_rst) + "\" min=\"-1\" max=\"48\"></div>";
    html += "</div>";

    html += "<button type=\"submit\">&#128190; Mentés & Újraindítás</button></form>";
    html += "<div class=\"foot\">Modbus-MQTT Bridge v2.0 — ESP32-S3-ETH (6DI+6R) — ESP32-S3</div>";
    html += "</body></html>";

    web.send(200, "text/html", html);
}

// ─── MODULES PAGE ──────────────────────────────────────────────
static void handleModules() {
    String html;
    html.reserve(8000);  // Largest page, many card loops
    html = R"rawliteral(<!DOCTYPE html><html lang="hu"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Modbus-MQTT Bridge — Modulok</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#0d1117;color:#c9d1d9;padding:12px;font-size:15px}
h1{color:#58a6ff;font-size:1.3em;margin:8px 0}
h2{color:#f0883e;background:#161b22;padding:8px 12px;border-radius:6px;margin:16px 0 8px;font-size:1.05em}
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px;margin:6px 0}
.row{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #21262d}
.row:last-child{border:none}
.key{color:#8b949e}.val{color:#c9d1d9;font-weight:600}
.on{color:#3fb950}.off{color:#f85149}
.fm{margin-bottom:8px}
label{display:block;font-weight:600;color:#7ee787;margin-bottom:2px;font-size:14px}
input,select{width:100%;padding:8px;border:1px solid #30363d;border-radius:4px;background:#0d1117;color:#c9d1d9;font-size:15px}
input:focus,select:focus{outline:none;border-color:#58a6ff}
button{background:#238636;color:white;border:none;padding:10px 20px;border-radius:6px;font-size:16px;cursor:pointer;margin:10px 0}
button:hover{background:#2ea043}
.note{color:#8b949e;font-size:12px;margin:2px 0 8px}
.nav{display:flex;gap:8px;margin:8px 0}
.nav a{background:#21262d;color:#58a6ff;padding:8px 16px;border-radius:6px;text-decoration:none;font-size:14px}
.nav a:hover{background:#30363d}
.nav a.active{background:#238636;color:white}
.foot{text-align:center;color:#484f58;font-size:11px;margin-top:16px;border-top:1px solid #21262d;padding-top:8px}
.mrow{display:flex;gap:8px;align-items:center;margin:4px 0}
.mrow .fm{flex:1}
.mod-card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px;margin:8px 0}
.badge{display:inline-block;padding:2px 8px;border-radius:4px;font-size:12px;margin:2px}
.badge.on{background:#238636;color:white}
.badge.off{background:#f851494d;color:#f85149}
.badge.sensor{background:#1f6feb4d;color:#58a6ff}
.elabel{color:#8b949e;font-size:12px;margin:4px 0 2px}
.mrow-wrap{display:flex;flex-wrap:wrap;gap:6px;margin:4px 0}
.fm-sm{flex:0 0 calc(33% - 4px);min-width:80px}
.fm-sm label{display:block;color:#8b949e;font-size:11px;margin-bottom:2px}
.fm-sm input{width:100%;background:#0d1117;border:1px solid #30363d;color:#c9d1d9;padding:4px 6px;border-radius:4px;font-size:13px}
.room-other{margin-top:4px}
.room-manage{margin:10px 0;padding:10px;background:#161b22;border:1px solid #30363d;border-radius:8px}
.room-manage h3{color:#f0883e;font-size:14px;margin:0 0 8px}
.room-tag{display:inline-flex;align-items:center;gap:4px;background:#21262d;padding:3px 10px;border-radius:4px;margin:2px;font-size:13px;color:#c9d1d9}
.room-tag button{background:none;color:#f85149;border:none;font-size:16px;cursor:pointer;padding:0 2px}
.room-add{display:flex;gap:6px;margin-top:6px}
.room-add input{flex:1;padding:6px 8px;font-size:13px}
.room-add button{padding:6px 12px;font-size:13px}
.rbtn{display:inline-block;cursor:pointer;padding:3px 10px;border-radius:4px;margin:2px;font-weight:600;font-size:12px;border:none;transition:opacity 0.15s}
.rbtn:active{opacity:0.6}
.rbtn.on{background:#238636;color:white}
.rbtn.off{background:#f851494d;color:#f85149;border:1px solid #f8514940}
</style>
<script>
function roomChanged(sel){
  var other=sel.parentElement.querySelector('.room-other');
  if(!other)other=sel.nextElementSibling;
  if(other)other.style.display=sel.value==='_other'?'block':'none';
}
function toggleRelay(addr,relay,curState){
  var newState=curState?0:1;
  var btn=document.getElementById('r'+addr+'_'+relay);
  if(!btn)return;
  btn.style.opacity='0.5';
  fetch('/relay?addr='+addr+'&relay='+relay+'&state='+newState)
  .then(r=>r.json())
  .then(d=>{
    if(d.ok){
      btn.textContent='R'+(relay+1)+' '+(newState?'ON':'OFF');
      btn.className='rbtn '+(newState?'on':'off');
      btn.onclick=function(){toggleRelay(addr,relay,newState);};
    }else{
      alert('Hiba: '+(d.error||'ismeretlen'));
    }
    btn.style.opacity='1';
  })
  .catch(e=>{alert('Hálózati hiba');btn.style.opacity='1';});
}
</script>
</head><body>
<h1>&#128268; Modbus Modulok</h1>
<div class="nav"><a href="/">Státusz</a><a href="/config">Beállítások</a><a href="/pins">Pinek</a><a class="active" href="/modules">Modulok</a><a href="/ota">OTA</a><a href="/admin">Admin</a></div>)rawliteral";

    if (!scan_active) {
        html += "<form action=\"/rescan\" method=\"POST\" style=\"display:inline;margin-bottom:8px\"><button type=\"submit\" style=\"background:#1f6feb\">🔄 Újrascan</button></form>";
        if (module_count > 0) {
            html += " <form action=\"/savemodlist\" method=\"POST\" style=\"display:inline;margin-bottom:8px\"><button type=\"submit\" style=\"background:#238636\">💾 Modullista mentése</button></form>";
        }
    } else {
        html += "<div id=\"scan-progress\" style=\"margin:8px 0\"><div style=\"background:#21262d;border-radius:4px;overflow:hidden;height:20px\"><div id=\"scan-bar\" style=\"background:#1f6feb;height:100%;width:0%;transition:width 0.3s\"></div></div><p class=\"note\" id=\"scan-text\">Buszkeresés folyamatban...</p></div>";
    }

    if (module_count == 0 && !scanning_done) {
        html += "<p class=\"note\">Modbus buszkeresés folyamatban...</p>";
    } else if (module_count == 0) {
        html += "<p class=\"note\">Nem találhatók Modbus modulok. Ellenőrizd a bekötést és a címtartományt a Beállítások oldalon.</p>";
    }

    // Global save form
    html += "<form action=\"/savemodules\" method=\"POST\">";

    for (uint16_t i = 0; i < module_count; i++) {
        Slave_Module &m = modules[i];
        String cls = m.online ? "on" : "off";
        String st = m.online ? "✅ ONLINE" : "❌ OFFLINE";

        String mqtt_name = config_get_mqtt_name(m.slave_addr);
        String ha_name = config_get_ha_name(m.slave_addr);

        html += "<div class=\"mod-card\">";
        String sim_badge = m.is_virtual ? " <span class=\"badge sensor\">SIM</span>" : "";
        html += "<div class=\"row\"><span class=\"key\">S" + String(m.slave_addr) + " " + String(m.model.model_name) + sim_badge + "</span><span class=\"val " + cls + "\">" + st + "</span></div>";
        // Virtual module: inaktív gomb
        if (m.is_virtual) {
            html += "<div style=\"margin:8px 0\"><a href=\"/togglevmod\" style=\"background:#f85149;color:white;padding:6px 14px;border-radius:4px;text-decoration:none;font-size:13px;font-weight:600\">🗑️ Virtuális modul inaktíválása</a></div>";
        }
        if (m.online) {
            html += "<div class=\"row\"><span class=\"key\">FW / SN</span><span class=\"val\">" + String(m.model.firmware_ver) + " / " + String(m.model.serial_number) + "</span></div>";
        }

        // Entity badges (only for online modules — relays are clickable)
        if (m.online) {
            // Relays — clickable toggle buttons
            html += "<div class=\"elabel\">Relék: <span class=\"note\">(kattintás = kapcsolás)</span></div><div>";
            for (uint8_t r = 0; r < HA_V2_RELAY_COUNT; r++) {
                String rcls = m.relays[r].state ? "rbtn on" : "rbtn off";
                String rtxt = "R" + String(r + 1) + (m.relays[r].state ? " ON" : " OFF");
                html += "<span id=\"r" + String(m.slave_addr) + "_" + String(r) + "\" class=\"" + rcls + "\" onclick=\"toggleRelay(" + String(m.slave_addr) + "," + String(r) + "," + String(m.relays[r].state ? 1 : 0) + ")\">" + rtxt + "</span>";
            }
            html += "</div>";

            // Digital Inputs
            html += "<div class=\"elabel\">Bemenetek:</div><div>";
            for (uint8_t d = 0; d < HA_V2_DI_COUNT; d++) {
                if (m.inputs[d].current) {
                    html += "<span class=\"badge on\">DI" + String(d + 1) + " ON</span>";
                } else {
                    html += "<span class=\"badge off\">DI" + String(d + 1) + " OFF</span>";
                }
            }
            html += "</div>";

            // Click sensors
            html += "<div class=\"elabel\">Kattintás:</div><div>";
            for (uint8_t d = 0; d < HA_V2_DI_COUNT; d++) {
                html += "<span class=\"badge sensor\">DI" + String(d + 1) + " ⚡</span>";
            }
            html += "</div>";
        }

        html += "<div class=\"mrow\">";
        html += "<div class=\"fm\"><label>MQTT név</label><input name=\"mn" + String(m.slave_addr) + "\" value=\"" + mqtt_name + "\" placeholder=\"pl. konyha_rele\"></div>";
        html += "<div class=\"fm\"><label>HA Friendly Name</label><input name=\"hn" + String(m.slave_addr) + "\" value=\"" + ha_name + "\" placeholder=\"pl. Konyha relék\"></div>";
        html += "</div>";

        // Room/Area selector
        String cur_area = config_get_module_area(m.slave_addr);
        html += "<div class=\"fm\"><label>Szoba / Helyiség</label>" + roomSelectHtml("ar" + String(m.slave_addr), cur_area) + "</div>";

        // Per-relay names (only for online modules)
        if (m.online) {
            html += "<div class=\"elabel\">Relé nevek:</div><div class=\"mrow-wrap\">";
            for (uint8_t r = 0; r < HA_V2_RELAY_COUNT; r++) {
                String rn = config_get_relay_name(m.slave_addr, r);
                html += "<div class=\"fm-sm\"><label>R" + String(r+1) + "</label><input name=\"rn" + String(m.slave_addr) + "_" + String(r) + "\" value=\"" + rn + "\" placeholder=\"Relé " + String(r+1) + "\"></div>";
            }
            html += "</div>";

            html += "<div class=\"elabel\">DI nevek:</div><div class=\"mrow-wrap\">";
            for (uint8_t d = 0; d < HA_V2_DI_COUNT; d++) {
                String dn = config_get_di_name(m.slave_addr, d);
                html += "<div class=\"fm-sm\"><label>DI" + String(d+1) + "</label><input name=\"dn" + String(m.slave_addr) + "_" + String(d) + "\" value=\"" + dn + "\" placeholder=\"DI " + String(d+1) + "\"></div>";
            }
            html += "</div>";
        }

        html += "</div>";
    }

    if (module_count > 0) {
        html += "<button type=\"submit\">&#128190; Mentés</button>";
    }
    html += "</form>";

    // ─── Szobakezelő szekció ────────────
    {
        Preferences nvrm;
        nvrm.begin(NV_NAMESPACE, true);
        String customRooms = nvrm.getString(NV_KEY_ROOMS, "");
        nvrm.end();

        html += "<div class=\"room-manage\">";
        html += "<h3>&#127968; Egyéni helyiségek</h3>";
        html += "<p class=\"note\">Az alapértelmezett 15 szoba mindig elérhető. Itt adhatsz hozzá új helyiségeket vagy törölheted a meglévőket.</p>";
        
        if (customRooms.length() > 0) {
            int s = 0, e;
            do {
                e = customRooms.indexOf('\n', s);
                String cr = (e >= 0) ? customRooms.substring(s, e) : customRooms.substring(s);
                cr.trim();
                if (cr.length() > 0) {
                    html += "<span class=\"room-tag\">" + cr + " <a href=\"/delroom?name=" + urlEncode(cr) + "\" style=\"color:#f85149;text-decoration:none;font-weight:bold\">&times;</a></span>";
                }
                s = e + 1;
            } while (e >= 0);
        } else {
            html += "<p class=\"note\" style=\"color:#484f58\">Még nincs egyéni helyiség hozzáadva.</p>";
        }
        
        html += "<div class=\"room-add\">";
        html += "<input id=\"newRoom\" placeholder=\"Új helyiség neve\">";
        html += "<button type=\"button\" onclick=\"location='/addroom?name='+encodeURIComponent(document.getElementById('newRoom').value)\">Hozzáadás</button>";
        html += "</div>";
        html += "</div>";
    }

    // ─── Scan progress auto-refresh JS ────
    html += "<script>\n";
    html += "var scanTimer=null;\n";
    html += "function updateScan(){\n";
    html += "  fetch('/scanstatus').then(r=>r.json()).then(d=>{\n";
    html += "    var bar=document.getElementById('scan-bar');\n";
    html += "    var txt=document.getElementById('scan-text');\n";
    html += "    if(bar&&d.active){\n";
    html += "      bar.style.width=d.progress+'%';\n";
    html += "      if(txt)txt.textContent='Buszkeresés: cím '+d.current+' ('+d.progress+'%)';\n";
    html += "    }\n";
    html += "    if(d.done){\n";
    html += "      if(scanTimer){clearInterval(scanTimer);scanTimer=null;}\n";
    html += "      if(bar){bar.style.width='100%';bar.style.background='#3fb950';}\n";
    html += "      if(txt)txt.textContent='Scan kész! '+d.modules+' modul találva.';\n";
    html += "      setTimeout(function(){location.reload();},1500);\n";
    html += "    }\n";
    html += "  }).catch(function(){\n";
    html += "    if(scanTimer){clearInterval(scanTimer);scanTimer=null;}\n";
    html += "  });\n";
    html += "}\n";
    html += "if(document.getElementById('scan-bar')){scanTimer=setInterval(updateScan,2000);}\n";
    html += "</script>\n";
    html += "<div class=\"foot\">Modbus-MQTT Bridge v2.0 — ESP32-S3-ETH (6DI+6R) — ESP32-S3</div>";
    html += "</body></html>";

    web.send(200, "text/html", html);
}

// ─── SAVE PINS HANDLER ─────────────────────────────────────────
static void handleSavePins() {
    if (!web_auth_ok()) return;
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);

    // Partial update — only write if present in POST (D4 bug prevention)
    if (web.hasArg("prx"))    nv.putInt(NV_KEY_PIN_RS485_RX, web.arg("prx").toInt());
    if (web.hasArg("ptx"))    nv.putInt(NV_KEY_PIN_RS485_TX, web.arg("ptx").toInt());
    if (web.hasArg("pde"))    nv.putInt(NV_KEY_PIN_RS485_DE, web.arg("pde").toInt());
    if (web.hasArg("pled"))   nv.putInt(NV_KEY_PIN_LED, web.arg("pled").toInt());
    if (web.hasArg("pbtn"))   nv.putInt(NV_KEY_PIN_BTN, web.arg("pbtn").toInt());
    if (web.hasArg("pemosi")) nv.putInt(NV_KEY_PIN_ETH_MOSI, web.arg("pemosi").toInt());
    if (web.hasArg("pemiso")) nv.putInt(NV_KEY_PIN_ETH_MISO, web.arg("pemiso").toInt());
    if (web.hasArg("pesclk")) nv.putInt(NV_KEY_PIN_ETH_SCLK, web.arg("pesclk").toInt());
    if (web.hasArg("pecs"))   nv.putInt(NV_KEY_PIN_ETH_CS, web.arg("pecs").toInt());
    if (web.hasArg("peint"))  nv.putInt(NV_KEY_PIN_ETH_INT, web.arg("peint").toInt());
    if (web.hasArg("perst"))  nv.putInt(NV_KEY_PIN_ETH_RST, web.arg("perst").toInt());

    nv.end();
    LOG_ILN("[WEB] Pin config saved");

    web.send(200, "text/html", R"rawliteral(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Pinek elmentve</title>
<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;text-align:center;padding:40px}
.ok{color:#3fb950;font-size:28px}p{margin:12px 0;color:#8b949e}</style></head>
<body><h1 class="ok">&#10004; Pinek elmentve!</h1>
<p>A GPIO pinek újraindítás után lépnek érvénybe.</p>
<p>Az eszköz újraindul 3 másodperc múlva...</p>
<script>setTimeout(function(){window.location='/'},5000)</script>
</body></html>)rawliteral");

    delay(3000);
    ESP.restart();
}

// ─── SAVE MODULES HANDLER ──────────────────────────────────────
static void handleSaveModules() {
    if (!web_auth_ok()) return;
    // Iterate all module slave addresses and save their names + area + entity names
    for (uint16_t i = 0; i < module_count; i++) {
        Slave_Module &m = modules[i];
        String mn_key = "mn" + String(m.slave_addr);
        String hn_key = "hn" + String(m.slave_addr);
        String ar_key = "ar" + String(m.slave_addr);
        String mn_val = web.arg(mn_key);
        String hn_val = web.arg(hn_key);
        String ar_val = web.arg(ar_key);
        
        config_save_module_name(m.slave_addr, mn_val.c_str(), hn_val.c_str());
        
        // Save area — if "_other", use the free text field
        if (ar_val == "_other") {
            String other_key = ar_key + "_other";
            String other_val = web.arg(other_key);
            other_val.trim();
            if (other_val.length() > 0) {
                config_save_module_area(m.slave_addr, other_val.c_str());
                // Add to custom rooms list if not already there
                Preferences pnv;
                pnv.begin(NV_NAMESPACE, false);
                String existing = pnv.getString(NV_KEY_ROOMS, "");
                pnv.end();
                bool found = false;
                if (existing.length() > 0) {
                    int s = 0, e;
                    do {
                        e = existing.indexOf('\n', s);
                        String cr = (e >= 0) ? existing.substring(s, e) : existing.substring(s);
                        cr.trim();
                        if (cr == other_val) { found = true; break; }
                        s = e + 1;
                    } while (e >= 0);
                }
                if (!found) {
                    if (existing.length() > 0) existing += "\n";
                    existing += other_val;
                    pnv.begin(NV_NAMESPACE, false);
                    pnv.putString(NV_KEY_ROOMS, existing);
                    pnv.end();
                }
            }
        } else {
            config_save_module_area(m.slave_addr, ar_val.c_str());
        }
        
        // Save per-relay names (hasArg guard: D4 fix)
        for (uint8_t r = 0; r < HA_V2_RELAY_COUNT; r++) {
            String rn_key = "rn" + String(m.slave_addr) + "_" + String(r);
            if (web.hasArg(rn_key)) {
                String rn_val = web.arg(rn_key);
                config_save_relay_name(m.slave_addr, r, rn_val.c_str());
            }
        }
        
        // Save per-DI names (hasArg guard: D4 fix)
        for (uint8_t d = 0; d < HA_V2_DI_COUNT; d++) {
            String dn_key = "dn" + String(m.slave_addr) + "_" + String(d);
            if (web.hasArg(dn_key)) {
                String dn_val = web.arg(dn_key);
                config_save_di_name(m.slave_addr, d, dn_val.c_str());
            }
        }
    }

    // Re-publish HA discovery with updated names
    if (mqtt_is_connected()) {
        for (uint16_t i = 0; i < module_count; i++) {
            if (modules[i].discovered && modules[i].online) {
                mqtt_publish_discovery(&modules[i]);
            }
        }
    }

    web.send(200, "text/html", R"rawliteral(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Modul nevek elmentve</title>
<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;text-align:center;padding:40px}
.ok{color:#3fb950;font-size:28px}p{margin:12px 0;color:#8b949e}</style></head>
<body><h1 class="ok">&#10004; Modul nevek elmentve!</h1>
<p>A modul nevek, szoba, relé/DI nevek mentve lettek.</p>
<p>HA discovery frissítve.</p>
<script>setTimeout(function(){window.location='/modules'},2000)</script>
</body></html>)rawliteral");
}

// ─── RESCAN HANDLER ──────────────────────────────────────────────
// ─── Simple URL encode helper ──────────────────────────────────
static String urlEncode(const String& s) {
    String enc;
    enc.reserve(s.length() * 3);
    for (unsigned i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            enc += c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
            enc += hex;
        }
    }
    return enc;
}

// ─── Add custom room ───────────────────────────────────────────
static void handleAddRoom() {
    if (!web_auth_ok()) return;
    String name = web.arg("name");
    name.trim();
    if (name.length() == 0) { web.sendHeader("Location","/modules"); web.send(302); return; }
    
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    String existing = nv.getString(NV_KEY_ROOMS, "");
    
    // Check duplicate
    bool dup = false;
    if (existing.length() > 0) {
        int s = 0, e;
        do {
            e = existing.indexOf('\n', s);
            String cr = (e >= 0) ? existing.substring(s, e) : existing.substring(s);
            cr.trim();
            if (cr == name) { dup = true; break; }
            s = e + 1;
        } while (e >= 0);
    }
    // Also check default rooms
    if (!dup) {
        for (auto dr : DEFAULT_ROOMS) { if (name == dr) { dup = true; break; } }
    }
    
    if (!dup) {
        // Count existing rooms
        int count = 0;
        if (existing.length() > 0) {
            int s = 0, e;
            do {
                e = existing.indexOf('\n', s);
                String cr = (e >= 0) ? existing.substring(s, e) : existing.substring(s);
                cr.trim();
                if (cr.length() > 0) count++;
                s = e + 1;
            } while (e >= 0);
        }
        if (count < MAX_ROOMS) {
            if (existing.length() > 0) existing += "\n";
            existing += name;
            nv.putString(NV_KEY_ROOMS, existing);
        }
    }
    nv.end();
    web.sendHeader("Location","/modules"); web.send(302);
}

// ─── Delete custom room ────────────────────────────────────────
static void handleDelRoom() {
    if (!web_auth_ok()) return;
    String name = web.arg("name");
    name.trim();
    if (name.length() == 0) { web.sendHeader("Location","/modules"); web.send(302); return; }
    
    // Don't allow deleting default rooms
    for (auto dr : DEFAULT_ROOMS) { if (name == dr) { web.sendHeader("Location","/modules"); web.send(302); return; } }
    
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    String existing = nv.getString(NV_KEY_ROOMS, "");
    
    String newList = "";
    if (existing.length() > 0) {
        int s = 0, e;
        do {
            e = existing.indexOf('\n', s);
            String cr = (e >= 0) ? existing.substring(s, e) : existing.substring(s);
            cr.trim();
            if (cr.length() > 0 && cr != name) {
                if (newList.length() > 0) newList += "\n";
                newList += cr;
            }
            s = e + 1;
        } while (e >= 0);
    }
    nv.putString(NV_KEY_ROOMS, newList);
    nv.end();
    web.sendHeader("Location","/modules"); web.send(302);
}

// ─── API: scan status (JSON) ────────────────────────────────────
static void handleApiScan() {
    uint16_t total = cfg.mb_scan_end - cfg.mb_scan_start + 1;
    uint16_t current = (scan_addr > cfg.mb_scan_end) ? total : (scan_addr - cfg.mb_scan_start);
    uint8_t pct = total > 0 ? (uint8_t)((current * 100) / total) : 0;
    
    String json = "{";
    json += "\"active\":" + String(scan_active ? "true" : "false") + ",";
    json += "\"done\":" + String(scanning_done ? "true" : "false") + ",";
    json += "\"current\":" + String(scan_addr) + ",";
    json += "\"start\":" + String(cfg.mb_scan_start) + ",";
    json += "\"end\":" + String(cfg.mb_scan_end) + ",";
    json += "\"progress\":" + String(pct) + ",";
    json += "\"modules\":" + String(module_count);
    json += "}";
    web.send(200, "application/json", json);
}

// ─── Toggle virtual module ON/OFF ────────────────────────────────
static void handleToggleVMod() {
    if (!web_auth_ok()) return;
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    bool cur = nv.getBool(NV_KEY_VIRTUAL_MOD, false);
    nv.putBool(NV_KEY_VIRTUAL_MOD, !cur);
    nv.end();
    
    if (!cur) {
        // Just enabled — trigger rescan to insert virtual module
        scan_modbus_start();
        scanning_done = false;
        web.sendHeader("Location","/modules"); web.send(302);
    } else {
        // Just disabled — remove virtual module from runtime and clean MQTT
        for (uint16_t i = 0; i < module_count; i++) {
            if (modules[i].is_virtual) {
                // Clean up MQTT entities for this module
                if (mqtt_is_connected()) {
                    mqtt_cleanup_discovery(&modules[i]);
                }
                // Shift remaining modules down
                for (uint16_t j = i; j < module_count - 1; j++) {
                    modules[j] = modules[j + 1];
                }
                module_count--;
                break;
            }
        }
        web.sendHeader("Location","/modules"); web.send(302);
    }
}

static void handleRescan() {
    if (!web_auth_ok()) return;
    config_clear_module_list();  // Clear saved list on rescan
    scan_modbus_start();
    scanning_done = false;
    LOG_ILN("[WEB] Rescan triggered from web UI (saved list cleared)");

    web.send(200, "text/html", R"rawliteral(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Buszkeresés</title>
<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;text-align:center;padding:40px}
.ok{color:#58a6ff;font-size:28px}p{margin:12px 0;color:#8b949e}</style></head>
<body><h1 class="ok">&#128260; Buszkeresés elindítva...</h1>
<p>A Modbus busz újraszkennelése folyamatban.</p>
<script>setTimeout(function(){window.location='/modules'},3000)</script>
</body></html>)rawliteral");
}

static void handleSaveModList() {
    if (!web_auth_ok()) return;
    config_save_module_list();
    web.send(200, "text/html", R"rawliteral(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Mentés</title>
<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;text-align:center;padding:40px}
.ok{color:#3fb950;font-size:28px}p{margin:12px 0;color:#8b949e}.note{color:#8b949e;font-size:14px;margin-top:16px}</style></head>
<body><h1 class="ok">&#128190; Modullista elmentve!</h1>
<p>A következő bootnál a bridge azonnal betölti a modulokat (scan kihagyása).</p>
<p class="note">Újrascan törli a mentett listát.</p>
<script>setTimeout(function(){window.location='/modules'},2000)</script>
</body></html>)rawliteral");
}

// ─── JSON API Endpoints ─────────────────────────────────────────
static void handleApiStatus() {
    JsonDocument doc;
    doc["hostname"] = cfg.hostname;
    doc["firmware"] = FIRMWARE_VERSION;
    doc["uptime_s"] = millis() / 1000;
    doc["heap_free"] = ESP.getFreeHeap();
    doc["heap_free_kb"] = ESP.getFreeHeap() / 1024;
    doc["interface"] = cfg.active_if == NET_IF_LAN ? "LAN" : cfg.active_if == NET_IF_WIFI ? "WiFi" : "NONE";
    doc["ip"] = active_ip;
    // Dual-stack: always show both interfaces
    doc["wifi_ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "0.0.0.0";
    doc["wifi_rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
    doc["lan_started"] = eth_is_started();
    doc["lan_connected"] = eth_is_connected();
    if (eth_is_started()) {
        doc["lan_ip"] = eth_get_ip();
    } else {
        doc["lan_ip"] = "0.0.0.0";
    }
    doc["mqtt_connected"] = mqtt_is_connected();
    doc["modules"] = module_count;
    if (module_count > 0) {
        JsonArray mods = doc["module_list"].to<JsonArray>();
        for (uint16_t i = 0; i < module_count; i++) {
            JsonObject m = mods.add<JsonObject>();
            m["addr"] = modules[i].slave_addr;
            m["online"] = modules[i].online;
            m["name"] = config_get_mqtt_name(modules[i].slave_addr);
        }
    }
    doc["mb"]["tx"] = mb_stats.tx_count;
    doc["mb"]["rx"] = mb_stats.rx_count;
    doc["mb"]["err"] = mb_stats.err_count;
    doc["mb"]["poll_ms"] = cfg.mb_poll_ms;
    // Saved module list
    Preferences nv;
    nv.begin(NV_NAMESPACE, true);
    uint8_t saved_n = nv.getUChar(NV_KEY_MOD_LIST_N, 0);
    nv.end();
    doc["saved_modules"] = saved_n;
    // TCP bridge stats
    doc["tcp"]["enabled"] = cfg.tcp_enabled;
    doc["tcp"]["req"] = tcp_get_req_count();
    doc["tcp"]["err"] = tcp_get_err_count();
    String payload;
    serializeJson(doc, payload);
    web.send(200, "application/json", payload);
}

static void handleApiConfig() {
    JsonDocument doc;
    doc["hostname"] = cfg.hostname;
    doc["wifi_ssid"] = cfg.wifi_ssid;
    doc["wifi_mode"] = cfg.wifi_mode;
    doc["lan_type"] = cfg.lan_type;
    doc["lan_enabled"] = cfg.lan_enabled;
    doc["lan_dhcp"] = cfg.lan_dhcp;
    doc["mqtt_host"] = cfg.mqtt_host;
    doc["mqtt_port"] = cfg.mqtt_port;
    doc["mqtt_user"] = cfg.mqtt_user;
    doc["mqtt_prefix"] = cfg.mqtt_prefix;
    doc["ha_discovery"] = cfg.ha_discovery;
    doc["virtual_module"] = cfg.virtual_module;
    doc["mb_profile"] = cfg.mb_profile;
    doc["mb_baud"] = cfg.mb_baud;
    doc["mb_poll_ms"] = cfg.mb_poll_ms;
    doc["mb_scan_start"] = cfg.mb_scan_start;
    doc["mb_scan_end"] = cfg.mb_scan_end;
    doc["tcp_enabled"] = cfg.tcp_enabled;
    doc["tcp_port"] = cfg.tcp_port;
    String payload;
    serializeJson(doc, payload);
    web.send(200, "application/json", payload);
}

static void handleApiModules() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (uint16_t i = 0; i < module_count; i++) {
        JsonObject m = arr.add<JsonObject>();
        m["addr"] = modules[i].slave_addr;
        m["model_id"] = modules[i].model.model_id;
        m["model_name"] = modules[i].model.model_name;
        m["online"] = modules[i].online;
        m["mqtt_name"] = config_get_mqtt_name(modules[i].slave_addr);
        m["relay_count"] = modules[i].model.RELAY_COUNT;
        m["di_count"] = modules[i].model.DI_COUNT;
        m["is_virtual"] = modules[i].is_virtual;
        // Relay states
        if (modules[i].model.RELAY_COUNT > 0) {
            JsonObject relays = m["relays"].to<JsonObject>();
            for (uint8_t r = 0; r < modules[i].model.RELAY_COUNT; r++) {
                relays[String(r)] = modules[i].relays[r].state ? "ON" : "OFF";
            }
        }
        // DI states
        if (modules[i].model.DI_COUNT > 0) {
            JsonObject inputs = m["inputs"].to<JsonObject>();
            for (uint8_t d = 0; d < modules[i].model.DI_COUNT; d++) {
                inputs[String(d)] = modules[i].inputs[d].current ? "ON" : "OFF";
            }
        }
    }
    String payload;
    serializeJson(doc, payload);
    web.send(200, "application/json", payload);
}


// ─── Full Config Export API ──────────────────────────────────
// GET /api/export — dumps ALL NVRAM config as JSON (for backup)
static void handleApiLan() {
    JsonDocument doc;
    doc["lan_enabled"] = cfg.lan_enabled;
    doc["lan_type"] = cfg.lan_type;
    doc["pin_rst"] = cfg.pin_eth_rst;
    doc["pin_cs"] = cfg.pin_eth_cs;
    doc["pin_int"] = cfg.pin_eth_int;
    doc["pin_mosi"] = cfg.pin_eth_mosi;
    doc["pin_miso"] = cfg.pin_eth_miso;
    doc["pin_sclk"] = cfg.pin_eth_sclk;
    
    int step = web.hasArg("step") ? web.arg("step").toInt() : -1;
    doc["step"] = step;
    
#ifdef USE_W5500
    if (step == 1) {
        // Step 1: Hardware reset W5500
        if (cfg.pin_eth_rst >= 0) {
            pinMode(cfg.pin_eth_rst, OUTPUT);
            digitalWrite(cfg.pin_eth_rst, LOW);
            delay(10);
            digitalWrite(cfg.pin_eth_rst, HIGH);
            delay(150);
            doc["step1"] = "RST_OK";
        } else {
            doc["step1"] = "RST_SKIP";
        }
    } else if (step == 2) {
        // Step 2: SPI bus init
        SPI.begin(cfg.pin_eth_sclk, cfg.pin_eth_miso, cfg.pin_eth_mosi, cfg.pin_eth_cs);
        doc["step2"] = "SPI_BEGIN_OK";
    } else if (step == 3) {
        // Step 3: Ethernet CS init + chip detect
        Ethernet.init(cfg.pin_eth_cs);
        doc["step3"] = "INIT_OK";
        doc["hw"] = Ethernet.hardwareStatus() == EthernetNoHardware ? "NO_HW" :
                    Ethernet.hardwareStatus() == EthernetW5500 ? "W5500" :
                    Ethernet.hardwareStatus() == EthernetW5200 ? "W5200" :
                    Ethernet.hardwareStatus() == EthernetW5100 ? "W5100" : "OTHER";
        doc["link"] = (Ethernet.linkStatus() == LinkON) ? "ON" : 
                      (Ethernet.linkStatus() == LinkOFF) ? "OFF" : "UNKNOWN";
    } else if (step == 4) {
        // Step 4: Try DHCP (5s timeout)
        int result = Ethernet.begin(lan_mac, 5000, 2000);
        doc["step4_dhcp"] = result;
        if (result == 0) {
            doc["step4_status"] = "DHCP_FAILED";
        } else {
            doc["step4_status"] = "DHCP_OK";
            doc["ip"] = Ethernet.localIP().toString();
            eth_set_connected(true);
        }
        eth_set_started(true);
        doc["hw"] = Ethernet.hardwareStatus() == EthernetNoHardware ? "NO_HW" :
                    Ethernet.hardwareStatus() == EthernetW5500 ? "W5500" : "OTHER";
        doc["link"] = (Ethernet.linkStatus() == LinkON) ? "ON" : "OFF";
    }
#else
    doc["error"] = "USE_W5500 not defined";
#endif
    
    String payload;
    serializeJson(doc, payload);
    web.send(200, "application/json", payload);
}

// ─── Config Backup / Restore ──────────────────────────────────
// Backup: reads ALL NVRAM keys → JSON (includes passwords, per-module names, rooms)
// Restore: POST JSON → writes NVRAM keys → reboot

// NVRAM keys to export — MUST match NV_KEY_* defines in modbus_mqtt_ha_bridge.h
// type: s=string, u=uint, U=UShort, B=UChar, b=bool, i=int
static const struct { const char* key; char type; } BACKUP_KEYS[] = {
    {"hostname",'s'}, {"ssid",'s'}, {"wpass",'s'}, {"apn",'s'}, {"appass",'s'},
    {"wifimode",'B'}, {"wdhcp",'b'},
    {"wip",'s'}, {"wgw",'s'}, {"wmask",'s'}, {"wdns",'s'},
    {"ethen",'b'}, {"edhcp",'b'}, {"etype",'B'},
    {"eip",'s'}, {"egw",'s'}, {"emask",'s'}, {"edns",'s'},
    {"mhost",'s'}, {"mport",'U'}, {"muser",'s'}, {"mpass",'s'}, {"mpfx",'s'},
    {"hadisc",'b'}, {"mtls",'b'},
    {"mbaud",'u'}, {"mstart",'B'}, {"mend",'B'}, {"mpar",'B'}, {"mbprof",'B'},
    {"mbcoil",'U'}, {"mbdi",'U'}, {"mbpoll",'U'}, {"vmod",'b'},
    {"tcpen",'b'}, {"tcpp",'U'},
    {"prx",'i'}, {"ptx",'i'}, {"pde",'i'},
    {"pled",'i'}, {"pbtn",'i'},
    {"pemosi",'i'}, {"pemiso",'i'}, {"pesclk",'i'},
    {"pecs",'i'}, {"peint",'i'}, {"perst",'i'},
    {"rooms",'s'},
    {"mlist_n",'B'},
    {"wauth",'b'},
    {"wauthp",'s'},
};
#define BACKUP_KEYS_COUNT (sizeof(BACKUP_KEYS)/sizeof(BACKUP_KEYS[0]))

static void handleApiBackup() {
    if (!web_auth_ok()) return;
    JsonDocument doc;
    Preferences nv;
    nv.begin(NV_NAMESPACE, true);

    doc["version"] = FIRMWARE_VERSION;
    doc["type"] = "modbus-mqtt-bridge-backup";

    for (uint16_t i = 0; i < BACKUP_KEYS_COUNT; i++) {
        const char* key = BACKUP_KEYS[i].key;
        switch (BACKUP_KEYS[i].type) {
            case 's': doc[key] = nv.getString(key, ""); break;
            case 'u': doc[key] = nv.getUInt(key, 0); break;
            case 'U': doc[key] = nv.getUShort(key, 0); break;
            case 'B': doc[key] = nv.getUChar(key, 0); break;
            case 'b': doc[key] = nv.getBool(key, false); break;
            case 'i': doc[key] = nv.getInt(key, 0); break;
        }
    }

    // Password masking — never expose real passwords in backup
    doc["wpass"] = "***";
    doc["mpass"] = "***";
    doc["wauthp"] = "***";
    doc["appass"] = "***";
    doc["passwords_masked"] = true;

    // Module list entries
    uint8_t mlist_n = nv.getUInt("mlist_n", 0);
    for (uint8_t i = 0; i < mlist_n && i < 16; i++) {
        String idx = String(i);
        doc["mlist_a" + idx] = nv.getUChar(("mlist_a" + idx).c_str(), 0);
        doc["mlist_m" + idx] = nv.getUChar(("mlist_m" + idx).c_str(), 0);
    }

    // Per-module names/area + relay/DI names (only for discovered modules)
    for (uint16_t i = 0; i < module_count; i++) {
        uint8_t addr = modules[i].slave_addr;
        String sa = String(addr);
        String mn = nv.getString(("mn" + sa).c_str(), "");
        String hn = nv.getString(("hn" + sa).c_str(), "");
        String ar = nv.getString(("ar" + sa).c_str(), "");
        if (mn.length() > 0) doc["mn" + sa] = mn;
        if (hn.length() > 0) doc["hn" + sa] = hn;
        if (ar.length() > 0) doc["ar" + sa] = ar;
        for (uint8_t r = 0; r < 6; r++) {
            String rn_val = nv.getString(("rn" + sa + "_" + String(r)).c_str(), "");
            if (rn_val.length() > 0) doc["rn" + sa + "_" + String(r)] = rn_val;
            String dn_val = nv.getString(("dn" + sa + "_" + String(r)).c_str(), "");
            if (dn_val.length() > 0) doc["dn" + sa + "_" + String(r)] = dn_val;
        }
    }

    nv.end();
    String payload;
    serializeJson(doc, payload);
    web.send(200, "application/json", payload);
}

static void handleApiRestore() {
    if (!web_auth_ok()) return;
    if (!web.hasArg("plain")) {
        web.send(400, "application/json", "{\"ok\":false,\"error\":\"no body\"}");
        return;
    }
    JsonDocument inDoc;
    DeserializationError err = deserializeJson(inDoc, web.arg("plain"));
    if (err) {
        web.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid json\"}");
        return;
    }
    if (!inDoc.containsKey("type") || strcmp(inDoc["type"], "modbus-mqtt-bridge-backup") != 0) {
        web.send(400, "application/json", "{\"ok\":false,\"error\":\"not a valid backup\"}");
        return;
    }

    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    uint16_t written = 0;

    // Build lookup from BACKUP_KEYS for type-correct writes
    for (JsonPair kv : inDoc.as<JsonObject>()) {
        const char* key = kv.key().c_str();
        if (strcmp(key, "type") == 0 || strcmp(key, "version") == 0 || 
            strcmp(key, "passwords_masked") == 0) continue;

        JsonVariant val = kv.value();

        // Skip masked passwords — don't overwrite with literal "***"
        if (val.is<const char*>() && strcmp(val.as<const char*>(), "***") == 0) continue;

        // Find type from BACKUP_KEYS table
        char ktype = 0;
        for (uint16_t i = 0; i < BACKUP_KEYS_COUNT; i++) {
            if (strcmp(BACKUP_KEYS[i].key, key) == 0) { ktype = BACKUP_KEYS[i].type; break; }
        }

        // Dynamic module keys: mlist_a/B, mlist_m/B, mn/s, hn/s, ar/s, rn/s, dn/s
        if (ktype == 0) {
            if (strncmp(key, "mlist_a", 7) == 0 || strncmp(key, "mlist_m", 7) == 0) ktype = 'B';
            else if (strncmp(key, "mn", 2) == 0 || strncmp(key, "hn", 2) == 0 ||
                     strncmp(key, "ar", 2) == 0 || strncmp(key, "rn", 2) == 0 ||
                     strncmp(key, "dn", 2) == 0) ktype = 's';
        }

        // Write using type-correct Preferences method
        if (val.is<const char*>() && (ktype == 's' || ktype == 0)) {
            nv.putString(key, val.as<const char*>());
        } else if (ktype == 'B') {
            nv.putUChar(key, (uint8_t)(val.is<int>() ? val.as<int>() : val.as<unsigned int>()));
        } else if (ktype == 'U') {
            nv.putUShort(key, (uint16_t)(val.is<int>() ? val.as<int>() : val.as<unsigned int>()));
        } else if (ktype == 'b' || val.is<bool>()) {
            nv.putBool(key, val.as<bool>());
        } else if (ktype == 'i') {
            nv.putInt(key, val.as<int>());
        } else if (ktype == 'u') {
            nv.putUInt(key, val.as<unsigned int>());
        } else if (val.is<int>()) {
            nv.putInt(key, val.as<int>());
        } else {
            continue;
        }
        written++;
    }
    nv.end();

    LOG_I("[RESTORE] %d keys written, rebooting...\n", written);
    web.send(200, "application/json",
        "{\"ok\":true,\"keys\":" + String(written) + ",\"action\":\"reboot\"}");
    delay(3000);
    ESP.restart();
}

// ─── Init & Loop ───────────────────────────────────────────────
// ─── Admin Panel ────────────────────────────────────────────────
static void handleAdmin() {
    if (!web_auth_ok()) return;

    String html;
    html.reserve(3000);
    html = R"rawliteral(<!DOCTYPE html><html lang="hu"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Modbus-MQTT Bridge — Admin</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#0d1117;color:#c9d1d9;padding:12px;font-size:15px}
h1{color:#58a6ff;font-size:1.3em;margin:8px 0}
h2{color:#f0883e;background:#161b22;padding:8px 12px;border-radius:6px;margin:16px 0 8px;font-size:1.05em}
.fm{margin-bottom:8px}
label{display:block;font-weight:600;color:#7ee787;margin-bottom:2px;font-size:14px}
input{width:100%;padding:8px;border:1px solid #30363d;border-radius:4px;background:#0d1117;color:#c9d1d9;font-size:15px}
input:focus{outline:none;border-color:#58a6ff}
.chk{display:flex;align-items:center;gap:6px;margin:6px 0}
.chk label{margin:0;font-weight:normal;color:#c9d1d9;cursor:pointer}
.chk input{width:auto;accent-color:#58a6ff}
.row{display:flex;gap:8px}
.row .fm{flex:1}
.btn{background:#238636;color:white;border:none;padding:10px 20px;border-radius:6px;font-size:16px;cursor:pointer;margin:10px 2px}
.btn:hover{background:#2ea043}
.btn.rst{background:#da3633}
.btn.rst:hover{background:#f85149}
.note{color:#8b949e;font-size:12px;margin:2px 0 8px}
.pri{background:#161b22;border:1px solid #f0883e;border-radius:6px;padding:12px;margin:8px 0}
.pri b{color:#f0883e}
.nav{display:flex;gap:8px;margin:8px 0}
.nav a{background:#21262d;color:#58a6ff;padding:8px 16px;border-radius:6px;text-decoration:none;font-size:14px}
.nav a:hover{background:#30363d}
.nav a.active{background:#238636;color:white}
.foot{text-align:center;color:#484f58;font-size:11px;margin-top:16px;border-top:1px solid #21262d;padding-top:8px}
</style></head><body>
<h1>&#128272; Admin</h1>
<div class="nav"><a href="/">Státusz</a><a href="/config">Beállítások</a><a href="/pins">Pinek</a><a href="/modules">Modulok</a><a href="/ota">OTA</a><a class="active" href="/admin">Admin</a></div>
<div class="pri"><b>&#9888; Figyelem!</b> Itt jelszavakat módosítasz. Ha elfelejted a jelszót, csak USB flash-sel lehet visszaállítani!</div>
<form action="/saveadmin" method="POST">
<h2>&#128100; Admin jelszó (Digest Auth)</h2>
<div class="chk"><label>Felhasználó: <b>admin</b> (nem módosítható)</label></div>
<div class="fm"><label>Jelenlegi jelszó</label><input type="password" name="curpass" placeholder="Írd be a jelenlegi jelszót" required></div>
<div class="row"><div class="fm"><label>Új jelszó</label><input type="password" name="newpass" placeholder="Új jelszó" required></div>
<div class="fm"><label>Megerősítés</label><input type="password" name="newpass2" placeholder="Új jelszó újra" required></div></div>
<p class="note">Min. 4 karakter. Üresen hagyva = auth kikapcsolva (nem ajánlott).</p>
<div><button type="submit" class="btn" name="action" value="changepass">Jelszó módosítása</button>
<button type="submit" class="btn rst" name="action" value="resetpass">Alapértelmezett (admin)</button></div>

<h2>&#128225; AP jelszó (WiFi Access Point)</h2>
<div class="fm"><label>AP jelszó</label><input type="password" name="appass" value=")rawliteral";

    // Show AP pass as dots (current value)
    html += String(cfg.ap_pass);
    html += R"rawliteral(" placeholder="12345678"></div>
<p class="note">Az AP jelszó a saját WiFi hálózatod védelme. Min. 8 karakter! Alapértelmezett: 12345678</p>
<div><button type="submit" class="btn" name="action" value="changeap">AP jelszó mentése</button>
<button type="submit" class="btn rst" name="action" value="resetap">Alapértelmezett (12345678)</button></div>
</form>
<div class="foot">Modbus-MQTT Bridge v)rawliteral";

    html += String(FIRMWARE_VERSION);
    html += R"rawliteral(</div></body></html>)rawliteral";

    web.send(200, "text/html", html);
}

static void handleSaveAdmin() {
    if (!web_auth_ok()) return;

    String action = web.arg("action");

    if (action == "changepass" || action == "resetpass") {
        // Change admin password
        String curpass  = web.arg("curpass");
        String newpass  = web.arg("newpass");
        String newpass2 = web.arg("newpass2");

        // Verify current password
        if (curpass != String(cfg.web_pass)) {
            web.send(403, "text/html", R"rawliteral(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Hiba</title>
<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;text-align:center;padding:40px}
.err{color:#f85149;font-size:28px}p{color:#8b949e}a{color:#58a6ff}</style></head>
<body><h1 class="err">&#10060; Hibás jelenlegi jelszó!</h1><p>A megadott jelszó nem egyezik.</p>
<a href="/admin">Vissza</a></body></html>)rawliteral");
            return;
        }

        if (action == "resetpass") {
            // Reset to factory default
            strlcpy(cfg.web_pass, "admin", sizeof(cfg.web_pass));
        } else {
            // Validate new password
            if (newpass != newpass2) {
                web.send(400, "text/html", R"rawliteral(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Hiba</title>
<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;text-align:center;padding:40px}
.err{color:#f85149;font-size:28px}p{color:#8b949e}a{color:#58a6ff}</style></head>
<body><h1 class="err">&#10060; A két jelszó nem egyezik!</h1><p>Próbáld újra.</p>
<a href="/admin">Vissza</a></body></html>)rawliteral");
                return;
            }
            if (newpass.length() < 4) {
                web.send(400, "text/html", R"rawliteral(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Hiba</title>
<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;text-align:center;padding:40px}
.err{color:#f85149;font-size:28px}p{color:#8b949e}a{color:#58a6ff}</style></head>
<body><h1 class="err">&#10060; Túl rövid jelszó!</h1><p>Min. 4 karakter szükséges.</p>
<a href="/admin">Vissza</a></body></html>)rawliteral");
                return;
            }
            strlcpy(cfg.web_pass, newpass.c_str(), sizeof(cfg.web_pass));
        }

        // Save to NVRAM
        Preferences nv;
        nv.begin(NV_NAMESPACE, false);
        nv.putString(NV_KEY_WEB_PASS, cfg.web_pass);
        nv.end();

        LOG_ILN("[ADMIN] Web auth password changed");
        web.send(200, "text/html", R"rawliteral(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Mentve</title>
<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;text-align:center;padding:40px}
.ok{color:#3fb950;font-size:28px}p{color:#8b949e}a{color:#58a6ff}</style></head>
<body><h1 class="ok">&#10004; Admin jelszó elmentve!</h1><p>A változás azonnal érvényes.</p>
<a href="/admin">Vissza</a></body></html>)rawliteral");
        return;
    }

    if (action == "changeap" || action == "resetap") {
        // Change AP password
        if (action == "resetap") {
            strlcpy(cfg.ap_pass, "12345678", sizeof(cfg.ap_pass));
        } else {
            String appass = web.arg("appass");
            if (appass.length() < 8) {
                web.send(400, "text/html", R"rawliteral(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Hiba</title>
<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;text-align:center;padding:40px}
.err{color:#f85149;font-size:28px}p{color:#8b949e}a{color:#58a6ff}</style></head>
<body><h1 class="err">&#10060; Túl rövid AP jelszó!</h1><p>Min. 8 karakter szükséges a WPA2 miatt.</p>
<a href="/admin">Vissza</a></body></html>)rawliteral");
                return;
            }
            strlcpy(cfg.ap_pass, appass.c_str(), sizeof(cfg.ap_pass));
        }

        // Update AP immediately
        String ap_name = cfg.ap_name[0] ? String(cfg.ap_name) : String(cfg.hostname) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
        WiFi.softAP(ap_name.c_str(), cfg.ap_pass);

        // Save to NVRAM
        Preferences nv;
        nv.begin(NV_NAMESPACE, false);
        nv.putString(NV_KEY_AP_PASS, cfg.ap_pass);
        nv.end();

        LOG_ILN("[ADMIN] AP password changed, softAP updated");
        web.send(200, "text/html", R"rawliteral(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Mentve</title>
<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;text-align:center;padding:40px}
.ok{color:#3fb950;font-size:28px}p{color:#8b949e}a{color:#58a6ff}</style></head>
<body><h1 class="ok">&#10004; AP jelszó elmentve!</h1><p>Az AP azonnal újraindult az új jelszóval.</p>
<a href="/admin">Vissza</a></body></html>)rawliteral");
        return;
    }

    web.send(400, "text/html", "Bad request");
}

void web_server_init() {
    web.on("/", HTTP_GET, handleStatus);
    web.on("/scanstatus", HTTP_GET, handleApiScan);
    web.on("/config", HTTP_GET, handleConfig);
    web.on("/pins", HTTP_GET, handlePins);
    web.on("/modules", HTTP_GET, handleModules);
    web.on("/admin", HTTP_GET, handleAdmin);
    web.on("/saveadmin", HTTP_POST, handleSaveAdmin);
    web.on("/ota", HTTP_GET, handleOtaPage);
    web.on("/otaupload", HTTP_POST, handleOtaUpload, handleOtaUpload);
    web.on("/save", HTTP_POST, handleSave);
    web.on("/savepins", HTTP_POST, handleSavePins);
    web.on("/savemodules", HTTP_POST, handleSaveModules);
    web.on("/savemodlist", HTTP_POST, handleSaveModList);
    web.on("/addroom", HTTP_GET, handleAddRoom);
    web.on("/delroom", HTTP_GET, handleDelRoom);
    web.on("/togglevmod", HTTP_GET, handleToggleVMod);
    web.on("/relay", HTTP_GET, handleRelay);
    web.on("/rescan", HTTP_POST, handleRescan);
    web.on("/restart", HTTP_GET, handleRestart);
    web.on("/api/backup", HTTP_GET, handleApiBackup);
    web.on("/api/restore", HTTP_POST, handleApiRestore);
    // Aliases for clarity: /api/export = /api/backup, /api/import = /api/restore
    web.on("/api/export", HTTP_GET, handleApiBackup);
    web.on("/api/import", HTTP_POST, handleApiRestore);
    // ── JSON API endpoints (scripting-friendly) ───────────────
    web.on("/api/status", HTTP_GET, handleApiStatus);
    web.on("/api/config", HTTP_GET, handleApiConfig);
    web.on("/api/modules", HTTP_GET, handleApiModules);
    web.on("/api/lan", HTTP_GET, handleApiLan);
    web.begin();
    LOG_ILN("[WEB] Status & Config server started on port 80");
}

void web_server_loop() {
    web.handleClient();
    eth_web_loop();  // W5500 Ethernet web server
}