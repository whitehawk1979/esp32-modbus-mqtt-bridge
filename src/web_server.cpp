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
#include <algorithm>
#include <esp_task_wdt.h>
#include "web_templates.h"
#include "web_adapter.h"
#ifdef USE_W5500
#include <SPI.h>
#include <Ethernet.h>
// eth_mac is defined static in eth_handler.cpp — use same MAC here
static byte lan_mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
#endif
#include <Preferences.h>
#include <ArduinoJson.h>
#include "modbus_mqtt_ha_bridge.h"

// ─── Forward declarations from ota_handler.cpp ──────────────
void handleOtaPage();
void handleOtaUpload();
void handleOtaFromURL();
void handleApiOtaRaw();

WebServer web(80);

// ─── Web Adapter — allows same handler functions on WiFi and LAN ──
WiFiWebAdapter wifiAdapter(web);
WebInterface *WS = &wifiAdapter; // default: WiFi

#ifdef USE_W5500
EthWebServer ethWeb(80);
EthWebAdapter lanAdapter(ethWeb);
#endif

// ─── HTML Escape — XSS Prevention ──────────────────────────────
// Escapes user-controlled strings before embedding in HTML attributes
// or text content.  Covers: & < > " '
static String htmlEscape(const String &s)
{
    String out;
    out.reserve(s.length() + 16); // modest pre-alloc
    for (size_t i = 0; i < s.length(); i++)
    {
        char c = s.charAt(i);
        switch (c)
        {
        case '&':  out += F("&amp;");  break;
        case '<':  out += F("&lt;");   break;
        case '>':  out += F("&gt;");   break;
        case '"':  out += F("&quot;"); break;
        case '\'': out += F("&#39;");  break;
        default:   out += c;          break;
        }
    }
    return out;
}

// ─── Web Authentication Helper ─────────────────────────────────
// Check if web authentication is required and validate
// ─── Auth Rate Limiting ──────────────────────────────────────────
#define AUTH_MAX_ATTEMPTS 5
#define AUTH_LOCKOUT_MS 30000 // 30s lockout after 5 failed attempts

static uint8_t auth_fail_count = 0;
static uint32_t auth_lockout_until = 0;
static uint32_t auth_last_fail = 0;

// Forward declaration (defined after web_auth_ok)
static bool api_rate_ok();

bool web_auth_ok()
{
    if (!cfg.web_auth || strlen(cfg.web_pass) == 0)
        return true; // Auth disabled

    // ── Rate limiting ───────────────────────────────────────
    uint32_t now = millis();
    if (auth_lockout_until > 0 && now < auth_lockout_until)
    {
        WS->send(429, "text/plain", "Too Many Attempts — try again later");
        return false;
    }
    // Reset counter if lockout expired
    if (auth_lockout_until > 0 && now >= auth_lockout_until)
    {
        auth_fail_count = 0;
        auth_lockout_until = 0;
    }
    // Reset counter if last fail was >5 min ago (natural cooldown)
    if (auth_fail_count > 0 && now - auth_last_fail > 300000)
    {
        auth_fail_count = 0;
    }

    if (!WS->authenticate("admin", cfg.web_pass))
    {
        // ── Fallback: query-param auth for non-browser clients (curl, scripts) ──
        if (WS->hasArg("auth") && WS->arg("auth") == String(cfg.web_pass))
        {
            // Query-param auth OK — skip Digest challenge
            auth_fail_count = 0;
            auth_lockout_until = 0;
            return true;
        }

        auth_fail_count++;
        auth_last_fail = millis();

        if (auth_fail_count >= AUTH_MAX_ATTEMPTS)
        {
            auth_lockout_until = millis() + AUTH_LOCKOUT_MS;
            LOG_E("[AUTH] ⚠ LOCKOUT: %u failed attempts, blocked for %us\n", auth_fail_count, AUTH_LOCKOUT_MS / 1000);
            WS->send(429, "text/plain", "Too Many Attempts — locked for 30s");
            return false;
        }

        LOG_E("[AUTH] Failed attempt %u/%u from %s\n",
              auth_fail_count,
              AUTH_MAX_ATTEMPTS,
              WS->clientIP().toString().c_str());
        WS->requestAuthentication();
        return false;
    }

    // Successful auth — reset counters
    auth_fail_count = 0;
    auth_lockout_until = 0;

    // API rate limiting check (per-IP, 30 req/min)
    if (!api_rate_ok())
        return false;

    return true;
}

// ─── API Rate Limiting (per-IP) ─────────────────────────────────
#define API_RATE_MAX 30       // max requests per window
#define API_RATE_WINDOW 60000 // 60s sliding window
#define API_RATE_IPS  4       // track max 4 IPs (RAM-constrained)

struct api_rate_entry
{
    uint32_t ip;        // client IP as uint32
    uint8_t count;      // requests in current window
    uint32_t window_start; // millis() at window start
};

static api_rate_entry api_rate_table[API_RATE_IPS] = {};
static uint8_t api_rate_next = 0; // round-robin index

static bool api_rate_ok()
{
    uint32_t now = millis();
    uint32_t client_ip = WS->clientIP();
    
    // Find existing entry or free slot
    int8_t slot = -1;
    for (uint8_t i = 0; i < API_RATE_IPS; i++)
    {
        if (api_rate_table[i].ip == client_ip)
        {
            slot = i;
            break;
        }
    }
    
    if (slot < 0)
    {
        // New IP — find oldest or use round-robin
        slot = api_rate_next;
        api_rate_next = (api_rate_next + 1) % API_RATE_IPS;
        api_rate_table[slot].ip = client_ip;
        api_rate_table[slot].count = 0;
        api_rate_table[slot].window_start = now;
    }
    
    // Reset window if expired
    if (now - api_rate_table[slot].window_start > API_RATE_WINDOW)
    {
        api_rate_table[slot].count = 0;
        api_rate_table[slot].window_start = now;
    }
    
    api_rate_table[slot].count++;
    
    if (api_rate_table[slot].count > API_RATE_MAX)
    {
        LOG_I("[RATE] IP %s rate limited (%u req/%us)\n",
              WS->clientIP().toString().c_str(),
              api_rate_table[slot].count,
              API_RATE_WINDOW / 1000);
        WS->send(429, "application/json", "{\"error\":\"rate_limited\",\"retry_after\":60}");
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
static void handleRelay()
{
    if (!web_auth_ok())
        return;
    if (!WS->hasArg("addr") || !WS->hasArg("relay") || !WS->hasArg("state"))
    {
        WS->send(400, "application/json", "{\"ok\":false,\"error\":\"missing params\"}");
        return;
    }
    uint8_t addr = (uint8_t)WS->arg("addr").toInt();
    uint8_t relay = (uint8_t)WS->arg("relay").toInt();
    bool state = WS->arg("state").toInt() != 0;

    // Find module
    Slave_Module *mod = nullptr;
    for (uint16_t i = 0; i < module_count; i++)
    {
        if (modules[i].slave_addr == addr)
        {
            mod = &modules[i];
            break;
        }
    }
    if (!mod)
    {
        WS->send(404, "application/json", "{\"ok\":false,\"error\":\"module not found\"}");
        return;
    }

    bool ok;
    if (mod->is_virtual)
    {
        // Virtual module: just flip internal state + publish
        mod->relays[relay].state = state;
        mod->relays[relay].published = true;
        mqtt_publish_relay_state(mod, relay);
        LOG_I("[WEB] Virtual S%d R%d → %s\n", addr, relay + 1, state ? "ON" : "OFF");
        ok = true;
    }
    else if (!mod->online)
    {
        WS->send(409, "application/json", "{\"ok\":false,\"error\":\"module offline\"}");
        return;
    }
    else
    {
        // Physical module: FC05 Modbus write + read-back + MQTT publish
        ok = modbus_write_coil(addr, relay, state);
        if (ok)
        {
            mqtt_publish_relay_state(mod, relay);
            LOG_I("[WEB] S%d R%d → %s (Modbus OK)\n", addr, relay + 1, state ? "ON" : "OFF");
        }
        else
        {
            LOG_E("[WEB] S%d R%d write FAILED\n", addr, relay + 1);
        }
    }

    if (ok)
    {
        String resp = "{\"ok\":true,\"addr\":" + String(addr) + ",\"relay\":" + String(relay) +
                      ",\"state\":" + String(state ? 1 : 0) + "}";
        WS->send(200, "application/json", resp);
    }
    else
    {
        WS->send(500, "application/json", "{\"ok\":false,\"error\":\"modbus write failed\"}");
    }
}

// ─── Handle DI toggle (virtual module) ──────────────────────
// GET /di?addr=200&di=0&state=1  (state: 1=ON, 0=OFF)
static void handleDI()
{
    if (!web_auth_ok())
        return;
    if (!WS->hasArg("addr") || !WS->hasArg("di") || !WS->hasArg("state"))
    {
        WS->send(400, "application/json", "{\"ok\":false,\"error\":\"missing params\"}");
        return;
    }
    uint8_t addr = (uint8_t)WS->arg("addr").toInt();
    uint8_t di = (uint8_t)WS->arg("di").toInt();
    bool state = WS->arg("state").toInt() != 0;

    Slave_Module *mod = nullptr;
    for (uint16_t i = 0; i < module_count; i++)
    {
        if (modules[i].slave_addr == addr)
        {
            mod = &modules[i];
            break;
        }
    }
    if (!mod)
    {
        WS->send(404, "application/json", "{\"ok\":false,\"error\":\"module not found\"}");
        return;
    }
    if (!mod->is_virtual)
    {
        WS->send(400, "application/json", "{\"ok\":false,\"error\":\"DI toggle only for virtual module\"}");
        return;
    }

    mod->inputs[di].current = state;
    mod->inputs[di].published = true;
    mqtt_publish_di_state(mod, di, state);
    LOG_I("[WEB] Virtual S%d DI%d → %s\n", addr, di + 1, state ? "ON" : "OFF");

    // ── DI→Edge mapping for virtual module (v2.8+) ──
    if (state) // Rising edge (pressed)
    {
        di_execute_edge_action(mod, di, mod->di_edge_map[di].rising_action, true);
    }
    else // Falling edge (released)
    {
        di_execute_edge_action(mod, di, mod->di_edge_map[di].falling_action, true);
    }
    // Legacy di_relay_map: toggle on any change (backward compat, only if edge map not active)
    if (mod->di_edge_map[di].relay == 0xFF)
    {
        uint8_t mapped = mod->di_relay_map[di];
        if (mapped != 0xFF && mapped < HA_V2_RELAY_COUNT)
        {
            bool new_relay = !mod->relays[mapped].state;
            mod->relays[mapped].state = new_relay;
            mod->relays[mapped].published = false;
            // Virtual module: no Modbus write, just update state + MQTT publish
            mqtt_publish_relay_state(mod, mapped);
            LOG_I("[DI→RELAY] S%d DI%d→R%d: %s (virtual, legacy)\n",
                  mod->slave_addr, di + 1, mapped + 1,
                  new_relay ? "ON" : "OFF");
        }
    }

    String resp = "{\"ok\":true,\"addr\":" + String(addr) + ",\"di\":" + String(di) +
                  ",\"state\":" + String(state ? 1 : 0) + "}";
    WS->send(200, "application/json", resp);
}

// ─── Helpers ───────────────────────────────────────────────────
static String uptimeStr()
{
    uint32_t s = millis() / 1000;
    uint16_t d = s / 86400;
    s %= 86400;
    uint16_t h = s / 3600;
    s %= 3600;
    uint16_t m = s / 60;
    s %= 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%dd %02dh %02dm %02ds", d, h, m, (uint16_t)s);
    return String(buf);
}

static String ifName(NetInterface n)
{
    switch (n)
    {
    case NET_IF_LAN:
        return "LAN &#9889;";
    case NET_IF_WIFI:
        return "WiFi &#128225;";
    default:
        return "Nincs &#10060;";
    }
}

static String ifColor(NetInterface n)
{
    switch (n)
    {
    case NET_IF_LAN:
        return "#3fb950"; // green
    case NET_IF_WIFI:
        return "#f0883e"; // orange
    default:
        return "#f85149"; // red
    }
}

static String profileName(uint8_t p)
{
    switch (p)
    {
    case MB_PROFILE_KC868_HA:
        return "KC868-HA V2";
    case MB_PROFILE_GENERIC:
        return "Generic";
    case MB_PROFILE_NIBE:
        return "NIBE S1156-18";
    case MB_PROFILE_SABIANA:
        return "Sabiana";
    case MB_PROFILE_CUSTOM:
        return "Egyedi";
    default:
        return "?";
    }
}

static String urlEncode(const String &); // forward

// ─── Room list helpers ─────────────────────────────────────────
static const char *DEFAULT_ROOMS[] = {"Nappali",
                                      "Hálószoba",
                                      "Konyha",
                                      "Fürdőszoba",
                                      "Előszoba",
                                      "Gyerekszoba",
                                      "Dolgozószoba",
                                      "Mosókonyha",
                                      "Garázs",
                                      "Pince",
                                      "Terasz",
                                      "Folyosó",
                                      "Lépcsőház",
                                      "Étterem",
                                      "Raktár"};
#define DEFAULT_ROOM_COUNT 15

// Build room <select> HTML for a module. cur_area = current saved value.
static String roomSelectHtml(const String &field_name, const String &cur_area)
{
    String h = "<select name=\"" + field_name + "\" onchange=\"roomChanged(this)\">";
    h += "<option value=\"\"" + String(cur_area.length() == 0 ? " selected" : "") + ">— nincs megadva —</option>";

    // Default rooms
    for (auto rm : DEFAULT_ROOMS)
    {
        h += "<option value=\"" + htmlEscape(rm) + "\"" + String(cur_area == rm ? " selected" : "") + ">" + htmlEscape(rm) +
             "</option>";
    }

    // Custom rooms from NVRAM
    Preferences nv;
    nv.begin(NV_NAMESPACE, true);
    String customRooms = nv.getString(NV_KEY_ROOMS, "");
    nv.end();

    if (customRooms.length() > 0)
    {
        int start = 0, end;
        do
        {
            end = customRooms.indexOf('\n', start);
            String rm = (end >= 0) ? customRooms.substring(start, end) : customRooms.substring(start);
            rm.trim();
            if (rm.length() > 0)
            {
                h += "<option value=\"" + htmlEscape(rm) + "\"" + String(cur_area == rm ? " selected" : "") + ">" + htmlEscape(rm) +
                     "</option>";
            }
            start = end + 1;
        } while (end >= 0);
    }

    // "Egyéb" — selected if cur_area is non-empty but not in any list
    bool is_default = false;
    is_default = std::any_of(
            DEFAULT_ROOMS, DEFAULT_ROOMS + DEFAULT_ROOM_COUNT, [&](const char *dr) { return cur_area == dr; });
    bool is_custom = false;
    if (!is_default && customRooms.length() > 0)
    {
        int s = 0, e;
        do
        {
            e = customRooms.indexOf('\n', s);
            String cr = (e >= 0) ? customRooms.substring(s, e) : customRooms.substring(s);
            cr.trim();
            if (cr == cur_area)
            {
                is_custom = true;
                break;
            }
            s = e + 1;
        } while (e >= 0);
    }
    bool show_other_selected = (cur_area.length() > 0 && !is_default && !is_custom);
    h += "<option value=\"_other\"" + String(show_other_selected ? " selected" : "") + ">Egyéb...</option>";
    h += "</select>";

    // Free text input shown only when "Egyéb" selected
    String other_val = show_other_selected ? cur_area : "";
    h += "<input class=\"room-other\" name=\"" + field_name + "_other\" value=\"" + htmlEscape(other_val) +
         "\" placeholder=\"Helyiség neve\" style=\"display:" + String(show_other_selected ? "block" : "none") +
         ";margin-top:4px\">";

    return h;
}

// ─── STATUS PAGE ───────────────────────────────────────────────
static void handleStatus()
{
    if (!web_auth_ok())
        return;
    String html;
    html.reserve(6000); // Pre-allocate to reduce heap fragmentation
    html = pageStart(F("Modbus-MQTT Bridge — Státusz"), CSS_STATUS) + pageStyleEnd();

    html += "<h1>&#9889; " + htmlEscape(cfg.hostname) + "</h1>";
    html += navHtml(PG_STATUS, WS->hasArg("auth") ? ("?auth=" + WS->arg("auth")) : "");

    // Network — Dual stack display (both interfaces always visible)
    html += "<h2>&#128279; Hálózat</h2><div class=\"card\">";

    // LAN status
    bool lan_up = eth_is_connected();
    bool lan_on = eth_is_started();
    html += "<div class=\"row\"><span class=\"key\">LAN</span><span class=\"val " + String(lan_up ? "on" : "off") +
            "\">" +
            String(lan_up   ? "CSATLAKOZVA ✅"
                   : lan_on ? "Keresés... ⏳"
                            : "LEÁLLÍTVA ⚫") +
            "</span></div>";
    if (lan_on)
    {
        html += "<div class=\"row\"><span class=\"key\">LAN IP</span><span class=\"val\">" + eth_get_ip() +
                "</span></div>";
    }
    html += "<div class=\"row\"><span class=\"key\">LAN DHCP</span><span class=\"val\">" +
            String(cfg.lan_dhcp ? "Igen" : "Nem (statikus)") + "</span></div>";

    // WiFi status
    bool wifi_up = (WiFi.status() == WL_CONNECTED);
    html += "<div class=\"row\"><span class=\"key\">WiFi</span><span class=\"val " + String(wifi_up ? "on" : "off") +
            "\">" + String(wifi_up ? "CSATLAKOZVA ✅" : "LECSATLAKOZVA ❌") + "</span></div>";
    if (wifi_up)
    {
        html += "<div class=\"row\"><span class=\"key\">WiFi IP</span><span class=\"val\">" +
                WiFi.localIP().toString() + "</span></div>";
        html += "<div class=\"row\"><span class=\"key\">RSSI</span><span class=\"val\">" + String(WiFi.RSSI()) +
                " dBm</span></div>";
    }
    html += "<div class=\"row\"><span class=\"key\">WiFi DHCP</span><span class=\"val\">" +
            String(cfg.wifi_dhcp ? "Igen" : "Nem (statikus)") + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">WiFi mód</span><span class=\"val\">" +
            String(cfg.wifi_mode == 0 ? "AP+STA (mindig elérhető)" : "Csak STA") + "</span></div>";

    // Active interface indicator
    html += "<div class=\"row\"><span class=\"key\">Aktív elsődleges</span><span class=\"val\" style=\"color:" +
            ifColor(cfg.active_if) + "\">" + ifName(cfg.active_if) + "</span></div>";

    // AP status
    bool ap_active = (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA);
    html += "<div class=\"row\"><span class=\"key\">WiFi AP</span><span class=\"val " +
            String(ap_active ? "on" : "off") + "\">" + String(ap_active ? "AKTÍV ✅" : "KI ❌") + "</span></div>";
    if (ap_active)
    {
        String live_ap = cfg.ap_name[0] ? htmlEscape(cfg.ap_name)
                                        : htmlEscape(cfg.hostname) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
        html += "<div class=\"row\"><span class=\"key\">AP Név</span><span class=\"val\">" + live_ap + "</span></div>";
        html += "<div class=\"row\"><span class=\"key\">AP IP</span><span class=\"val\">" + WiFi.softAPIP().toString() +
                "</span></div>";
    }
    html += "</div>";

    // Strategy info
    html += "<div class=\"card\" style=\"border-color:#f0883e\"><div class=\"row\"><span class=\"key\">&#128279; "
            "Stratégia</span><span class=\"val warn\">LAN elsődleges → WiFi auto-fallback → LAN "
            "auto-visszaváltás</span></div></div>";

    // MQTT
    html += "<h2>&#128172; MQTT</h2><div class=\"card\">";
    bool mc = mqtt_is_connected();
    html += "<div class=\"row\"><span class=\"key\">Állapot</span><span class=\"val " + String(mc ? "on" : "off") +
            "\">" + String(mc ? "CSATLAKOZVA ✅" : "NEM CSATLAKOZOTT ❌") + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Broker</span><span class=\"val\">" + htmlEscape(cfg.mqtt_host) + ":" +
            String(cfg.mqtt_port) + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">TLS</span><span class=\"val " +
            String(cfg.mqtt_tls ? "on" : "off") + "\">" + String(cfg.mqtt_tls ? "BE ✅" : "KI ❌") + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Prefix</span><span class=\"val\">" + htmlEscape(cfg.mqtt_prefix) +
            "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">HA Discovery</span><span class=\"val " +
            String(cfg.ha_discovery ? "on" : "off") + "\">" + String(cfg.ha_discovery ? "BE ✅" : "KI ❌") +
            "</span></div>";
    html += "</div>";

    // Modbus modules
    html += "<h2>&#128268; Modbus Modulok</h2><div class=\"card\">";
    html += "<div class=\"row\"><span class=\"key\">Modulok száma</span><span class=\"val\">" + String(module_count) +
            "</span></div>";
    // Show saved module list indicator
    {
        Preferences nv;
        nv.begin(NV_NAMESPACE, true);
        uint8_t saved_n = nv.getUChar(NV_KEY_MOD_LIST_N, 0);
        nv.end();
        if (saved_n > 0)
        {
            html += "<div class=\"row\"><span class=\"key\">Mentett modullista</span><span class=\"val on\">✅ " +
                    String(saved_n) + " modul (gyors boot)</span></div>";
        }
        else
        {
            html += "<div class=\"row\"><span class=\"key\">Mentett modullista</span><span class=\"val off\">❌ Nincs "
                    "(scan minden bootnál)</span></div>";
        }
    }
    html += "<div class=\"row\"><span class=\"key\">Cím tartomány</span><span class=\"val\">" +
            String(cfg.mb_scan_start) + " - " + String(cfg.mb_scan_end) + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Baud / Paritás</span><span class=\"val\">" + String(cfg.mb_baud) +
            " / " +
            String(cfg.mb_parity == 0   ? "8N1"
                   : cfg.mb_parity == 1 ? "8E1"
                                        : "8O1") +
            "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Profil</span><span class=\"val\">" + profileName(cfg.mb_profile) +
            "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Polling időköz</span><span class=\"val\">" +
            String(cfg.mb_poll_ms) + " ms</span></div>";

    // Per-module status
    for (uint16_t i = 0; i < module_count; i++)
    {
        Slave_Module &m = modules[i];
        String cls = m.online ? "on" : "off";
        String st = m.online ? "✅ ONLINE" : "❌ OFFLINE";
        String sim_tag = m.is_virtual ? " 🔹" : "";
        html += "<div class=\"row\" style=\"margin-top:6px;padding-top:6px;border-top:1px solid #30363d\"><span "
                "class=\"key\">S" +
                String(m.slave_addr) + " " + String(m.model.model_name) + sim_tag + "</span><span class=\"val " + cls +
                "\">" + st + "</span></div>";
        if (m.online)
        {
            html += "<div class=\"row\"><span class=\"key\">└ FW / SN</span><span class=\"val\">" +
                    String(m.model.firmware_ver) + " / " + String(m.model.serial_number) + "</span></div>";
        }
    }
    html += "</div>";

    // Modbus Statistics
    html += "<h2>&#128202; Modbus Statisztika</h2><div class=\"card\">";
    html += "<div class=\"row\"><span class=\"key\">TX (kérés)</span><span class=\"val\">" + String(mb_stats.tx_count) +
            "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">RX (válasz)</span><span class=\"val\">" +
            String(mb_stats.rx_count) + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Hibák</span><span class=\"val " +
            String(mb_stats.err_count > 0 ? "warn" : "") + "\">" + String(mb_stats.err_count) + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Utolsó hibakód</span><span class=\"val\">" +
            String(mb_stats.last_err_code) + "</span></div>";
    if (mb_stats.last_err_time > 0)
    {
        uint32_t since = (millis() - mb_stats.last_err_time) / 1000;
        html += "<div class=\"row\"><span class=\"key\">Utolsó hiba</span><span class=\"val\">" + String(since) +
                " mp ezelőtt</span></div>";
    }
    else
    {
        html += "<div class=\"row\"><span class=\"key\">Utolsó hiba</span><span class=\"val\">Nincs</span></div>";
    }
    html += "<div class=\"row\"><span class=\"key\">Utolsó címzett slave</span><span class=\"val\">S" +
            String(mb_stats.last_slave_addr) + "</span></div>";
    html += "</div>";

    // TCP Bridge
    html += "<h2>&#128272; TCP Bridge</h2><div class=\"card\">";
    html += "<div class=\"row\"><span class=\"key\">Állapot</span><span class=\"val " +
            String(cfg.tcp_enabled ? "on" : "off") + "\">" + String(cfg.tcp_enabled ? "BE ✅" : "KI ❌") +
            "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Port</span><span class=\"val\">" + String(cfg.tcp_port) +
            "</span></div>";
    html += "</div>";

    // System
    html += "<h2>&#9881; Rendszer</h2><div class=\"card\">";
    html += "<div class=\"row\"><span class=\"key\">Hostname</span><span class=\"val\">" + String(cfg.hostname) +
            "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Firmware</span><span class=\"val\">v" + String(FIRMWARE_VERSION) +
            "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Uptime</span><span class=\"val\">" + uptimeStr() + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Heap free</span><span class=\"val\">" +
            String(ESP.getFreeHeap() / 1024) + " KB</span></div>";
    html += "<div class=\"row\"><span class=\"key\">WDT reboots</span><span class=\"val\">" +
            String(wdt_get_reboots()) + "</span></div>";
    // Bridge system sensors
    {
        bool wifi_connected = (WiFi.status() == WL_CONNECTED);
        html += "<div class=\"row\"><span class=\"key\">WiFi RSSI</span><span class=\"val\">" +
                (wifi_connected ? String(WiFi.RSSI()) + " dBm" : String("—")) + "</span></div>";
        html += "<div class=\"row\"><span class=\"key\">Szabad memória</span><span class=\"val\">" +
                String(ESP.getFreeHeap()) + " B</span></div>";
        const char *net_if_str = cfg.active_if == NET_IF_LAN ? "LAN" : cfg.active_if == NET_IF_WIFI ? "WiFi" : "NONE";
        String net_detail = String(net_if_str);
        if (cfg.active_if == NET_IF_LAN && eth_is_started())
            net_detail += " (" + eth_get_ip() + ")";
        else if (cfg.active_if == NET_IF_WIFI && WiFi.status() == WL_CONNECTED)
            net_detail += " (" + WiFi.localIP().toString() + ")";
        html += "<div class=\"row\"><span class=\"key\">Hálózat</span><span class=\"val\" style=\"color:" +
                ifColor(cfg.active_if) + "\">" + net_detail + "</span></div>";
        // Debug: raw update_network() values
        bool _wifi_ok = (WiFi.status() == WL_CONNECTED);
        html += "<div class=\"row\"><span class=\"key\">[DBG] net_connected</span><span class=\"val\">" +
                String(net_connected ? "true" : "false") + "</span></div>";
        html += "<div class=\"row\"><span class=\"key\">[DBG] wifi_ok</span><span class=\"val\">" +
                String(_wifi_ok ? "true" : "false") + "</span></div>";
        html += "<div class=\"row\"><span class=\"key\">[DBG] lan_enabled</span><span class=\"val\">" +
                String(cfg.lan_enabled ? "true" : "false") + "</span></div>";
        html += "<div class=\"row\"><span class=\"key\">[DBG] active_if</span><span class=\"val\">" +
                String(cfg.active_if) + "</span></div>";
        // MQTT debug
        html += "<div class=\"row\"><span class=\"key\">[DBG] mqtt_host</span><span class=\"val\">" +
                String(cfg.mqtt_host[0] ? cfg.mqtt_host : "empty") + "</span></div>";
        html += "<div class=\"row\"><span class=\"key\">[DBG] mqtt_connected</span><span class=\"val\">" +
                String(mqtt_is_connected() ? "true" : "false") + "</span></div>";
    }
    html += "<div class=\"row\"><span class=\"key\">Flash használat</span><span class=\"val\">" +
            String((ESP.getSketchSize() * 100) / ESP.getFlashChipSize()) + "%</span></div>";
#ifdef USE_STORAGE
    {
        uint64_t st_total = storage_total_bytes();
        uint64_t st_used  = storage_used_bytes();
        uint32_t st_total_kb = (uint32_t)(st_total / 1024);
        uint32_t st_used_kb  = (uint32_t)(st_used / 1024);
        uint8_t st_pct = st_total > 0 ? (uint8_t)((st_used * 100) / st_total) : 0;
        html += "<div class=\"row\"><span class=\"key\">Flash Storage (LittleFS)</span><span class=\"val\">" +
                String(st_used_kb) + " / " + String(st_total_kb) + " KB (" + String(st_pct) + "%)</span></div>";
        html += "<div class=\"row\"><span class=\"key\"> /profiles /active</span><span class=\"val\"><a href=\"/storage" +
                (WS->hasArg("auth") ? ("?auth=" + WS->arg("auth")) : "") +
                "\">📂 Tallózás</a></span></div>";
    }
#endif
    html += "</div>";

    // Restart button
    html += "<div style=\"margin-top:12px;text-align:center\"><a href=\"/restart\" "
            "style=\"background:#da3633;color:white;padding:10px "
            "20px;border-radius:6px;text-decoration:none;display:inline-block\" onclick=\"return confirm('Biztosan "
            "újraindítod az eszközt?')\">&#128260; Újraindítás</a></div>";

    html += pageFoot();

    WS->send(200, "text/html", html);
}

// ─── CONFIG PAGE ───────────────────────────────────────────────
static void handleConfig()
{
    if (!web_auth_ok())
        return;
    // Build form with current values pre-filled
    String html;
    html.reserve(8000);
    html = pageStart(F("Modbus-MQTT Bridge — Beállítások"), CSS_FORMS) + pageStyleEnd();

    html += F("<h1>&#9889; Beállítások</h1>");
    html += navHtml(PG_CONFIG, WS->hasArg("auth") ? ("?auth=" + WS->arg("auth")) : "");
    html += F("<div class=\"pri\"><b>&#128279; LAN elsődleges — WiFi automatikus fallback</b><br><span class=\"note\">LAN mindig preferált. Ha leáll, WiFi átveszi. Ha LAN visszajön, automatikusan visszaáll.</span></div>"
              "<form action=\"/save\" method=\"POST\">");

    // ── Device Identity ──────────────────────────────────
    html += "<h2>&#128100; Eszközazonosító</h2>";
    html += "<div class=\"fm\"><label>Hostname (mDNS, AP név, MQTT ID)</label><input name=\"hostname\" value=\"" +
            htmlEscape(cfg.hostname) + "\"></div>";
    html += "<p class=\"note\">Több eszköznél egyedi hostname szükséges! Pl: modbusmqtt-garazs, modbusmqtt-pince</p>";

    // ── LAN Section ─────────────────────────────────────
    html += "<h2>&#128279; LAN / Ethernet (Elsődleges)</h2>";
    html += "<div class=\"chk\"><input type=\"checkbox\" id=\"lanen\" name=\"lanen\" value=\"1\" " +
            String(cfg.lan_enabled ? "checked" : "") +
            " onchange=\"toggleLan()\"><label for=\"lanen\">LAN engedélyezése</label></div>";
    html += "<div id=\"lanSection\" style=\"display:" + String(cfg.lan_enabled ? "block" : "none") + "\">";

    html += "<div class=\"fm\"><label>Chip típus</label><select name=\"lantype\">";
    html += "<option value=\"0\"" + String(cfg.lan_type == 0 ? " selected" : "") + ">W5500 (SPI)</option>";
    html += "<option value=\"1\"" + String(cfg.lan_type == 1 ? " selected" : "") + ">LAN8720 (RMII)</option>";
    html += "<option value=\"2\"" + String(cfg.lan_type == 2 ? " selected" : "") +
            ">IP101 (Waveshare beépített)</option></select></div>";

    html += "<div class=\"radio\"><label><input type=\"radio\" name=\"landhcp\" value=\"1\" " +
            String(cfg.lan_dhcp ? "checked" : "") + " onchange=\"toggleLanStatic()\"> DHCP (automatikus)</label>";
    html += "<label><input type=\"radio\" name=\"landhcp\" value=\"0\" " + String(!cfg.lan_dhcp ? "checked" : "") +
            " onchange=\"toggleLanStatic()\"> Kézi (statikus IP)</label></div>";

    html += "<div id=\"lanStatic\" style=\"display:" + String(cfg.lan_dhcp ? "none" : "block") + "\">";
    html += "<div class=\"row\"><div class=\"fm\"><label>IP cím</label><input name=\"lanip\" value=\"" +
            String(cfg.lan_ip) + "\" placeholder=\"192.168.1.100\"></div>";
    html += "<div class=\"fm\"><label>Alhálózati maszk</label><input name=\"lanmask\" value=\"" + String(cfg.lan_mask) +
            "\" placeholder=\"255.255.255.0\"></div></div>";
    html += "<div class=\"row\"><div class=\"fm\"><label>Átjáró</label><input name=\"langw\" value=\"" +
            String(cfg.lan_gw) + "\" placeholder=\"192.168.1.1\"></div>";
    html += "<div class=\"fm\"><label>DNS</label><input name=\"landns\" value=\"" + String(cfg.lan_dns) +
            "\" placeholder=\"192.168.1.1\"></div></div></div></div>";

    // ── WiFi Section ─────────────────────────────────────
    html += "<h2>&#128225; WiFi (Fallback + AP)</h2>";
    html += "<div class=\"radio\"><label><input type=\"radio\" name=\"wifimode\" value=\"0\" " +
            String(cfg.wifi_mode == 0 ? "checked" : "") + "> AP+STA (mindig elérhető AP-n)</label>";
    html += "<label><input type=\"radio\" name=\"wifimode\" value=\"1\" " +
            String(cfg.wifi_mode == 1 ? "checked" : "") + "> Csak STA</label></div>";
    html += "<p class=\"note\">AP+STA: saját WiFi hálózatot is sugároz, így elérhető ha nincs router. Csak STA: "
            "kevesebb áram, de router nélkül elérhetetlen.</p>";
    html += "<div class=\"fm\"><label>SSID</label><input name=\"ssid\" value=\"" + htmlEscape(cfg.wifi_ssid) +
            "\" placeholder=\"WiFi neve\"></div>";
    html += "<div class=\"fm\"><label>Jelszó</label><input type=\"password\" name=\"wpass\" value=\"" +
            htmlEscape(cfg.wifi_pass) + "\" placeholder=\"WiFi jelszó\"></div>";
    html += "<div class=\"fm\"><label>AP Név</label><input name=\"apn\" value=\"" + htmlEscape(cfg.ap_name) +
            "\" placeholder=\"Üres = automatikus (hostname-MAC)\"></div>";
    html += "<div class=\"fm\"><label>AP Jelszó</label><input type=\"password\" name=\"appass\" value=\"" +
            htmlEscape(cfg.ap_pass) +
            "\" placeholder=\"12345678\"><p class=\"note\">Min. 8 karakter. Mentés után az AP azonnal újraindul az új "
            "jelszóval.</p></div>";

    html += "<div class=\"radio\"><label><input type=\"radio\" name=\"wdhcp\" value=\"1\" " +
            String(cfg.wifi_dhcp ? "checked" : "") + " onchange=\"toggleWifiStatic()\"> DHCP (automatikus)</label>";
    html += "<label><input type=\"radio\" name=\"wdhcp\" value=\"0\" " + String(!cfg.wifi_dhcp ? "checked" : "") +
            " onchange=\"toggleWifiStatic()\"> Kézi (statikus IP)</label></div>";

    html += "<div id=\"wifiStatic\" style=\"display:" + String(cfg.wifi_dhcp ? "none" : "block") + "\">";
    html += "<div class=\"row\"><div class=\"fm\"><label>IP cím</label><input name=\"wip\" value=\"" +
            String(cfg.wifi_ip) + "\" placeholder=\"192.168.1.101\"></div>";
    html += "<div class=\"fm\"><label>Alhálózati maszk</label><input name=\"wmask\" value=\"" + String(cfg.wifi_mask) +
            "\" placeholder=\"255.255.255.0\"></div></div>";
    html += "<div class=\"row\"><div class=\"fm\"><label>Átjáró</label><input name=\"wgw\" value=\"" +
            String(cfg.wifi_gw) + "\" placeholder=\"192.168.1.1\"></div>";
    html += "<div class=\"fm\"><label>DNS</label><input name=\"wdns\" value=\"" + String(cfg.wifi_dns) +
            "\" placeholder=\"192.168.1.1\"></div></div></div>";

    // ── MQTT Section ─────────────────────────────────────
    html += "<h2>&#128172; MQTT Broker</h2>";
    html += "<div class=\"row\"><div class=\"fm\"><label>Host</label><input name=\"mhost\" value=\"" +
            htmlEscape(cfg.mqtt_host) + "\"></div>";
    html += "<div class=\"fm\"><label>Port</label><input type=\"number\" name=\"mport\" value=\"" +
            String(cfg.mqtt_port) + "\"></div></div>";
    html += "<div class=\"row\"><div class=\"fm\"><label>Felhasználó</label><input name=\"muser\" value=\"" +
            htmlEscape(cfg.mqtt_user) + "\"></div>";
    html += "<div class=\"fm\"><label>Jelszó</label><input type=\"password\" name=\"mpass\" value=\"" +
            htmlEscape(cfg.mqtt_pass) + "\"></div></div>";
    html += "<div class=\"fm\"><label>MQTT Prefix</label><input name=\"mpfx\" value=\"" + htmlEscape(cfg.mqtt_prefix) +
            "\"></div>";
    html += "<div class=\"chk\"><input type=\"checkbox\" id=\"mtls\" name=\"mtls\" value=\"1\" " +
            String(cfg.mqtt_tls ? "checked" : "") + "><label for=\"mtls\">TLS (titkosított kapcsolat)</label></div>";
    html += "<p class=\"note\">TLS: port 8883 ajánlott. LAN broker esetén nem kötelező. A tanúsítvány nincs "
            "ellenőrizve (setInsecure).</p>";

    // ── Auth Section ─────────────────────────────────────
    html += "<h2>&#128272; Web hitelesítés</h2>";
    html += "<div class=\"chk\"><input type=\"checkbox\" id=\"wauth\" name=\"wauth\" value=\"1\" " +
            String(cfg.web_auth ? "checked" : "") + "><label for=\"wauth\">Web hitelesítés bekapcsolása</label></div>";
    html += "<div class=\"fm\"><label>Auth jelszó</label><input type=\"password\" name=\"wauthp\" value=\"" +
            htmlEscape(cfg.web_pass) + "\" placeholder=\"Üres = kikapcsolva\"></div>";
    html += "<p class=\"note\">A hitelesítés védi a teljes felületet (minden oldal és API). Felhasználónév: admin</p>";

    // ── HA Section ───────────────────────────────────────
    html += "<h2>&#127968; Home Assistant</h2>";
    html += "<div class=\"chk\"><input type=\"checkbox\" id=\"hadisc\" name=\"hadisc\" value=\"1\" " +
            String(cfg.ha_discovery ? "checked" : "") +
            "><label for=\"hadisc\">Auto-discovery (MQTT automatikus eszközfelvétel)</label></div>";
    html += "<p class=\"note\">6 relé + 6 DI automatikusan megjelenik a HA-ban. Kikapcsolva csak nyers MQTT.</p>";

    // ── Modbus Section ───────────────────────────────────
    html += "<h2>&#128268; Modbus RS485</h2>";
    html += "<div class=\"row\"><div class=\"fm\"><label>Baud rate</label><select name=\"mbaud\">";
    for (int b : {9600, 19200, 38400, 115200})
    {
        html += "<option value=\"" + String(b) + "\"" + String(cfg.mb_baud == (uint32_t)b ? " selected" : "") + ">" +
                String(b) + "</option>";
    }
    html += "</select></div>";
    html += "<div class=\"fm\"><label>Paritás</label><select name=\"mpar\">";
    html += "<option value=\"0\"" + String(cfg.mb_parity == 0 ? " selected" : "") + ">8N1</option>";
    html += "<option value=\"1\"" + String(cfg.mb_parity == 1 ? " selected" : "") + ">8E1</option>";
    html += "<option value=\"2\"" + String(cfg.mb_parity == 2 ? " selected" : "") +
            ">8O1</option></select></div></div>";
    html += "<div class=\"row\"><div class=\"fm\"><label>Cím kezdete</label><input type=\"number\" name=\"mstart\" "
            "value=\"" +
            String(cfg.mb_scan_start) + "\" min=\"1\" max=\"247\"></div>";
    html += "<div class=\"fm\"><label>Cím vége</label><input type=\"number\" name=\"mend\" value=\"" +
            String(cfg.mb_scan_end) + "\" min=\"1\" max=\"247\"></div></div>";

    // Profile dropdown
    html += "<div class=\"fm\"><label>Modbus profil</label><select name=\"mbprof\" id=\"mbprof\" "
            "onchange=\"toggleGeneric()\">";
    html += "<option value=\"1\"" + String(cfg.mb_profile == MB_PROFILE_KC868_HA ? " selected" : "") +
            ">KC868-HA V2</option>";
    html += "<option value=\"2\"" + String(cfg.mb_profile == MB_PROFILE_GENERIC ? " selected" : "") +
            ">Generic (konfigurálható)</option>";
    html += "<option value=\"3\"" + String(cfg.mb_profile == MB_PROFILE_NIBE ? " selected" : "") +
            ">NIBE S1156-18</option>";
    html += "<option value=\"4\"" + String(cfg.mb_profile == MB_PROFILE_SABIANA ? " selected" : "") +
            ">Sabiana</option>";
    html += "<option value=\"0\"" + String(cfg.mb_profile == MB_PROFILE_CUSTOM ? " selected" : "") + ">Egyedi</option>";
    html += "</select></div>";

    // Generic profile register settings (only visible when Generic selected)
    html += "<div id=\"genericSection\" style=\"display:" +
            String(cfg.mb_profile == MB_PROFILE_GENERIC ? "block" : "none") + "\">";
    html += "<div class=\"row\"><div class=\"fm\"><label>Coil kezdő regiszter</label><input type=\"number\" "
            "name=\"mbcoil\" value=\"" +
            String(cfg.mb_reg_coil_start) + "\" min=\"0\" max=\"65535\"></div>";
    html += "<div class=\"fm\"><label>DI kezdő regiszter</label><input type=\"number\" name=\"mbdi\" value=\"" +
            String(cfg.mb_reg_di_start) + "\" min=\"0\" max=\"65535\"></div></div>";
    html += "</div>";

    // Poll rate
    html += "<div class=\"fm\"><label>Polling időköz (ms)</label><input type=\"number\" name=\"mbpoll\" value=\"" +
            String(cfg.mb_poll_ms) + "\" min=\"500\" max=\"30000\"></div>";
    html += "<p class=\"note\">Modbus lekérdezés gyakorisága. Alacsonyabb = gyorsabb válasz, de nagyobb hálózati "
            "forgalom. Ajánlott: 1000-5000 ms.</p>";

    // ── Virtual Module ──────────────────────────────
    html += "<h2>&#128187; Virtuális Modul (Teszt)</h2>";
    html += "<div class=\"chk\"><input type=\"checkbox\" id=\"vmod\" name=\"vmod\" value=\"1\" " +
            String(cfg.virtual_module ? "checked" : "") +
            "><label for=\"vmod\">Virtuális HA V2 modul bekapcsolása</label></div>";
    html += "<p class=\"note\">Cím 200. Szimulált 6 relé + 6 DI fizikai hardver nélkül. Tesztelésre és HA dashboard "
            "beállításra.</p>";

    // ── TCP Section ──────────────────────────────────────
    html += "<h2>&#128272; Modbus TCP Bridge</h2>";
    html += "<div class=\"chk\"><input type=\"checkbox\" id=\"tcpen\" name=\"tcpen\" value=\"1\" " +
            String(cfg.tcp_enabled ? "checked" : "") +
            " onchange=\"toggleTcp()\"><label for=\"tcpen\">TCP bridge engedélyezése</label></div>";
    html += "<div id=\"tcpSection\" style=\"display:" + String(cfg.tcp_enabled ? "block" : "none") +
            "\"><div class=\"fm\"><label>TCP port</label><input type=\"number\" name=\"tcpp\" value=\"" +
            String(cfg.tcp_port) + "\"></div></div>";

    // Save button
    html += "<button type=\"submit\">&#128190; Mentés & Újraindítás</button></form>";

    // Back to status
    html += "<div style=\"text-align:center;margin:12px 0\"><a href=\"/\" style=\"color:#58a6ff\">← Vissza a státusz "
            "oldalra</a></div>";

    html += "<div class=\"foot\">Modbus-MQTT Bridge v2.0 — ESP32-S3-ETH (6DI+6R) — ESP32-S3</div>";

    // JavaScript
    html += R"rawliteral(<script>
function toggleLan(){document.getElementById('lanSection').style.display=document.getElementById('lanen').checked?'block':'none'}
function toggleLanStatic(){var d=document.querySelector('input[name=landhcp]:checked').value==='0';document.getElementById('lanStatic').style.display=d?'block':'none'}
function toggleWifiStatic(){var d=document.querySelector('input[name=wdhcp]:checked').value==='0';document.getElementById('wifiStatic').style.display=d?'block':'none'}
function toggleTcp(){document.getElementById('tcpSection').style.display=document.getElementById('tcpen').checked?'block':'none'}
function toggleGeneric(){var p=document.getElementById('mbprof').value;document.getElementById('genericSection').style.display=(p==='2')?'block':'none'}
</script></body></html>)rawliteral";

    WS->send(200, "text/html", html);
}

// ─── SAVE Handler ──────────────────────────────────────────────
static void handleSave()
{
    if (!web_auth_ok())
        return;
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);

    // Only write keys that are present in the POST body (partial update).
    // Missing keys are NOT overwritten — prevents D4 bug (empty field → NVRAM zeroed)

    if (WS->hasArg("ssid"))
        nv.putString(NV_KEY_WIFI_SSID, WS->arg("ssid"));
    if (WS->hasArg("wpass"))
        nv.putString(NV_KEY_WIFI_PASS, WS->arg("wpass"));
    if (WS->hasArg("apn"))
        nv.putString(NV_KEY_AP_NAME, WS->arg("apn"));
    if (WS->hasArg("appass"))
        nv.putString(NV_KEY_AP_PASS, WS->arg("appass"));
    if (WS->hasArg("wifimode"))
        nv.putUChar(NV_KEY_WIFI_MODE, WS->arg("wifimode").toInt());
    if (WS->hasArg("wdhcp"))
        nv.putBool(NV_KEY_WIFI_DHCP, WS->arg("wdhcp") == "1");
    if (WS->hasArg("wip"))
        nv.putString(NV_KEY_WIFI_IP, WS->arg("wip"));
    if (WS->hasArg("wgw"))
        nv.putString(NV_KEY_WIFI_GW, WS->arg("wgw"));
    if (WS->hasArg("wmask"))
        nv.putString(NV_KEY_WIFI_MASK, WS->arg("wmask"));
    if (WS->hasArg("wdns"))
        nv.putString(NV_KEY_WIFI_DNS, WS->arg("wdns"));

    if (WS->hasArg("lanen"))
        nv.putBool(NV_KEY_ETH_EN, WS->arg("lanen") == "1");
    if (WS->hasArg("landhcp"))
        nv.putBool(NV_KEY_ETH_DHCP, WS->arg("landhcp") == "1");
    if (WS->hasArg("lanip"))
        nv.putString(NV_KEY_ETH_IP, WS->arg("lanip"));
    if (WS->hasArg("langw"))
        nv.putString(NV_KEY_ETH_GW, WS->arg("langw"));
    if (WS->hasArg("lanmask"))
        nv.putString(NV_KEY_ETH_MASK, WS->arg("lanmask"));
    if (WS->hasArg("landns"))
        nv.putString(NV_KEY_ETH_DNS, WS->arg("landns"));
    if (WS->hasArg("lantype"))
        nv.putUChar(NV_KEY_ETH_TYPE, WS->arg("lantype").toInt());

    if (WS->hasArg("mhost"))
        nv.putString(NV_KEY_MQTT_HOST, WS->arg("mhost"));
    if (WS->hasArg("mport"))
        nv.putUShort(NV_KEY_MQTT_PORT, WS->arg("mport").toInt());
    if (WS->hasArg("muser"))
        nv.putString(NV_KEY_MQTT_USER, WS->arg("muser"));
    if (WS->hasArg("mpass"))
        nv.putString(NV_KEY_MQTT_PASS, WS->arg("mpass"));
    if (WS->hasArg("mpfx"))
        nv.putString(NV_KEY_MQTT_PREFIX, WS->arg("mpfx"));
    if (WS->hasArg("mtls"))
        nv.putBool(NV_KEY_MQTT_TLS, WS->arg("mtls") == "1");

    if (WS->hasArg("hadisc"))
        nv.putBool(NV_KEY_HA_DISC, WS->arg("hadisc") == "1");

    if (WS->hasArg("mbaud"))
        nv.putUInt(NV_KEY_MB_BAUD, WS->arg("mbaud").toInt());
    if (WS->hasArg("mstart"))
        nv.putUChar(NV_KEY_MB_START, WS->arg("mstart").toInt());
    if (WS->hasArg("mend"))
        nv.putUChar(NV_KEY_MB_END, WS->arg("mend").toInt());
    if (WS->hasArg("mpar"))
        nv.putUChar(NV_KEY_MB_PARITY, WS->arg("mpar").toInt());
    if (WS->hasArg("mbprof"))
        nv.putUChar(NV_KEY_MB_PROFILE, WS->arg("mbprof").toInt());
    if (WS->hasArg("mbcoil"))
        nv.putUShort(NV_KEY_MB_REG_COIL, WS->arg("mbcoil").toInt());
    if (WS->hasArg("mbdi"))
        nv.putUShort(NV_KEY_MB_REG_DI, WS->arg("mbdi").toInt());
    if (WS->hasArg("mbpoll"))
        nv.putUShort(NV_KEY_MB_POLL_MS, WS->arg("mbpoll").toInt());
    if (WS->hasArg("vmod"))
        nv.putBool(NV_KEY_VIRTUAL_MOD, WS->arg("vmod") == "1");

    if (WS->hasArg("tcpen"))
        nv.putBool(NV_KEY_TCP_EN, WS->arg("tcpen") == "1");
    if (WS->hasArg("tcpp"))
        nv.putUShort(NV_KEY_TCP_PORT, WS->arg("tcpp").toInt());

    if (WS->hasArg("hostname"))
        nv.putString(NV_KEY_HOSTNAME, WS->arg("hostname"));

    if (WS->hasArg("wauth"))
        nv.putBool(NV_KEY_WEB_AUTH, WS->arg("wauth") == "1");
    if (WS->hasArg("wauthp"))
        nv.putString(NV_KEY_WEB_PASS, WS->arg("wauthp"));

    nv.end();
    LOG_ILN("[WEB] Settings saved");

    // Update config CRC after NVRAM write
    config_write_crc();

    WS->send(200, "text/html", pageStart(F("Mentve"), CSS_MINIMAL) + pageStyleEnd() +
             F("<h1 class=\"ok\">&#10004; Beállítások elmentve!</h1>"
               "<p>Az eszköz újraindul 3 másodperc múlva...</p>"
               "<script>setTimeout(function(){window.location='/'},5000)</script>"
               "</body></html>"));

    delay(3000);
    eth_hard_reset_and_restart();
}

// ─── RESTART Handler ───────────────────────────────────────────
static void handleRestart()
{
    if (!web_auth_ok())
        return;
    WS->send(200, "text/html", pageStart(F("Újraindítás"), CSS_MINIMAL) + pageStyleEnd() +
             F("<h1 class=\"ok\">&#128260; Újraindítás...</h1><p>Az eszköz újraindul.</p>"
               "<script>setTimeout(function(){window.location='/'},10000)</script>"
               "</body></html>"));
    delay(1000);
    eth_hard_reset_and_restart();
}

// ─── LOGOUT Handler ────────────────────────────────────────────
// Digest Auth logout: send 401 with stale=true to force browser
// to discard cached credentials, then redirect to /
static void handleLogout()
{
    if (!cfg.web_auth)
    {
        // Auth disabled — just redirect
        WS->sendHeader("Location", "/");
        WS->send(302);
        return;
    }
    // Force browser to drop Digest credentials by requesting re-auth
    // with a different stale realm — then redirect
    WS->sendHeader("Location", "/");
    WS->requestAuthentication();
}

// ─── PINS PAGE ─────────────────────────────────────────────────
static void handlePins()
{
    if (!web_auth_ok())
        return;
    String html;
    html.reserve(6000);
    html = pageStart(F("Modbus-MQTT Bridge — Pinek"), CSS_FORMS) + FPSTR(CSS_PINS) + pageStyleEnd();

    html += F("<h1>&#128295; GPIO Pinek</h1>");
    html += navHtml(PG_PINS, WS->hasArg("auth") ? ("?auth=" + WS->arg("auth")) : "");
    html += F("<p class=\"note\">-1 = nem használt / letiltott. A pinek megváltoztatása után újraindítás szükséges!</p>"
              "<form action=\"/savepins\" method=\"POST\">");

    // RS485 section
    html += "<h2>&#128268; RS485 / Modbus bus</h2>";
    html += "<div class=\"row\">";
    html += "<div class=\"fm\"><label>UART2 RX</label><input type=\"number\" name=\"prx\" value=\"" +
            String(cfg.pin_rs485_rx) + "\" min=\"-1\" max=\"48\"></div>";
    html += "<div class=\"fm\"><label>UART2 TX</label><input type=\"number\" name=\"ptx\" value=\"" +
            String(cfg.pin_rs485_tx) + "\" min=\"-1\" max=\"48\"></div>";
    html += "<div class=\"fm\"><label>DE/RE (Driver Enable)</label><input type=\"number\" name=\"pde\" value=\"" +
            String(cfg.pin_rs485_de) + "\" min=\"-1\" max=\"48\"></div>";
    html += "</div>";

    // Status LED & Button
    html += "<h2>&#128161; Státusz LED és gomb</h2>";
    html += "<div class=\"row\">";
    html += "<div class=\"fm\"><label>Status LED</label><input type=\"number\" name=\"pled\" value=\"" +
            String(cfg.pin_status_led) + "\" min=\"-1\" max=\"48\"></div>";
    html += "<div class=\"fm\"><label>Konfig gomb (BOOT)</label><input type=\"number\" name=\"pbtn\" value=\"" +
            String(cfg.pin_config_btn) + "\" min=\"-1\" max=\"48\"></div>";
    html += "</div>";

    // W5500 Ethernet
    html += "<h2>&#128279; W5500 SPI Ethernet</h2>";
    html += "<div class=\"warn-box\"><b>&#9888; Figyelem!</b> Waveshare ESP32-S3-ETH V1.0 beépített IP101 Ethernet — "
            "ezek a pinek csak W5500 SPI modulhoz!</div>";
    html += "<div class=\"row\">";
    html += "<div class=\"fm\"><label>SPI MOSI</label><input type=\"number\" name=\"pemosi\" value=\"" +
            String(cfg.pin_eth_mosi) + "\" min=\"-1\" max=\"48\"></div>";
    html += "<div class=\"fm\"><label>SPI MISO</label><input type=\"number\" name=\"pemiso\" value=\"" +
            String(cfg.pin_eth_miso) + "\" min=\"-1\" max=\"48\"></div>";
    html += "<div class=\"fm\"><label>SPI CLK</label><input type=\"number\" name=\"pesclk\" value=\"" +
            String(cfg.pin_eth_sclk) + "\" min=\"-1\" max=\"48\"></div>";
    html += "</div>";
    html += "<div class=\"row\">";
    html += "<div class=\"fm\"><label>SPI CS</label><input type=\"number\" name=\"pecs\" value=\"" +
            String(cfg.pin_eth_cs) + "\" min=\"-1\" max=\"48\"></div>";
    html += "<div class=\"fm\"><label>INT (Interrupt)</label><input type=\"number\" name=\"peint\" value=\"" +
            String(cfg.pin_eth_int) + "\" min=\"-1\" max=\"48\"></div>";
    html += "<div class=\"fm\"><label>RST (Reset)</label><input type=\"number\" name=\"perst\" value=\"" +
            String(cfg.pin_eth_rst) + "\" min=\"-1\" max=\"48\"></div>";
    html += "</div>";

    // ── SD Card (shares FSPI bus with W5500) ──
#ifdef USE_SD
    html += "<h2>&#128190; SD Kártya</h2>";
    html += "<div class=\"warn-box\"><b>&#9888; Külön SPI buszok!</b> "
            "W5500: FSPI (MOSI=11, MISO=12, SCLK=13, CS=14). "
            "SD: HSPI (MOSI=6, MISO=5, SCLK=7, CS=4). "
            "SDIO 1-bit: CLK=7, CMD=4, D0=6, D1=5.</div>";
    // Check GPIO4 conflict: RS485 DE vs SD CS / SDIO CMD
    if (cfg.pin_rs485_de == cfg.pin_sd_cs && cfg.pin_sd_cs >= 0)
    {
        html += "<div class=\"error-box\"><b>&#128680; GPIO" + String(cfg.pin_sd_cs) +
                " ütközés!</b> RS485 DE/RE és SD CS ugyanazon a pinen! "
                "SD használathoz állítsd a DE/RE-t más GPIO-ra (pl. 42).</div>";
    }
    html += "<div class=\"row\">";
    html += "<div class=\"fm\"><label>SD Engedélyezve</label><select name=\"sdena\">"
            "<option value=\"1\"" + String(cfg.sd_enabled ? " selected" : "") + ">Igen</option>"
            "<option value=\"0\"" + String(!cfg.sd_enabled ? " selected" : "") + ">Nem</option></select></div>";
    html += "<div class=\"fm\"><label>SD CS Pin</label><input type=\"number\" name=\"psdcs\" value=\"" +
            String(cfg.pin_sd_cs) + "\" min=\"-1\" max=\"48\"></div>";
    html += "</div>";
#else
    html += "<h2>&#128190; SD Kártya</h2>";
    html += "<div class=\"info-box\">SD kártya támogatás nincs belefordítva. Fordítsd <code>-DUSE_SD</code> flaggel.</div>";
#endif

    html += "<button type=\"submit\">&#128190; Mentés & Újraindítás</button></form>";
    html += "<div class=\"foot\">Modbus-MQTT Bridge v2.0 — ESP32-S3-ETH (6DI+6R) — ESP32-S3</div>";
    html += "</body></html>";

    WS->send(200, "text/html", html);
}

// ─── MODULES PAGE ──────────────────────────────────────────────
static void handleModules()
{
    if (!web_auth_ok())
        return;
    String html;
    html.reserve(8000); // Largest page, many card loops
    html = pageStart(F("Modbus-MQTT Bridge — Modulok"), CSS_MODULES) + FPSTR(CSS_FORMS) + R"rawliteral(
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
var _auth=(location.search.match(/auth=([^&]+)/)||[])[1]||'';
function setDiRelay(addr,di,val){
  var q='/api/direlay?addr='+addr+'&d'+di+'='+val;
  if(_auth)q+='&auth='+_auth;
  fetch(q,{method:'POST'})
  .then(r=>r.json())
  .then(d=>{if(!d.ok)alert('Hiba a mentésnél');})
  .catch(e=>{alert('Hálózati hiba');});
}
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
function toggleDI(addr,di,curState){
  var newState=curState?0:1;
  var btn=document.getElementById('d'+addr+'_'+di);
  if(!btn)return;
  btn.style.opacity='0.5';
  fetch('/di?addr='+addr+'&di='+di+'&state='+newState)
  .then(r=>r.json())
  .then(d=>{
    if(d.ok){
      btn.textContent='DI'+(di+1)+' '+(newState?'ON':'OFF');
      btn.className='rbtn '+(newState?'on':'off');
      btn.onclick=function(){toggleDI(addr,di,newState);};
    }else{
      alert('Hiba: '+(d.error||'ismeretlen'));
    }
    btn.style.opacity='1';
  })
  .catch(e=>{alert('Hálózati hiba');btn.style.opacity='1';});
}
</script>
</head><body>)rawliteral";

    html += F("<h1>&#128268; Modbus Modulok</h1>");
    html += navHtml(PG_MODULES, WS->hasArg("auth") ? ("?auth=" + WS->arg("auth")) : "");

    if (!scan_active)
    {
        html += "<form action=\"/rescan\" method=\"POST\" style=\"display:inline;margin-bottom:8px\"><button "
                "type=\"submit\" style=\"background:#1f6feb\">🔄 Újrascan</button></form>";
        if (module_count > 0)
        {
            html += " <form action=\"/savemodlist\" method=\"POST\" style=\"display:inline;margin-bottom:8px\"><button "
                    "type=\"submit\" style=\"background:#238636\">💾 Modullista mentése</button></form>";
        }
    }
    else
    {
        html += "<div id=\"scan-progress\" style=\"margin:8px 0\"><div "
                "style=\"background:#21262d;border-radius:4px;overflow:hidden;height:20px\"><div id=\"scan-bar\" "
                "style=\"background:#1f6feb;height:100%;width:0%;transition:width 0.3s\"></div></div><p class=\"note\" "
                "id=\"scan-text\">Buszkeresés folyamatban...</p></div>";
    }

    if (module_count == 0 && !scanning_done)
    {
        html += "<p class=\"note\">Modbus buszkeresés folyamatban...</p>";
    }
    else if (module_count == 0)
    {
        html += "<p class=\"note\">Nem találhatók Modbus modulok. Ellenőrizd a bekötést és a címtartományt a "
                "Beállítások oldalon.</p>";
    }

    // Global save form
    html += "<form action=\"/savemodules\" method=\"POST\">";

    for (uint16_t i = 0; i < module_count; i++)
    {
        Slave_Module &m = modules[i];
        String cls = m.online ? "on" : "off";
        String st = m.online ? "✅ ONLINE" : "❌ OFFLINE";

        String mqtt_name = config_get_mqtt_name(m.slave_addr);
        String ha_name = config_get_ha_name(m.slave_addr);

        html += "<div class=\"mod-card\">";
        String sim_badge = m.is_virtual ? " <span class=\"badge sensor\">SIM</span>" : "";
        html += "<div class=\"row\"><span class=\"key\">S" + String(m.slave_addr) + " " + String(m.model.model_name) +
                sim_badge + "</span><span class=\"val " + cls + "\">" + st + "</span></div>";
        // Virtual module: inaktív gomb
        if (m.is_virtual)
        {
            html += "<div style=\"margin:8px 0\"><a href=\"/togglevmod\" "
                    "style=\"background:#f85149;color:white;padding:6px "
                    "14px;border-radius:4px;text-decoration:none;font-size:13px;font-weight:600\">🗑️ Virtuális "
                    "modul inaktíválása</a></div>";
        }
        if (m.online)
        {
            html += "<div class=\"row\"><span class=\"key\">FW / SN</span><span class=\"val\">" +
                    String(m.model.firmware_ver) + " / " + String(m.model.serial_number) + "</span></div>";
        }

        // Entity badges (only for online modules — relays are clickable)
        if (m.online)
        {
            // Relays — clickable toggle buttons
            html += "<div class=\"mod-section\"><div class=\"mod-section-title\">⚡ Relék <span class=\"note\">(kattintás = kapcsolás)</span></div><div class=\"entity-row\">";
            for (uint8_t r = 0; r < HA_V2_RELAY_COUNT; r++)
            {
                String rcls = m.relays[r].state ? "rbtn on" : "rbtn off";
                String rtxt = "R" + String(r + 1) + (m.relays[r].state ? " ON" : " OFF");
                html += "<span id=\"r" + String(m.slave_addr) + "_" + String(r) + "\" class=\"" + rcls +
                        "\" onclick=\"toggleRelay(" + String(m.slave_addr) + "," + String(r) + "," +
                        String(m.relays[r].state ? 1 : 0) + ")\">" + rtxt + "</span>";
            }
            html += "</div></div>";

            // Digital Inputs
            html += "<div class=\"mod-section\"><div class=\"mod-section-title\">🔌 Bemenetek</div>";
            for (uint8_t d = 0; d < HA_V2_DI_COUNT; d++)
            {
                html += "<div class=\"entity-row\">";
                if (m.is_virtual)
                {
                    // Virtual module: clickable toggle button
                    String dcls = m.inputs[d].current ? "rbtn on" : "rbtn off";
                    String dtxt = "DI" + String(d + 1) + " " + (m.inputs[d].current ? "ON" : "OFF");
                    html += "<span id=\"d" + String(m.slave_addr) + "_" + String(d) +
                            "\" class=\"" + dcls +
                            "\" onclick=\"toggleDI(" + String(m.slave_addr) + "," + String(d) +
                            "," + String(m.inputs[d].current ? 1 : 0) + ")\">" + dtxt + "</span>";
                }
                else
                {
                    if (m.inputs[d].current)
                    {
                        html += "<span class=\"badge on\">DI" + String(d + 1) + " ON</span>";
                    }
                    else
                    {
                        html += "<span class=\"badge off\">DI" + String(d + 1) + " OFF</span>";
                    }
                }
                // ── DI→Relay mapping selector ──
                html += "<select id=\"dr" + String(m.slave_addr) + "_" + String(d) +
                         "\" onchange=\"setDiRelay(" + String(m.slave_addr) + "," + String(d) + ",this.value)\" style=\"font-size:12px;margin-left:8px;\">";
                html += "<option value=\"255\"" + String(m.di_relay_map[d] == 255 ? " selected" : "") + ">—</option>";
                for (uint8_t r = 0; r < m.model.RELAY_COUNT; r++)
                    html += "<option value=\"" + String(r) + "\"" +
                             String(m.di_relay_map[d] == r ? " selected" : "") +
                             ">R" + String(r + 1) + "</option>";
                html += "</select>";
                html += "</div>"; // entity-row
            }
            html += "</div>";

            // Click sensors
            html += "<div class=\"mod-section\"><div class=\"mod-section-title\">👆 Kattintás</div><div class=\"entity-row\">";
            for (uint8_t d = 0; d < HA_V2_DI_COUNT; d++)
            {
                html += "<span class=\"badge sensor\">DI" + String(d + 1) + " ⚡</span>";
            }
            html += "</div></div>";
        }

        html += "<div class=\"mrow\">";
        html += "<div class=\"fm\"><label>MQTT név</label><input name=\"mn" + String(m.slave_addr) + "\" value=\"" +
                htmlEscape(mqtt_name) + "\" placeholder=\"pl. konyha_rele\"></div>";
        html += "<div class=\"fm\"><label>HA Friendly Name</label><input name=\"hn" + String(m.slave_addr) +
                "\" value=\"" + htmlEscape(ha_name) + "\" placeholder=\"pl. Konyha relék\"></div>";
        html += "</div>";

        // Room/Area selector
        String cur_area = config_get_module_area(m.slave_addr);
        html += "<div class=\"fm\"><label>Szoba / Helyiség</label>" +
                roomSelectHtml("ar" + String(m.slave_addr), cur_area) + "</div>";

        // Per-relay names (only for online modules)
        if (m.online)
        {
            html += "<div class=\"mod-section\"><div class=\"mod-section-title\">🏷️ Relé nevek</div><div class=\"mrow-wrap\">";
            for (uint8_t r = 0; r < HA_V2_RELAY_COUNT; r++)
            {
                String rn = config_get_relay_name(m.slave_addr, r);
                html += "<div class=\"fm-sm\"><label>R" + String(r + 1) + "</label><input name=\"rn" +
                        String(m.slave_addr) + "_" + String(r) + "\" value=\"" + htmlEscape(rn) + "\" placeholder=\"Relé " +
                        String(r + 1) + "\"></div>";
            }
            html += "</div></div>";

            html += "<div class=\"mod-section\"><div class=\"mod-section-title\">🏷️ DI nevek</div><div class=\"mrow-wrap\">";
            for (uint8_t d = 0; d < HA_V2_DI_COUNT; d++)
            {
                String dn = config_get_di_name(m.slave_addr, d);
                html += "<div class=\"fm-sm\"><label>DI" + String(d + 1) + "</label><input name=\"dn" +
                        String(m.slave_addr) + "_" + String(d) + "\" value=\"" + htmlEscape(dn) + "\" placeholder=\"DI " +
                        String(d + 1) + "\"></div>";
            }
            html += "</div></div>";
        }

        html += "</div>";
    }

    if (module_count > 0)
    {
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
        html += "<p class=\"note\">Az alapértelmezett 15 szoba mindig elérhető. Itt adhatsz hozzá új helyiségeket vagy "
                "törölheted a meglévőket.</p>";

        if (customRooms.length() > 0)
        {
            int s = 0, e;
            do
            {
                e = customRooms.indexOf('\n', s);
                String cr = (e >= 0) ? customRooms.substring(s, e) : customRooms.substring(s);
                cr.trim();
                if (cr.length() > 0)
                {
                    html += "<span class=\"room-tag\">" + cr + " <a href=\"/delroom?name=" + urlEncode(cr) +
                            "\" style=\"color:#f85149;text-decoration:none;font-weight:bold\">&times;</a></span>";
                }
                s = e + 1;
            } while (e >= 0);
        }
        else
        {
            html += "<p class=\"note\" style=\"color:#484f58\">Még nincs egyéni helyiség hozzáadva.</p>";
        }

        html += "<div class=\"room-add\">";
        html += "<input id=\"newRoom\" placeholder=\"Új helyiség neve\">";
        html += "<button type=\"button\" "
                "onclick=\"location='/"
                "addroom?name='+encodeURIComponent(document.getElementById('newRoom').value)\">Hozzáadás</button>";
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

    WS->send(200, "text/html", html);
}

// ─── SAVE PINS HANDLER ─────────────────────────────────────────
static void handleSavePins()
{
    if (!web_auth_ok())
        return;
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);

    // Partial update — only write if present in POST (D4 bug prevention)
    if (WS->hasArg("prx"))
        nv.putInt(NV_KEY_PIN_RS485_RX, WS->arg("prx").toInt());
    if (WS->hasArg("ptx"))
        nv.putInt(NV_KEY_PIN_RS485_TX, WS->arg("ptx").toInt());
    if (WS->hasArg("pde"))
        nv.putInt(NV_KEY_PIN_RS485_DE, WS->arg("pde").toInt());
    if (WS->hasArg("pled"))
        nv.putInt(NV_KEY_PIN_LED, WS->arg("pled").toInt());
    if (WS->hasArg("pbtn"))
        nv.putInt(NV_KEY_PIN_BTN, WS->arg("pbtn").toInt());
    if (WS->hasArg("pemosi"))
        nv.putInt(NV_KEY_PIN_ETH_MOSI, WS->arg("pemosi").toInt());
    if (WS->hasArg("pemiso"))
        nv.putInt(NV_KEY_PIN_ETH_MISO, WS->arg("pemiso").toInt());
    if (WS->hasArg("pesclk"))
        nv.putInt(NV_KEY_PIN_ETH_SCLK, WS->arg("pesclk").toInt());
    if (WS->hasArg("pecs"))
        nv.putInt(NV_KEY_PIN_ETH_CS, WS->arg("pecs").toInt());
    if (WS->hasArg("peint"))
        nv.putInt(NV_KEY_PIN_ETH_INT, WS->arg("peint").toInt());
    if (WS->hasArg("perst"))
        nv.putInt(NV_KEY_PIN_ETH_RST, WS->arg("perst").toInt());
#ifdef USE_SD
    // SD Card config
    if (WS->hasArg("sdena"))
        nv.putBool(NV_KEY_SD_EN, WS->arg("sdena").toInt() != 0);
    if (WS->hasArg("psdcs"))
        nv.putInt(NV_KEY_PIN_SD_CS, WS->arg("psdcs").toInt());
#endif

    nv.end();
    LOG_ILN("[WEB] Pin config saved");

    // Also persist pins to /active/pins.json for NVS recovery after flash
#ifdef USE_STORAGE
    {
        JsonDocument doc;
        // Read from POST params (more current than cfg struct before reboot)
        doc["pin_rs485_rx"]   = WS->hasArg("prx")    ? WS->arg("prx").toInt()    : cfg.pin_rs485_rx;
        doc["pin_rs485_tx"]   = WS->hasArg("ptx")    ? WS->arg("ptx").toInt()    : cfg.pin_rs485_tx;
        doc["pin_rs485_de"]   = WS->hasArg("pde")    ? WS->arg("pde").toInt()    : cfg.pin_rs485_de;
        doc["pin_status_led"] = WS->hasArg("pled")   ? WS->arg("pled").toInt()   : cfg.pin_status_led;
        doc["pin_config_btn"] = WS->hasArg("pbtn")   ? WS->arg("pbtn").toInt()   : cfg.pin_config_btn;
        doc["pin_eth_mosi"]   = WS->hasArg("pemosi") ? WS->arg("pemosi").toInt() : cfg.pin_eth_mosi;
        doc["pin_eth_miso"]   = WS->hasArg("pemiso") ? WS->arg("pemiso").toInt() : cfg.pin_eth_miso;
        doc["pin_eth_sclk"]   = WS->hasArg("pesclk") ? WS->arg("pesclk").toInt() : cfg.pin_eth_sclk;
        doc["pin_eth_cs"]     = WS->hasArg("pecs")   ? WS->arg("pecs").toInt()    : cfg.pin_eth_cs;
        doc["pin_eth_int"]    = WS->hasArg("peint")  ? WS->arg("peint").toInt()   : cfg.pin_eth_int;
        doc["pin_eth_rst"]    = WS->hasArg("perst")  ? WS->arg("perst").toInt()   : cfg.pin_eth_rst;
        doc["pin_sd_cs"]      = WS->hasArg("psdcs")  ? WS->arg("psdcs").toInt()   : cfg.pin_sd_cs;
        String json;
        serializeJson(doc, json);
        storage_write_file("/active/pins.json", json.c_str(), json.length());
        LOG_ILN("[WEB] Pin config also saved to /active/pins.json");
    }
#endif

    // Update config CRC after NVRAM write
    config_write_crc();

    WS->send(200, "text/html",
             pageStart(F("Pinek elmentve"), CSS_MINIMAL) + pageStyleEnd() +
             F("<h1 class=\"ok\">&#10004; Pinek elmentve!</h1>"
               "<p>A GPIO pinek újraindítás után lépnek érvénybe.</p>"
               "<p>Az eszköz újraindul 3 másodperc múlva...</p>"
               "<script>setTimeout(function(){window.location='/'},5000)</script>"
               "</body></html>"));

    delay(3000);
    eth_hard_reset_and_restart();
}

// ─── SAVE MODULES HANDLER ──────────────────────────────────────
static void handleSaveModules()
{
    if (!web_auth_ok())
        return;
    // Iterate all module slave addresses and save their names + area + entity names
    for (uint16_t i = 0; i < module_count; i++)
    {
        Slave_Module &m = modules[i];
        String mn_key = "mn" + String(m.slave_addr);
        String hn_key = "hn" + String(m.slave_addr);
        String ar_key = "ar" + String(m.slave_addr);
        String mn_val = WS->arg(mn_key);
        String hn_val = WS->arg(hn_key);
        String ar_val = WS->arg(ar_key);

        config_save_module_name(m.slave_addr, mn_val.c_str(), hn_val.c_str());

        // Save area — if "_other", use the free text field
        if (ar_val == "_other")
        {
            String other_key = ar_key + "_other";
            String other_val = WS->arg(other_key);
            other_val.trim();
            if (other_val.length() > 0)
            {
                config_save_module_area(m.slave_addr, other_val.c_str());
                // Add to custom rooms list if not already there
                Preferences pnv;
                pnv.begin(NV_NAMESPACE, false);
                String existing = pnv.getString(NV_KEY_ROOMS, "");
                pnv.end();
                bool found = false;
                if (existing.length() > 0)
                {
                    int s = 0, e;
                    do
                    {
                        e = existing.indexOf('\n', s);
                        String cr = (e >= 0) ? existing.substring(s, e) : existing.substring(s);
                        cr.trim();
                        if (cr == other_val)
                        {
                            found = true;
                            break;
                        }
                        s = e + 1;
                    } while (e >= 0);
                }
                if (!found)
                {
                    if (existing.length() > 0)
                        existing += "\n";
                    existing += other_val;
                    pnv.begin(NV_NAMESPACE, false);
                    pnv.putString(NV_KEY_ROOMS, existing);
                    pnv.end();
                }
            }
        }
        else
        {
            config_save_module_area(m.slave_addr, ar_val.c_str());
        }

        // Save per-relay names (hasArg guard: D4 fix)
        for (uint8_t r = 0; r < HA_V2_RELAY_COUNT; r++)
        {
            String rn_key = "rn" + String(m.slave_addr) + "_" + String(r);
            if (WS->hasArg(rn_key))
            {
                String rn_val = WS->arg(rn_key);
                config_save_relay_name(m.slave_addr, r, rn_val.c_str());
            }
        }

        // Save per-DI names (hasArg guard: D4 fix)
        for (uint8_t d = 0; d < HA_V2_DI_COUNT; d++)
        {
            String dn_key = "dn" + String(m.slave_addr) + "_" + String(d);
            if (WS->hasArg(dn_key))
            {
                String dn_val = WS->arg(dn_key);
                config_save_di_name(m.slave_addr, d, dn_val.c_str());
            }
        }
    }

    // Re-publish HA discovery with updated names
    if (mqtt_is_connected())
    {
        for (uint16_t i = 0; i < module_count; i++)
        {
            if (modules[i].discovered && modules[i].online)
            {
                mqtt_publish_discovery(&modules[i]);
            }
        }
    }

    WS->send(200, "text/html",
             pageStart(F("Modul nevek elmentve"), CSS_MINIMAL) + pageStyleEnd() +
             F("<h1 class=\"ok\">&#10004; Modul nevek elmentve!</h1>"
               "<p>A modul nevek, szoba, relé/DI nevek mentve lettek.</p>"
               "<p>HA discovery frissítve.</p>"
               "<script>setTimeout(function(){window.location='/modules'},2000)</script>"
               "</body></html>"));
}

// ─── RESCAN HANDLER ──────────────────────────────────────────────
// ─── Simple URL encode helper ──────────────────────────────────
static String urlEncode(const String &s)
{
    String enc;
    enc.reserve(s.length() * 3);
    for (unsigned i = 0; i < s.length(); i++)
    {
        char c = s.charAt(i);
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            enc += c;
        }
        else
        {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
            enc += hex;
        }
    }
    return enc;
}

// ─── Add custom room ───────────────────────────────────────────
static void handleAddRoom()
{
    if (!web_auth_ok())
        return;
    String name = WS->arg("name");
    name.trim();
    if (name.length() == 0)
    {
        WS->sendHeader("Location", "/modules");
        WS->send(302);
        return;
    }

    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    String existing = nv.getString(NV_KEY_ROOMS, "");

    // Check duplicate
    bool dup = false;
    if (existing.length() > 0)
    {
        int s = 0, e;
        do
        {
            e = existing.indexOf('\n', s);
            String cr = (e >= 0) ? existing.substring(s, e) : existing.substring(s);
            cr.trim();
            if (cr == name)
            {
                dup = true;
                break;
            }
            s = e + 1;
        } while (e >= 0);
    }
    // Also check default rooms
    if (!dup)
    {
        dup = std::any_of(
                DEFAULT_ROOMS, DEFAULT_ROOMS + DEFAULT_ROOM_COUNT, [&](const char *dr) { return name == dr; });
    }

    if (!dup)
    {
        // Count existing rooms
        int count = 0;
        if (existing.length() > 0)
        {
            int s = 0, e;
            do
            {
                e = existing.indexOf('\n', s);
                String cr = (e >= 0) ? existing.substring(s, e) : existing.substring(s);
                cr.trim();
                if (cr.length() > 0)
                    count++;
                s = e + 1;
            } while (e >= 0);
        }
        if (count < MAX_ROOMS)
        {
            if (existing.length() > 0)
                existing += "\n";
            existing += name;
            nv.putString(NV_KEY_ROOMS, existing);
        }
    }
    nv.end();
    WS->sendHeader("Location", "/modules");
    WS->send(302);
}

// ─── Delete custom room ────────────────────────────────────────
static void handleDelRoom()
{
    if (!web_auth_ok())
        return;
    String name = WS->arg("name");
    name.trim();
    if (name.length() == 0)
    {
        WS->sendHeader("Location", "/modules");
        WS->send(302);
        return;
    }

    // Don't allow deleting default rooms
    if (std::any_of(DEFAULT_ROOMS, DEFAULT_ROOMS + DEFAULT_ROOM_COUNT, [&](const char *dr) { return name == dr; }))
    {
        WS->sendHeader("Location", "/modules");
        WS->send(302);
        return;
    }

    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    String existing = nv.getString(NV_KEY_ROOMS, "");

    String newList = "";
    if (existing.length() > 0)
    {
        int s = 0, e;
        do
        {
            e = existing.indexOf('\n', s);
            String cr = (e >= 0) ? existing.substring(s, e) : existing.substring(s);
            cr.trim();
            if (cr.length() > 0 && cr != name)
            {
                if (newList.length() > 0)
                    newList += "\n";
                newList += cr;
            }
            s = e + 1;
        } while (e >= 0);
    }
    nv.putString(NV_KEY_ROOMS, newList);
    nv.end();
    WS->sendHeader("Location", "/modules");
    WS->send(302);
}

// ─── API: scan status (JSON) ────────────────────────────────────
static void handleApiScan()
{
    if (!web_auth_ok())
        return;
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
    WS->send(200, "application/json", json);
}

// ─── Toggle virtual module ON/OFF ────────────────────────────────
static void handleToggleVMod()
{
    if (!web_auth_ok())
        return;
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    bool cur = nv.getBool(NV_KEY_VIRTUAL_MOD, false);
    nv.putBool(NV_KEY_VIRTUAL_MOD, !cur);
    nv.end();

    if (!cur)
    {
        // Just enabled — trigger rescan to insert virtual module
        scan_modbus_start();
        scanning_done = false;
        WS->sendHeader("Location", "/modules");
        WS->send(302);
    }
    else
    {
        // Just disabled — remove virtual module from runtime and clean MQTT
        for (uint16_t i = 0; i < module_count; i++)
        {
            if (modules[i].is_virtual)
            {
                // Clean up MQTT entities for this module
                if (mqtt_is_connected())
                {
                    mqtt_cleanup_discovery(&modules[i]);
                }
                // Shift remaining modules down
                for (uint16_t j = i; j < module_count - 1; j++)
                {
                    modules[j] = modules[j + 1];
                }
                module_count--;
                break;
            }
        }
        WS->sendHeader("Location", "/modules");
        WS->send(302);
    }
}

static void handleRescan()
{
    if (!web_auth_ok())
        return;
    config_clear_module_list(); // Clear saved list on rescan
    scan_modbus_start();
    scanning_done = false;
    LOG_ILN("[WEB] Rescan triggered from web UI (saved list cleared)");

    WS->send(200, "text/html", pageStart(F("Buszkeresés"), CSS_MINIMAL) + pageStyleEnd() +
             F("<h1 class=\"ok\">&#128260; Buszkeresés elindítva...</h1>"
               "<p>A Modbus busz újraszkennelése folyamatban.</p>"
               "<script>setTimeout(function(){window.location='/modules'},3000)</script>"
               "</body></html>"));
}

static void handleSaveModList()
{
    if (!web_auth_ok())
        return;
    config_save_module_list();
    WS->send(200, "text/html", pageStart(F("Mentés"), CSS_MINIMAL) + pageStyleEnd() +
             F("<h1 class=\"ok\">&#128190; Modullista elmentve!</h1>"
               "<p>A következő bootnál a bridge azonnal betölti a modulokat (scan kihagyása).</p>"
               "<p class=\"note\">Újrascan törli a mentett listát.</p>"
               "<script>setTimeout(function(){window.location='/modules'},2000)</script>"
               "</body></html>"));
}

// ─── JSON API Endpoints ─────────────────────────────────────────
static void handleApiStatus()
{
    if (!web_auth_ok())
        return;
    JsonDocument doc(PsramAllocator::instance());
    doc["hostname"] = cfg.hostname;
    doc["firmware"] = FIRMWARE_VERSION;
    doc["uptime_s"] = millis() / 1000;
    doc["heap_free"] = ESP.getFreeHeap();
    doc["heap_free_kb"] = ESP.getFreeHeap() / 1024;
    doc["psram_free"] = psram_free();
    doc["psram_total"] = psram_total();
    doc["wdt_reboots"] = wdt_get_reboots();
    doc["interface"] = cfg.active_if == NET_IF_LAN ? "LAN" : cfg.active_if == NET_IF_WIFI ? "WiFi" : "NONE";
    doc["ip"] = active_ip;
    // Dual-stack: always show both interfaces
    doc["wifi_ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "0.0.0.0";
    doc["wifi_rssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
    doc["lan_started"] = eth_is_started();
    doc["lan_connected"] = eth_is_connected();
    if (eth_is_started())
    {
        doc["lan_ip"] = eth_get_ip();
    }
    else
    {
        doc["lan_ip"] = "0.0.0.0";
    }
    doc["mqtt_connected"] = mqtt_is_connected();
    doc["mqtt_transport"] = mqtt_is_on_lan() ? "LAN" : "WiFi";
    doc["mqtt_tls"] = cfg.mqtt_tls;
    // SD Card status
    doc["sd_enabled"] = cfg.sd_enabled;
    doc["sd_ok"] = sd_is_ok();
    doc["sd_mode"] = sd_is_sdio_mode() ? "sdio_1bit" : "spi";
    if (sd_is_ok())
    {
        doc["sd_type"] = sd_type_str();
        doc["sd_total_kb"] = sd_total_kb();
        doc["sd_used_kb"] = sd_used_kb();
    }
    doc["modules"] = module_count;
    if (module_count > 0)
    {
        JsonArray mods = doc["module_list"].to<JsonArray>();
        for (uint16_t i = 0; i < module_count; i++)
        {
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
    WS->send(200, "application/json", payload);
}

static void handleApiConfig()
{
    if (!web_auth_ok())
        return;
    JsonDocument doc(PsramAllocator::instance());
    doc["hostname"] = cfg.hostname;
    doc["wifi_ssid"] = cfg.wifi_ssid;
    doc["wifi_mode"] = cfg.wifi_mode;
    doc["lan_type"] = cfg.lan_type;
    doc["lan_enabled"] = cfg.lan_enabled;
    doc["lan_dhcp"] = cfg.lan_dhcp;
    doc["mqtt_host"] = cfg.mqtt_host;
    doc["mqtt_port"] = cfg.mqtt_port;
    doc["mqtt_tls"] = cfg.mqtt_tls;
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
    WS->send(200, "application/json", payload);
}

static void handleApiModules()
{
    if (!web_auth_ok())
        return;
    JsonDocument doc(PsramAllocator::instance());
    JsonArray arr = doc.to<JsonArray>();
    for (uint16_t i = 0; i < module_count; i++)
    {
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
        if (modules[i].model.RELAY_COUNT > 0)
        {
            JsonObject relays = m["relays"].to<JsonObject>();
            for (uint8_t r = 0; r < modules[i].model.RELAY_COUNT; r++)
            {
                relays[String(r)] = modules[i].relays[r].state ? "ON" : "OFF";
            }
        }
        // DI states
        if (modules[i].model.DI_COUNT > 0)
        {
            JsonObject inputs = m["inputs"].to<JsonObject>();
            for (uint8_t d = 0; d < modules[i].model.DI_COUNT; d++)
            {
                JsonObject di_obj = inputs["d" + String(d)].to<JsonObject>();
                di_obj["state"] = modules[i].inputs[d].current ? "ON" : "OFF";
                di_obj["type"] = di_input_type_str(modules[i].inputs[d].detected_type);
                di_obj["samples"] = modules[i].inputs[d].sample_count;
                di_obj["momentary_pct"] = modules[i].inputs[d].sample_count > 0
                    ? (uint8_t)((uint32_t)modules[i].inputs[d].momentary_votes * 100 / modules[i].inputs[d].sample_count)
                    : 0;
                di_obj["last_press_ms"] = modules[i].inputs[d].last_press_duration;
            }
            // DI→Relay mapping (legacy)
            JsonArray drmap = m["di_relay_map"].to<JsonArray>();
            for (uint8_t d = 0; d < modules[i].model.DI_COUNT; d++)
                drmap.add(modules[i].di_relay_map[d]);
            // DI→Edge mapping (v2.8+)
            JsonObject emap = m["di_edge_map"].to<JsonObject>();
            for (uint8_t d = 0; d < modules[i].model.DI_COUNT; d++)
            {
                JsonObject e = emap["d" + String(d)].to<JsonObject>();
                e["relay"] = modules[i].di_edge_map[d].relay;
                e["rising"] = modules[i].di_edge_map[d].rising_action;
                e["falling"] = modules[i].di_edge_map[d].falling_action;
            }
        }
    }
    String payload;
    serializeJson(doc, payload);
    WS->send(200, "application/json", payload);
}

// ─── DI→Relay mapping API ────────────────────────────────────
// POST /api/direlay?addr=S200&d0=0&d1=255&d2=1&d3=255&d4=255&d5=255
// dN = relay index (0-5) or 255 = no mapping
static void handleApiDiRelay()
{
    if (!web_auth_ok())
        return;
    if (!WS->hasArg("addr"))
    {
        WS->send(400, "text/plain", "Missing addr");
        return;
    }
    uint8_t addr = (uint8_t)WS->arg("addr").toInt();
    Slave_Module *mod = nullptr;
    for (uint16_t i = 0; i < module_count; i++)
    {
        if (modules[i].slave_addr == addr && modules[i].active)
        {
            mod = &modules[i];
            break;
        }
    }
    if (!mod)
    {
        WS->send(404, "text/plain", "Module not found");
        return;
    }
    // Update DI→Relay mapping
    for (uint8_t d = 0; d < HA_V2_DI_COUNT; d++)
    {
        String key = "d" + String(d);
        if (WS->hasArg(key))
        {
            int val = WS->arg(key).toInt();
            mod->di_relay_map[d] = (uint8_t)constrain(val, 0, 255);
        }
    }
    config_save_module_list(); // Persist to NVRAM
    LOG_I("[API] DI→Relay map saved for S%d\n", addr);
    WS->send(200, "application/json", "{\"ok\":true}");
}

// ─── DI→Edge Event mapping API (v2.8+) ──────────────────────────
// POST /api/diedge?addr=S200&d0_r=R1&d0_ra=1&d0_fa=2&d1_r=255&...
// dN_r = target relay (0-5 or 255=none), dN_ra = rising action (0-3), dN_fa = falling action (0-3)
// Actions: 0=NONE, 1=ON, 2=OFF, 3=TOGGLE
static void handleApiDiEdge()
{
    if (!web_auth_ok())
        return;
    if (!WS->hasArg("addr"))
    {
        WS->send(400, "text/plain", "Missing addr");
        return;
    }
    uint8_t addr = (uint8_t)WS->arg("addr").toInt();
    Slave_Module *mod = nullptr;
    for (uint16_t i = 0; i < module_count; i++)
    {
        if (modules[i].slave_addr == addr && modules[i].active)
        {
            mod = &modules[i];
            break;
        }
    }
    if (!mod)
    {
        WS->send(404, "text/plain", "Module not found");
        return;
    }
    // Update DI→Edge mapping
    bool changed = false;
    for (uint8_t d = 0; d < HA_V2_DI_COUNT; d++)
    {
        String rkey = "d" + String(d) + "_r";
        String rakey = "d" + String(d) + "_ra";
        String fakey = "d" + String(d) + "_fa";

        if (WS->hasArg(rkey))
        {
            int rv = WS->arg(rkey).toInt();
            mod->di_edge_map[d].relay = (uint8_t)constrain(rv, 0, 255);
            changed = true;
        }
        if (WS->hasArg(rakey))
        {
            int ra = WS->arg(rakey).toInt();
            mod->di_edge_map[d].rising_action = (uint8_t)constrain(ra, 0, 3);
            changed = true;
        }
        if (WS->hasArg(fakey))
        {
            int fa = WS->arg(fakey).toInt();
            mod->di_edge_map[d].falling_action = (uint8_t)constrain(fa, 0, 3);
            changed = true;
        }
    }
    if (changed)
    {
        config_save_module_list(); // Persist to NVRAM
        LOG_I("[API] DI→Edge map saved for S%d\n", addr);
    }
    WS->send(200, "application/json", "{\"ok\":true}");
}

// ─── SD Register List API ───────────────────────────────────────
// POST /api/sd/save  — save register list JSON to SD
//   ?device=NIBE_S1156&json={...}
// GET  /api/sd/list  — list device register files on SD
// GET  /api/sd/read  — read register list from SD
//   ?device=NIBE_S1156
// DELETE /api/sd/del  — delete register list from SD
//   ?device=NIBE_S1156

static void handleApiSdSave()
{
    if (!web_auth_ok()) return;
    if (!sd_is_ok())
    {
        WS->send(503, "application/json", "{\"error\":\"SD not available\"}");
        return;
    }
    if (!WS->hasArg("device") || !WS->hasArg("json"))
    {
        WS->send(400, "application/json", "{\"error\":\"missing device or json\"}");
        return;
    }
    String device = WS->arg("device");
    String json = WS->arg("json");
    // Sanitize device name: only alphanumeric + underscore
    for (uint8_t i = 0; i < device.length(); i++)
    {
        char c = device[i];
        if (!isalnum(c) && c != '_')
        {
            WS->send(400, "application/json", "{\"error\":\"invalid device name\"}");
            return;
        }
    }
    bool ok = sd_save_register_list(device.c_str(), json.c_str(), json.length());
    if (ok)
        WS->send(200, "application/json", "{\"ok\":true}");
    else
        WS->send(500, "application/json", "{\"error\":\"write failed\"}");
}

static void handleApiSdList()
{
    if (!web_auth_ok()) return;
    if (!sd_is_ok())
    {
        WS->send(503, "application/json", "{\"error\":\"SD not available\"}");
        return;
    }
    size_t len = 0;
    char *buf = sd_list_register_files(&len);
    if (!buf || len == 0)
    {
        WS->send(200, "application/json", "[]");
        if (buf) free(buf);
        return;
    }
    WS->send(200, "application/json", buf);
    free(buf);
}

static void handleApiSdRead()
{
    if (!web_auth_ok()) return;
    if (!sd_is_ok())
    {
        WS->send(503, "application/json", "{\"error\":\"SD not available\"}");
        return;
    }
    if (!WS->hasArg("device"))
    {
        WS->send(400, "application/json", "{\"error\":\"missing device\"}");
        return;
    }
    String device = WS->arg("device");
    size_t len = 0;
    char *buf = sd_read_register_list(device.c_str(), &len);
    if (!buf)
    {
        WS->send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }
    WS->send(200, "application/json", buf);
    free(buf);
}

static void handleApiSdDel()
{
    if (!web_auth_ok()) return;
    if (!sd_is_ok())
    {
        WS->send(503, "application/json", "{\"error\":\"SD not available\"}");
        return;
    }
    if (!WS->hasArg("device"))
    {
        WS->send(400, "application/json", "{\"error\":\"missing device\"}");
        return;
    }
    String device = WS->arg("device");
    bool ok = sd_delete_register_list(device.c_str());
    if (ok)
        WS->send(200, "application/json", "{\"ok\":true}");
    else
        WS->send(404, "application/json", "{\"error\":\"not found\"}");
}

// ─── SD Enable/Init API ────────────────────────────────────────
// POST /api/sd/toggle  — enable/disable + init/deinit (SPI only, safe)
//   ?enabled=1 or ?enabled=0
static void handleApiSdToggle()
{
    if (!web_auth_ok()) return;
    if (!WS->hasArg("enabled"))
    {
        WS->send(400, "application/json", "{\"error\":\"missing enabled\"}");
        return;
    }
    bool en = WS->arg("enabled").toInt() != 0;
    // Save to NVRAM
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    nv.putBool(NV_KEY_SD_EN, en);
    nv.end();
    cfg.sd_enabled = en;
    if (en)
    {
        // SAFE: SPI only by default (no crash risk)
        if (sd_init(cfg.pin_sd_cs))
        {
            LOG_I("[API] SD enabled + initialized (SPI)\n");
            WS->send(200, "application/json", "{\"ok\":true,\"sd_ok\":true,\"mode\":\"spi\"}");
        }
        else
        {
            LOG_E("[API] SD enabled but SPI init FAILED\n");
            WS->send(200, "application/json", "{\"ok\":true,\"sd_ok\":false,\"mode\":\"spi\"}");
        }
    }
    else
    {
        sd_deinit();
        LOG_I("[API] SD disabled\n");
        WS->send(200, "application/json", "{\"ok\":true,\"sd_ok\":false}");
    }
}

// POST /api/sd/init  — init SD with explicit mode (spi / sdio / auto)
//   ?mode=sdio    = SDIO 1-bit only (NO CS needed)
//   ?mode=spi     = SPI only (needs CS)
//   ?mode=auto    = try SDIO first, then SPI
static void handleApiSdInit()
{
    if (!web_auth_ok()) return;
    String mode = WS->arg("mode");
    if (mode.length() == 0) mode = "spi";
    if (mode != "spi" && mode != "sdio" && mode != "auto")
    {
        WS->send(400, "application/json", "{\"error\":\"invalid mode, use: spi, sdio, auto\"}");
        return;
    }

    if (sd_is_ok())
    {
        sd_deinit();
        delay(100);
    }

    LOG_I("[API] SD init mode=%s\n", mode.c_str());
    bool ok = sd_init(cfg.pin_sd_cs, mode.c_str());

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"sd_ok\":%s,\"mode\":\"%s\",\"sdio\":%s}",
             ok ? "true" : "false",
             ok ? "true" : "false",
             mode.c_str(),
             sd_is_sdio_mode() ? "true" : "false");
    WS->send(200, "application/json", buf);
}

// ─── SD Card Browser Page ────────────────────────────────────
static void handleSdCard()
{
    if (!web_auth_ok())
        return;

    // ── Enter SD exclusive mode (pauses Modbus if pin conflict) ──
    bool exclusiveOk = sd_begin_exclusive();
    bool inExclusive = sd_is_exclusive();

    String authSuf = WS->hasArg("auth") ? ("?auth=" + WS->arg("auth")) : "";

    String html;
    html.reserve(8000);
    html = pageStart(F("Modbus-MQTT Bridge — SD Kártya"), CSS_SD) + pageStyleEnd();
    html += F("<h1>&#128190; SD Kártya</h1>");
    html += navHtml(PG_SD, authSuf);

    // ── Exclusive mode banner ────────────────────────────────
    if (inExclusive)
    {
        html += F("<div class=\"sd-exclusive-banner\">&#9888;&#65039; Modbus szünetel az SD használat alatt</div>");
    }

    // ── SD init failure banner (exclusive mode failed) ────────
    if (!exclusiveOk && sd_has_pin_conflict())
    {
        html += F("<div class=\"sd-init-fail\">Nem sikerült az SD kártyát inicializálni</div>");
    }

    // ── SD Status Card ────────────────────────────────────────
    bool sdOk = sd_is_ok();
    html += F("<div class=\"card\"><h2>&#128202; Állapot</h2>");
    html += F("<div class=\"row\"><span class=\"key\">SD kártya</span><span class=\"");
    html += sdOk ? F("on\">&#10004; Aktív") : F("off\">&#10060; Nem elérhető");
    html += F("</span></div>");

    if (sdOk)
    {
        uint64_t totalKb = sd_total_kb();
        uint64_t usedKb = sd_used_kb();
        uint64_t freeKb = (totalKb > usedKb) ? (totalKb - usedKb) : 0;
        html += F("<div class=\"row\"><span class=\"key\">Típus</span><span class=\"val\">");
        html += htmlEscape(String(sd_type_str()));
        html += F("</span></div>");
        html += F("<div class=\"row\"><span class=\"key\">Összes</span><span class=\"val\">");
        html += String(totalKb);
        html += F(" KB</span></div>");
        html += F("<div class=\"row\"><span class=\"key\">Használt</span><span class=\"val\">");
        html += String(usedKb);
        html += F(" KB</span></div>");
        html += F("<div class=\"row\"><span class=\"key\">Szabad</span><span class=\"val\">");
        html += String(freeKb);
        html += F(" KB</span></div>");
    }
    html += F("</div>");

    // ── SD Enable/Disable Toggle ──────────────────────────────
    html += F("<div class=\"sd-actions\">");
    if (cfg.sd_enabled)
    {
        html += F("<button onclick=\"toggleSd(0)\" class=\"btn-warn\">SD Letiltása</button>");
    }
    else
    {
        html += F("<button onclick=\"toggleSd(1)\">SD Engedélyezése</button>");
    }
    if (sdOk)
    {
        html += F("<button onclick=\"showMkdirDialog()\">&#128193; Mappa létrehozás</button>");
        html += F("<button onclick=\"showUploadDialog()\">&#128228; Fájl feltöltés</button>");
        html += F("<button onclick=\"confirmFormat()\" class=\"btn-warn\">&#128465; Formázás</button>");
    }
    html += F("</div>");

    // ── File Browser ──────────────────────────────────────────
    if (sdOk)
    {
        html += F("<h2>&#128193; Tallózás</h2>");
        html += F("<div id=\"breadcrumb\" class=\"breadcrumb\"></div>");
        html += F("<div id=\"file-list\" class=\"file-list\"></div>");
    }
    else
    {
        html += F("<div class=\"card\"><p class=\"note\">SD kártya nem elérhető. Csatlakoztass egy SD kártyát és engedélyezd a funkciót.</p></div>");
    }

    // ── Mkdir Dialog ──────────────────────────────────────────
    html += F("<div id=\"mkdir-overlay\" class=\"modal-overlay\">"
              "<div class=\"modal\">"
              "<button class=\"modal-close\" onclick=\"closeMkdirDialog()\">&times;</button>"
              "<h2>&#128193; Mappa létrehozása</h2>"
              "<div class=\"fm\"><label>Mappa neve</label>"
              "<input id=\"mkdir-name\" placeholder=\"uj_mappa\"></div>"
              "<button onclick=\"doMkdir()\">Létrehozás</button>"
              "</div></div>");

    // ── Upload Dialog ─────────────────────────────────────────
    html += F("<div id=\"upload-overlay\" class=\"modal-overlay\">"
              "<div class=\"modal\">"
              "<button class=\"modal-close\" onclick=\"closeUploadDialog()\">&times;</button>"
              "<h2>&#128228; Fájl feltöltése</h2>"
              "<form id=\"upload-form\" method=\"POST\" enctype=\"multipart/form-data\">"
              "<div class=\"upload-area\">"
              "<input type=\"file\" id=\"upload-file\" name=\"file\" style=\"width:100%;padding:8px;color:#c9d1d9;background:#0d1117;border:1px solid #30363d;border-radius:4px\">"
              "<p class=\"note\">Fájl kiválasztása, majd Készítés gomb</p>"
              "</div>"
              "<button type=\"submit\">Feltöltés</button>"
              "</form>"
              "</div></div>");

    // ── File Viewer Modal ─────────────────────────────────────
    html += F("<div id=\"viewer-overlay\" class=\"modal-overlay\">"
              "<div class=\"modal\" style=\"max-width:95vw\">"
              "<button class=\"modal-close\" onclick=\"closeViewer()\">&times;</button>"
              "<h2 id=\"viewer-title\">Fájl tartalma</h2>"
              "<pre id=\"viewer-content\"></pre>"
              "</div></div>");

    // ── Confirm Dialog ────────────────────────────────────────
    html += F("<div id=\"confirm-overlay\" class=\"confirm-overlay\">"
              "<div class=\"confirm-box\">"
              "<p id=\"confirm-msg\">Biztosan?</p>"
              "<div class=\"btn-row\">"
              "<button onclick=\"confirmAction()\">Igen</button>"
              "<button onclick=\"cancelAction()\">Mégsem</button>"
              "</div></div></div>");

    // ── JavaScript ────────────────────────────────────────────
    html += F("<script>"
              "var curPath='/';var pendingFn=null;"
              "var authSuf='");
    html += authSuf;
    html += F("';"
              "function authQ(){return authSuf?('&'+authSuf.substring(1)):'';}"
              "");

    // browse
    html += F("function browse(p){"
              "curPath=p;"
              "fetch('/api/sd/browse?path='+encodeURIComponent(p)+authQ())"
              ".then(r=>r.json())"
              ".then(d=>{"
              "renderBreadcrumb(d.path);"
              "renderFiles(d.path,d.entries);"
              "}).catch(e=>{document.getElementById('file-list').innerHTML='<p class=\"note\">Hiba: '+e+'</p>';});"
              "}");

    // render breadcrumb
    html += F("function renderBreadcrumb(path){"
              "var bc=document.getElementById('breadcrumb');"
              "var parts=path.split('/').filter(Boolean);"
              "var html='<a href=\"#\" onclick=\"browse(\\'/\\');return false;\">&#128193; /</a>';"
              "var cur='/';"
              "for(var i=0;i<parts.length;i++){"
              "cur+='/'+parts[i];"
              "html+='<span>/</span><a href=\"#\" onclick=\"browse(\\''+cur+'\\');return false;\">'+parts[i]+'</a>';"
              "}"
              "bc.innerHTML=html;"
              "}");

    // render files
    html += F("function renderFiles(path,entries){"
              "var el=document.getElementById('file-list');"
              "if(!entries||entries.length==0){el.innerHTML='<p class=\"note\">Üres mappa</p>';return;}"
              "var html='';"
              "for(var i=0;i<entries.length;i++){"
              "var e=entries[i];"
              "var fullPath=(path=='/'?'/':path+'/')+e.name;"
              "html+='<div class=\"file-entry\">';"
              "html+='<div class=\"file-info\">';"
              "if(e.is_dir){"
              "html+='<span class=\"dir-icon\">&#128193;</span>';"
              "html+='<a class=\"file-name\" href=\"#\" onclick=\"browse(\\''+fullPath+'\\');return false;\">'+e.name+'</a>';"
              "}else{"
              "html+='<span class=\"file-icon\">&#128196;</span>';"
              "var ext=e.name.split('.').pop().toLowerCase();"
              "if(ext==='json'||ext==='txt'||ext==='csv'||ext==='cfg'||ext==='log'||ext==='ini'){"
              "html+='<a class=\"file-name\" href=\"#\" onclick=\"viewFile(\\''+fullPath+'\\');return false;\">'+e.name+'</a>';"
              "}else{"
              "html+='<span class=\"file-name\">'+e.name+'</span>';"
              "}"
              "html+='<span class=\"file-size\">'+formatSize(e.size)+'</span>';"
              "}"
              "html+='</div>';"
              "html+='<div class=\"file-actions\">';"
              "html+='<button class=\"btn-del\" onclick=\"confirmDelete(\\''+fullPath+'\\',\\''+e.name+'\\')\">Törlés</button>';"
              "html+='</div></div>';"
              "}"
              "el.innerHTML=html;"
              "}");

    // format size
    html += F("function formatSize(s){"
              "if(s<1024)return s+' B';"
              "if(s<1048576)return (s/1024).toFixed(1)+' KB';"
              "return (s/1048576).toFixed(1)+' MB';"
              "}");

    // view file
    html += F("function viewFile(p){"
              "fetch('/api/sd/view?path='+encodeURIComponent(p)+authQ())"
              ".then(r=>{if(!r.ok)throw new Error('Nem olvasható');return r.text();})");
    html += F(".then(t=>{"
              "document.getElementById('viewer-title').textContent=p.split('/').pop();"
              "document.getElementById('viewer-content').textContent=t;"
              "document.getElementById('viewer-overlay').classList.add('active');"
              "}).catch(e=>{alert('Hiba: '+e);});"
              "}");

    html += F("function closeViewer(){document.getElementById('viewer-overlay').classList.remove('active');}");

    // confirm delete
    html += F("function confirmDelete(p,n){"
              "document.getElementById('confirm-msg').textContent='Biztosan törlöd: '+n+'?';"
              "pendingFn=function(){doDelete(p);};"
              "document.getElementById('confirm-overlay').classList.add('active');"
              "}");

    html += F("function doDelete(p){"
              "fetch('/api/sd/remove?path='+encodeURIComponent(p)+authQ(),{method:'POST'})"
              ".then(r=>r.json())"
              ".then(d=>{browse(curPath);})"
              ".catch(e=>{alert('Törlés sikertelen: '+e);});"
              "}");

    // confirm format
    html += F("function confirmFormat(){"
              "document.getElementById('confirm-msg').textContent='Biztosan formázod az SD kártyát? MINDEN adat törlődik!';"
              "pendingFn=function(){doFormat();};"
              "document.getElementById('confirm-overlay').classList.add('active');"
              "}");

    html += F("function doFormat(){"
              "fetch('/api/sd/format'+authQ(),{method:'POST'})"
              ".then(r=>r.json())"
              ".then(d=>{browse('/');})"  // Refresh the page to show emptied card
              ".catch(e=>{alert('Formázás sikertelen: '+e);});"
              "}");

    // toggle SD
    html += F("function toggleSd(en){"
              "fetch('/api/sd/toggle?enabled='+en+authQ(),{method:'POST'})"
              ".then(r=>r.json())"  // Reload page to reflect changes
              ".then(d=>{location.reload();})"  // Force reload
              ".catch(e=>{alert('Hiba: '+e);});"
              "}");

    // mkdir
    html += F("function showMkdirDialog(){document.getElementById('mkdir-overlay').classList.add('active');document.getElementById('mkdir-name').value='';document.getElementById('mkdir-name').focus();}");
    html += F("function closeMkdirDialog(){document.getElementById('mkdir-overlay').classList.remove('active');}");

    html += F("function doMkdir(){"
              "var n=document.getElementById('mkdir-name').value.trim();"
              "if(!n){alert('Adj meg nevet!');return;}"
              "var p=(curPath=='/'?'/'  : curPath+'/')+n;"
              "fetch('/api/sd/mkdir?path='+encodeURIComponent(p)+authQ(),{method:'POST'})"
              ".then(r=>r.json())"
              ".then(d=>{closeMkdirDialog();browse(curPath);})"
              ".catch(e=>{alert('Hiba: '+e);});"
              "}");

    // upload
    html += F("function showUploadDialog(){"
              "document.getElementById('upload-overlay').classList.add('active');"
              "document.getElementById('upload-form').action='/api/sd/upload?dir='+encodeURIComponent(curPath)+authQ();"
              "}");
    html += F("function closeUploadDialog(){document.getElementById('upload-overlay').classList.remove('active');}");

    // confirm/cancel
    html += F("function confirmAction(){if(pendingFn)pendingFn();pendingFn=null;document.getElementById('confirm-overlay').classList.remove('active');}");
    html += F("function cancelAction(){pendingFn=null;document.getElementById('confirm-overlay').classList.remove('active');}");

    // Init
    html += F("if(");
    html += sdOk ? F("true") : F("false");
    html += F(")browse('/');"
              "</script>");

    html += pageFoot();
    WS->send(200, "text/html", html);

    // ── End SD exclusive mode (resumes Modbus if was paused) ──
    sd_end_exclusive();
}
// GET /api/sd/browse?path=/
static void handleApiSdBrowse()
{
    if (!web_auth_ok()) return;
    // Enter SD exclusive mode for this API call
    if (!sd_begin_exclusive())
    {
        WS->send(503, "application/json", "{\"error\":\"SD not available\"}");
        return;
    }
    if (!WS->hasArg("path"))
    {
        WS->send(400, "application/json", "{\"error\":\"missing path\"}");
        sd_end_exclusive();
        return;
    }
    String path = WS->arg("path");
    // Basic path traversal protection
    if (path.indexOf("..") >= 0)
    {
        WS->send(400, "application/json", "{\"error\":\"invalid path\"}");
        sd_end_exclusive();
        return;
    }
    size_t len = 0;
    char *buf = sd_browse_dir(path.c_str(), &len);
    if (!buf || len == 0)
    {
        WS->send(200, "application/json", "{\"path\":\"/\",\"entries\":[]}");
        if (buf) free(buf);
        sd_end_exclusive();
        return;
    }
    WS->send(200, "application/json", buf);
    free(buf);
    sd_end_exclusive();
}

// ─── SD View File API ────────────────────────────────────────
// GET /api/sd/view?path=/registers/test.json
static void handleApiSdView()
{
    if (!web_auth_ok()) return;
    if (!sd_begin_exclusive())
    {
        WS->send(503, "text/plain", "SD not available");
        return;
    }
    if (!WS->hasArg("path"))
    {
        WS->send(400, "text/plain", "missing path");
        sd_end_exclusive();
        return;
    }
    String path = WS->arg("path");
    if (path.indexOf("..") >= 0)
    {
        WS->send(400, "text/plain", "invalid path");
        sd_end_exclusive();
        return;
    }
    size_t len = 0;
    char *buf = sd_read_file(path.c_str(), &len);
    if (!buf)
    {
        WS->send(404, "text/plain", "File not found");
        sd_end_exclusive();
        return;
    }
    // Determine content type from extension
    String ct = "text/plain";
    if (path.endsWith(".json")) ct = "application/json";
    else if (path.endsWith(".csv")) ct = "text/csv";
    else if (path.endsWith(".html")) ct = "text/html";

    WS->send(200, ct, buf);
    free(buf);
    sd_end_exclusive();
}

// ─── SD Remove API ───────────────────────────────────────────
// POST /api/sd/remove?path=/registers/test.json
static void handleApiSdRemove()
{
    if (!web_auth_ok()) return;
    if (!sd_begin_exclusive())
    {
        WS->send(503, "application/json", "{\"error\":\"SD not available\"}");
        return;
    }
    if (!WS->hasArg("path"))
    {
        WS->send(400, "application/json", "{\"error\":\"missing path\"}");
        sd_end_exclusive();
        return;
    }
    String path = WS->arg("path");
    if (path.indexOf("..") >= 0 || path == "/")
    {
        WS->send(400, "application/json", "{\"error\":\"invalid path\"}");
        sd_end_exclusive();
        return;
    }
    bool ok = sd_delete_path(path.c_str());
    if (ok)
        WS->send(200, "application/json", "{\"ok\":true}");
    else
        WS->send(500, "application/json", "{\"error\":\"delete failed\"}");
    sd_end_exclusive();
}

// ─── SD Mkdir API ────────────────────────────────────────────
// POST /api/sd/mkdir?path=/test_dir
static void handleApiSdMkdir()
{
    if (!web_auth_ok()) return;
    if (!sd_begin_exclusive())
    {
        WS->send(503, "application/json", "{\"error\":\"SD not available\"}");
        return;
    }
    if (!WS->hasArg("path"))
    {
        WS->send(400, "application/json", "{\"error\":\"missing path\"}");
        sd_end_exclusive();
        return;
    }
    String path = WS->arg("path");
    if (path.indexOf("..") >= 0)
    {
        WS->send(400, "application/json", "{\"error\":\"invalid path\"}");
        sd_end_exclusive();
        return;
    }
    bool ok = sd_mkdir(path.c_str());
    if (ok)
        WS->send(200, "application/json", "{\"ok\":true}");
    else
        WS->send(500, "application/json", "{\"error\":\"mkdir failed\"}");
    sd_end_exclusive();
}

// ─── SD Format API ───────────────────────────────────────────
// POST /api/sd/format
static void handleApiSdFormat()
{
    if (!web_auth_ok()) return;
    if (!sd_begin_exclusive())
    {
        WS->send(503, "application/json", "{\"error\":\"SD not available\"}");
        return;
    }
    LOG_I("[API] SD format requested\n");
    bool ok = sd_format();
    if (ok)
        WS->send(200, "application/json", "{\"ok\":true}");
    else
        WS->send(500, "application/json", "{\"error\":\"format failed\"}");
    sd_end_exclusive();
}

// ─── SD Upload API ───────────────────────────────────────────
// POST /api/sd/upload?dir=/registers  — multipart file upload
static void handleApiSdUpload()
{
    if (!web_auth_ok()) return;

    // For WiFi WebServer: use HTTPUpload to get file
    HTTPUpload &upload = web.upload();
    static String uploadPath;
    static bool upload_exclusive = false;

    if (upload.status == UPLOAD_FILE_START)
    {
        // Enter SD exclusive mode at the start of upload
        if (!sd_begin_exclusive())
        {
            WS->send(503, "application/json", "{\"error\":\"SD not available\"}");
            return;
        }
        upload_exclusive = true;

        String dir = WS->hasArg("dir") ? WS->arg("dir") : "/";
        if (dir.indexOf("..") >= 0)
        {
            WS->send(400, "application/json", "{\"error\":\"invalid dir\"}");
            sd_end_exclusive();
            upload_exclusive = false;
            return;
        }

        String filename = upload.filename;
        if (filename.length() == 0)
        {
            LOG_E("[API] SD upload: empty filename\n");
            sd_end_exclusive();
            upload_exclusive = false;
            return;
        }
        // Sanitize: keep only the base filename
        int slash = filename.lastIndexOf('/');
        int bslash = filename.lastIndexOf('\\');
        int lastSep = (slash > bslash) ? slash : bslash;
        if (lastSep >= 0) filename = filename.substring(lastSep + 1);

        uploadPath = dir;
        if (!uploadPath.endsWith("/")) uploadPath += "/";
        uploadPath += filename;

        // Remove existing file if present
        if (sd_file_exists(uploadPath.c_str()))
            sd_remove_file(uploadPath.c_str());

        LOG_I("[API] SD upload start: %s\n", uploadPath.c_str());
    }
    else if (upload.status == UPLOAD_FILE_WRITE)
    {
        // Append data to SD file via sd_handler
        if (uploadPath.length() > 0)
        {
            sd_append_file(uploadPath.c_str(), upload.buf, upload.currentSize);
        }
    }
    else if (upload.status == UPLOAD_FILE_END)
    {
        LOG_I("[API] SD upload complete: %s\n", uploadPath.c_str());
        sd_refresh_stats();
        // Return JSON response after upload complete
        WS->send(200, "application/json", "{\"ok\":true,\"path\":\"" + uploadPath + "\"}");
        uploadPath = "";
        if (upload_exclusive) { sd_end_exclusive(); upload_exclusive = false; }
    }
    else if (upload.status == UPLOAD_FILE_ABORTED)
    {
        LOG_E("[API] SD upload aborted\n");
        // Clean up partial file
        if (uploadPath.length() > 0 && sd_file_exists(uploadPath.c_str()))
            sd_remove_file(uploadPath.c_str());
        uploadPath = "";
        if (upload_exclusive) { sd_end_exclusive(); upload_exclusive = false; }
    }
}

// ─── Full Config Export API ──────────────────────────────────
// GET /api/export — dumps ALL NVRAM config as JSON (for backup)
static void handleApiLan()
{
    if (!web_auth_ok())
        return;
    JsonDocument doc(PsramAllocator::instance());
    doc["lan_enabled"] = cfg.lan_enabled;
    doc["lan_type"] = cfg.lan_type;
    doc["pin_rst"] = cfg.pin_eth_rst;
    doc["pin_cs"] = cfg.pin_eth_cs;
    doc["pin_int"] = cfg.pin_eth_int;
    doc["pin_mosi"] = cfg.pin_eth_mosi;
    doc["pin_miso"] = cfg.pin_eth_miso;
    doc["pin_sclk"] = cfg.pin_eth_sclk;

    int step = WS->hasArg("step") ? WS->arg("step").toInt() : -1;
    doc["step"] = step;

#ifdef USE_W5500
    if (step == 1)
    {
        // Step 1: Hardware reset W5500
        if (cfg.pin_eth_rst >= 0)
        {
            pinMode(cfg.pin_eth_rst, OUTPUT);
            digitalWrite(cfg.pin_eth_rst, LOW);
            delay(10);
            digitalWrite(cfg.pin_eth_rst, HIGH);
            delay(150);
            doc["step1"] = "RST_OK";
        }
        else
        {
            doc["step1"] = "RST_SKIP";
        }
    }
    else if (step == 2)
    {
        // Step 2: SPI bus init
        SPI.begin(cfg.pin_eth_sclk, cfg.pin_eth_miso, cfg.pin_eth_mosi, cfg.pin_eth_cs);
        doc["step2"] = "SPI_BEGIN_OK";
    }
    else if (step == 3)
    {
        // Step 3: Ethernet CS init + chip detect
        Ethernet.init(cfg.pin_eth_cs);
        doc["step3"] = "INIT_OK";
        doc["hw"] = Ethernet.hardwareStatus() == EthernetNoHardware ? "NO_HW"
                    : Ethernet.hardwareStatus() == EthernetW5500    ? "W5500"
                    : Ethernet.hardwareStatus() == EthernetW5200    ? "W5200"
                    : Ethernet.hardwareStatus() == EthernetW5100    ? "W5100"
                                                                    : "OTHER";
        doc["link"] = (Ethernet.linkStatus() == LinkON) ? "ON" : (Ethernet.linkStatus() == LinkOFF) ? "OFF" : "UNKNOWN";
    }
    else if (step == 4)
    {
        // Step 4: Try DHCP (5s timeout)
        int result = Ethernet.begin(lan_mac, 5000, 2000);
        doc["step4_dhcp"] = result;
        if (result == 0)
        {
            doc["step4_status"] = "DHCP_FAILED";
        }
        else
        {
            doc["step4_status"] = "DHCP_OK";
            doc["ip"] = Ethernet.localIP().toString();
            eth_set_connected(true);
        }
        eth_set_started(true);
        doc["hw"] = Ethernet.hardwareStatus() == EthernetNoHardware ? "NO_HW"
                    : Ethernet.hardwareStatus() == EthernetW5500    ? "W5500"
                                                                    : "OTHER";
        doc["link"] = (Ethernet.linkStatus() == LinkON) ? "ON" : "OFF";
    }
#else
    doc["error"] = "USE_W5500 not defined";
#endif

    String payload;
    serializeJson(doc, payload);
    WS->send(200, "application/json", payload);
}

// ─── W5500 Diagnostics API ─────────────────────────────────
// GET /api/diag — comprehensive W5500 + SD diagnostics
static void handleApiSdGpio()
{
    if (!web_auth_ok()) return;
#ifdef USE_SD
    String result = sd_gpio_diag();
    WS->send(200, "application/json", result);
#else
    WS->send(200, "application/json", "{\"error\":\"SD not compiled\"}");
#endif
}

static void handleApiSdTest()
{
    if (!web_auth_ok()) return;
#ifdef USE_SD
    String result = sd_test_init();
    WS->send(200, "application/json", result);
#else
    WS->send(200, "application/json", "{\"error\":\"SD not compiled\"}");
#endif
}

static void handleApiDiag()
{
    if (!web_auth_ok())
        return;
    JsonDocument doc(PsramAllocator::instance());

    // ── System ──
    doc["fw"] = FIRMWARE_VERSION;
    doc["uptime_s"] = millis() / 1000;
    doc["heap_free"] = ESP.getFreeHeap();
    doc["psram_free"] = ESP.getFreePsram();
    doc["psram_total"] = ESP.getPsramSize();
    doc["wdt_reboots"] = wdt_get_reboots();

    // ── WiFi ──
    doc["wifi_ip"] = WiFi.localIP().toString();
    doc["wifi_rssi"] = WiFi.RSSI();

    // ── W5500 Config (what firmware THINKS the pins are) ──
    JsonObject pins = doc.createNestedObject("w5500_pins");
    pins["mosi"] = cfg.pin_eth_mosi;
    pins["miso"] = cfg.pin_eth_miso;
    pins["sclk"] = cfg.pin_eth_sclk;
    pins["cs"] = cfg.pin_eth_cs;
    pins["int"] = cfg.pin_eth_int;
    pins["rst"] = cfg.pin_eth_rst;

    // ── W5500 Runtime status ──
    doc["lan_enabled"] = cfg.lan_enabled;
    doc["lan_type"] = cfg.lan_type;
    doc["lan_dhcp"] = cfg.lan_dhcp;
    doc["lan_started"] = eth_is_started();
    doc["lan_connected"] = eth_is_connected();

#ifdef USE_W5500
    // ── W5500 Hardware Status ──
    int hw = Ethernet.hardwareStatus();
    switch (hw)
    {
    case EthernetNoHardware: doc["hw_status"] = "NO_HARDWARE"; break;
    case EthernetW5100:      doc["hw_status"] = "W5100"; break;
    case EthernetW5200:      doc["hw_status"] = "W5200"; break;
    case EthernetW5500:      doc["hw_status"] = "W5500"; break;
    default:                 doc["hw_status"] = hw; break;
    }
    doc["hw_raw"] = hw;

    // ── Link Status ──
    int lnk = Ethernet.linkStatus();
    switch (lnk)
    {
    case LinkON:  doc["link_status"] = "ON"; break;
    case LinkOFF: doc["link_status"] = "OFF"; break;
    default:      doc["link_status"] = "UNKNOWN"; break;
    }
    doc["link_raw"] = lnk;

    // ── IP from W5500 ──
    doc["eth_ip"] = Ethernet.localIP().toString();
    doc["eth_subnet"] = Ethernet.subnetMask().toString();
    doc["eth_gw"] = Ethernet.gatewayIP().toString();
    doc["eth_dns"] = Ethernet.dnsServerIP().toString();
    // MAC not accessible (static in eth_handler) — read from Ethernet lib
    byte mac[6];
    Ethernet.MACAddress(mac);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    doc["eth_mac"] = mac_str;

    // ── SPI bus test: read W5500 version register ──
    // W5500 version register at 0x0000_0039 should return 0x51
    // We test by reading hardwareStatus which does SPI internally
    doc["spi_responsive"] = (hw != EthernetNoHardware);

    // ── GPIO sanity check ──
    if (cfg.pin_eth_cs >= 0)
    {
        doc["cs_gpio_read"] = digitalRead(cfg.pin_eth_cs);
    }
    if (cfg.pin_eth_rst >= 0)
    {
        doc["rst_gpio_level"] = digitalRead(cfg.pin_eth_rst);
    }
    if (cfg.pin_eth_int >= 0)
    {
        doc["int_gpio_read"] = digitalRead(cfg.pin_eth_int);
    }
#else
    doc["hw_status"] = "USE_W5500_NOTCompiled";
#endif

    // ── SD Card ──
    JsonObject sd = doc.createNestedObject("sd");
    sd["enabled"] = cfg.sd_enabled;
    sd["ok"] = sd_is_ok();
    sd["mode"] = sd_is_sdio_mode() ? "sdio_1bit" : "spi";
    sd["pin_conflict"] = sd_has_pin_conflict();
    sd["exclusive_mode"] = sd_is_exclusive();
    sd["pin_sd_cs"] = cfg.pin_sd_cs;
    sd["pin_rs485_de"] = cfg.pin_rs485_de;
    sd["gpio4_conflict"] = (cfg.pin_sd_cs == cfg.pin_rs485_de && cfg.pin_sd_cs >= 0);
    JsonObject sd_pins = sd.createNestedObject("pins");
    sd_pins["spi_sclk"] = PIN_SD_SCLK;
    sd_pins["spi_mosi"] = PIN_SD_MOSI;
    sd_pins["spi_miso"] = PIN_SD_MISO;
    sd_pins["spi_cs"] = cfg.pin_sd_cs;
    sd_pins["sdio_clk"] = PIN_SDIO_CLK;
    sd_pins["sdio_cmd"] = PIN_SDIO_CMD;
    sd_pins["sdio_d0"] = PIN_SDIO_D0;
    sd_pins["sdio_d1"] = PIN_SDIO_D1;
    if (sd_has_pin_conflict())
    {
        sd["conflict_msg"] = "SD CS és RS485 DE ugyanazon a GPIO-n!";
    }
    if (sd_is_ok())
    {
        sd["type"] = sd_type_str();
        sd["total_kb"] = sd_total_kb();
        sd["used_kb"] = sd_used_kb();
    }

#ifdef USE_STORAGE
    // ── Flash Storage (LittleFS) ──
    JsonObject st = doc.createNestedObject("storage");
    st["ok"] = storage_exists("/");
    st["total_kb"] = (uint32_t)(storage_total_bytes() / 1024);
    st["used_kb"] = (uint32_t)(storage_used_bytes() / 1024);
#endif

    // ── Modbus state ──
    doc["modbus_paused"] = modbus_is_paused();
    doc["register_count"] = register_count;

    // ── DHCP config ──
    JsonObject dhcp = doc.createNestedObject("dhcp_config");
    dhcp["enabled"] = cfg.lan_dhcp;
    dhcp["static_ip"] = cfg.lan_ip;
    dhcp["gw"] = cfg.lan_gw;
    dhcp["mask"] = cfg.lan_mask;
    dhcp["dns"] = cfg.lan_dns;

    String payload;
    serializeJson(doc, payload);
    WS->send(200, "application/json", payload);
}

// ─── Flash Storage API ──────────────────────────────────────
#ifdef USE_STORAGE
// GET /api/storage/list?path=/profiles
static void handleApiStorageList()
{
    if (!web_auth_ok()) return;
    String path = WS->arg("path");
    if (path.isEmpty()) path = "/";
    String result;
    bool ok = storage_list_dir(path.c_str(), result);
    WS->send(ok ? 200 : 404, "application/json", ok ? result : "{\"error\":\"not found\"}");
}

// GET /api/storage/read?path=/profiles/nibe.json
static void handleApiStorageRead()
{
    if (!web_auth_ok()) return;
    String path = WS->arg("path");
    if (path.isEmpty()) { WS->send(400, "application/json", "{\"error\":\"path required\"}"); return; }
    String content;
    bool ok = storage_read_file(path.c_str(), content);
    WS->send(ok ? 200 : 404, "application/json", ok ? content : "{\"error\":\"file not found\"}");
}

// POST /api/storage/write?path=/profiles/sabiana.json  (body = JSON content)
static void handleApiStorageWrite()
{
    if (!web_auth_ok()) return;
    String path = WS->arg("path");
    if (path.isEmpty()) { WS->send(400, "application/json", "{\"error\":\"path required\"}"); return; }
    String body = WS->arg("plain");
    if (body.isEmpty()) body = WS->arg("body");
    if (body.isEmpty()) { WS->send(400, "application/json", "{\"error\":\"empty body\"}"); return; }
    bool ok = storage_write_file(path.c_str(), body.c_str(), body.length());
    WS->send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// DELETE /api/storage/delete?path=/profiles/old.json
static void handleApiStorageDelete()
{
    if (!web_auth_ok()) return;
    String path = WS->arg("path");
    if (path.isEmpty()) { WS->send(400, "application/json", "{\"error\":\"path required\"}"); return; }
    bool ok = storage_delete_file(path.c_str());
    WS->send(ok ? 200 : 404, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}
#endif // USE_STORAGE

// ─── Modbus Write API ──────────────────────────────────────
// POST /api/mb/write?slave=1&reg=255&value=10
static void handleApiMbWrite()
{
    if (!web_auth_ok()) return;
    uint8_t slave = (uint8_t)WS->arg("slave").toInt();
    uint16_t reg  = (uint16_t)strtoul(WS->arg("reg").c_str(), nullptr, 0);
    uint16_t val  = (uint16_t)strtoul(WS->arg("value").c_str(), nullptr, 0);
    if (slave < 1 || slave > 247) { WS->send(400, "application/json", "{\"error\":\"slave 1-247\"}"); return; }

    bool ok = modbus_write_register(slave, reg, val);
    JsonDocument doc;
    doc["ok"] = ok;
    doc["slave"] = slave;
    doc["reg"] = reg;
    doc["value"] = val;
    if (!ok) doc["error"] = "write failed";
    String payload;
    serializeJson(doc, payload);
    WS->send(ok ? 200 : 500, "application/json", payload);
}

// POST /api/mb/writeid?slave=1&reg=255&new_id=5
static void handleApiMbWriteId()
{
    if (!web_auth_ok()) return;
    uint8_t slave  = (uint8_t)WS->arg("slave").toInt();
    uint16_t reg    = (uint16_t)strtoul(WS->arg("reg").c_str(), nullptr, 0);
    uint8_t new_id = (uint8_t)WS->arg("new_id").toInt();
    if (slave < 1 || slave > 247 || new_id < 1 || new_id > 247)
    {
        WS->send(400, "application/json", "{\"error\":\"slave and new_id must be 1-247\"}");
        return;
    }
    bool ok = modbus_write_slave_id(slave, reg, new_id);
    JsonDocument doc;
    doc["ok"] = ok;
    doc["old_slave"] = slave;
    doc["new_id"] = new_id;
    doc["reg"] = reg;
    if (!ok) doc["error"] = "ID write/verify failed — power cycle may be needed";
    String payload;
    serializeJson(doc, payload);
    WS->send(ok ? 200 : 500, "application/json", payload);
}

// GET /api/mb/scan/result — scan progress + found devices
static void handleApiMbScanResult()
{
    if (!web_auth_ok()) return;
    JsonDocument doc;
    doc["scan_active"] = scan_active;
    doc["scan_done"] = scanning_done;
    doc["scan_progress"] = (scan_addr > cfg.mb_scan_end) ? 100 :
        (uint8_t)(((uint32_t)(scan_addr - cfg.mb_scan_start) * 100) /
                  (cfg.mb_scan_end - cfg.mb_scan_start + 1));
    doc["modules_found"] = scan_result_count;

    JsonArray devices = doc.createNestedArray("devices");
    for (uint8_t i = 0; i < scan_result_count; i++) {
        JsonObject d = devices.createNestedObject();
        d["addr"] = scan_results[i].addr;
        d["model_id"] = scan_results[i].model_id;
        d["firmware"] = scan_results[i].firmware_ver;
        d["model_name"] = scan_results[i].model_name;
        d["identified"] = scan_results[i].identified;
    }

    String payload;
    serializeJson(doc, payload);
    WS->send(200, "application/json", payload);
}

// POST /api/mb/scan?start=1&end=247
static void handleApiMbScan()
{
    if (!web_auth_ok()) return;
    uint8_t start = (uint8_t)WS->arg("start").toInt();
    uint8_t end   = (uint8_t)WS->arg("end").toInt();
    if (start < 1) start = 1;
    if (end > 247 || end < start) end = 247;
    // Update scan range and trigger scan
    cfg.mb_scan_start = start;
    cfg.mb_scan_end = end;
    scan_modbus_start();
    JsonDocument doc;
    doc["ok"] = true;
    doc["start"] = start;
    doc["end"] = end;
    doc["message"] = "Scan started — check /api/diag for progress";
    String payload;
    serializeJson(doc, payload);
    WS->send(200, "application/json", payload);
}

// GET /api/mb/regscan?slave=1&type=3&start=0&count=20
// Scans a range of registers on a specific slave and returns all values
static void handleApiMbRegScan()
{
    if (!web_auth_ok()) return;
    if (modbus_is_paused()) {
        WS->send(503, "application/json", "{\"ok\":false,\"error\":\"bus_paused\"}");
        return;
    }

    uint8_t slave = (uint8_t)WS->arg("slave").toInt();
    uint8_t fc = (uint8_t)WS->arg("type").toInt();   // 3=FC03 holding, 4=FC04 input
    uint16_t start = (uint16_t)WS->arg("start").toInt();
    uint16_t count = (uint16_t)WS->arg("count").toInt();

    if (slave < 1 || slave > 247) {
        WS->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_slave\"}");
        return;
    }
    if (fc != 3 && fc != 4) {
        WS->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_fc\"}");
        return;
    }
    if (count == 0 || count > 100) count = 20;  // default 20, max 100

    JsonDocument doc;
    doc["ok"] = true;
    doc["slave"] = slave;
    doc["fc"] = fc;
    doc["start"] = start;
    doc["count"] = count;

    JsonArray regs = doc.createNestedArray("registers");

    // Read in chunks of 10 registers (Modbus limit for one transaction)
    uint16_t remaining = count;
    uint16_t offset = 0;
    while (remaining > 0) {
        uint8_t chunk = (remaining > 10) ? 10 : remaining;

        // Use modbus_raw_request for batch read
        ModbusRawResult result = modbus_raw_request(slave, fc, start + offset, chunk);

        if (result.status != 0x00) {
            // Error — fill remaining with null
            for (uint8_t i = 0; i < chunk; i++) {
                regs.add((char*)nullptr);  // null = could not read
            }
        } else {
            for (uint8_t i = 0; i < result.resp_len && i < chunk; i++) {
                regs.add(result.resp_buf[i]);
            }
            // Pad if fewer results than chunk
            for (uint8_t i = result.resp_len; i < chunk; i++) {
                regs.add((char*)nullptr);
            }
        }
        offset += chunk;
        remaining -= chunk;
        delay(50); // Small delay between chunks to avoid bus overload
    }

    String payload;
    serializeJson(doc, payload);
    WS->send(200, "application/json", payload);
}

// POST /api/mb/coil?slave=1&coil=0&state=1
// FC05 Write Single Coil
static void handleApiMbCoilWrite()
{
    if (!web_auth_ok()) return;
    if (modbus_is_paused()) {
        WS->send(503, "application/json", "{\"ok\":false,\"error\":\"bus_paused\"}");
        return;
    }

    uint8_t slave = (uint8_t)WS->arg("slave").toInt();
    uint16_t coil = (uint16_t)WS->arg("coil").toInt();
    bool state = WS->arg("state").toInt() != 0;

    if (slave < 1 || slave > 247) {
        WS->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_slave\"}");
        return;
    }

    bool ok = modbus_write_coil(slave, (uint8_t)coil, state);

    JsonDocument doc;
    doc["ok"] = ok;
    doc["slave"] = slave;
    doc["coil"] = coil;
    doc["state"] = state;
    if (!ok) doc["error"] = "write_failed";

    String payload;
    serializeJson(doc, payload);
    WS->send(ok ? 200 : 500, "application/json", payload);
}

#ifdef USE_WS2812
// GET /api/led — current LED state
static void handleApiLed()
{
    if (!web_auth_ok()) return;
    JsonDocument doc;
    doc["state"] = led_is_on() ? "ON" : "OFF";
    uint8_t r, g, b;
    led_get_color(&r, &g, &b);
    JsonObject color = doc.createNestedObject("color");
    color["r"] = r;
    color["g"] = g;
    color["b"] = b;
    doc["brightness"] = led_get_brightness();
    doc["pin"] = PIN_WS2812;
    String payload;
    serializeJson(doc, payload);
    WS->send(200, "application/json", payload);
}

// POST /api/led/set — set LED state
// Params: state=ON/OFF, r=0-255, g=0-255, b=0-255, brightness=0-100
static void handleApiLedSet()
{
    if (!web_auth_ok()) return;

    if (WS->hasArg("state"))
    {
        String state = WS->arg("state");
        led_set_state(state.equalsIgnoreCase("ON"));
    }
    if (WS->hasArg("r") || WS->hasArg("g") || WS->hasArg("b"))
    {
        uint8_t r = WS->hasArg("r") ? (uint8_t)WS->arg("r").toInt() : 0;
        uint8_t g = WS->hasArg("g") ? (uint8_t)WS->arg("g").toInt() : 0;
        uint8_t b = WS->hasArg("b") ? (uint8_t)WS->arg("b").toInt() : 0;
        led_set_color(r, g, b);
    }
    if (WS->hasArg("brightness"))
    {
        uint8_t bri = (uint8_t)WS->arg("brightness").toInt();
        led_set_brightness(bri);
    }

    led_publish_state();  // Sync to MQTT

    // Return new state
    JsonDocument doc;
    doc["ok"] = true;
    doc["state"] = led_is_on() ? "ON" : "OFF";
    uint8_t r, g, b;
    led_get_color(&r, &g, &b);
    JsonObject color = doc.createNestedObject("color");
    color["r"] = r;
    color["g"] = g;
    color["b"] = b;
    doc["brightness"] = led_get_brightness();
    String payload;
    serializeJson(doc, payload);
    WS->send(200, "application/json", payload);
}

// ─── Modbus Scan Page ────────────────────────────────────────
static void handleScan()
{
    if (!web_auth_ok()) return;
    String authSfx = WS->hasArg("auth") ? ("&auth=" + WS->arg("auth")) : "";
    String authQ = WS->hasArg("auth") ? ("?auth=" + WS->arg("auth")) : "";

    String html;
    html.reserve(8000);
    html = pageStart(F("Modbus-MQTT Bridge — Scan"), CSS_STATUS) + pageStyleEnd();
    html += F("<h1>&#128269; Modbus Scan</h1>");
    html += navHtml(PG_SCAN, authQ);

    // ─── Slave Discovery ───
    html += F("<div class=\"card\"><h2>&#128225; Slave Discovery</h2>");
    html += F("<div class=\"fm\"><label>Start cím: <input id=\"scanStart\" type=\"number\" min=\"1\" max=\"247\" value=\"1\" style=\"width:70px\"></label>");
    html += F("<label>End cím: <input id=\"scanEnd\" type=\"number\" min=\"1\" max=\"247\" value=\"247\" style=\"width:70px\"></label></div>");
    html += F("<div class=\"fm\"><button onclick=\"startScan()\" id=\"btnScan\">&#9654; Scan indítása</button>");
    html += F("<button onclick=\"pollScan()\" style=\"margin-left:8px\">&#128260; Frissítés</button></div>");
    html += F("<div id=\"scanProgress\" style=\"margin-top:8px;color:#8b949e;font-size:13px\"></div>");
    html += F("<div id=\"scanTable\" style=\"margin-top:12px;display:none\"><table style=\"width:100%;border-collapse:collapse;font-size:13px\">");
    html += F("<tr style=\"color:#58a6ff;border-bottom:1px solid #30363d\"><th style=\"text-align:left;padding:6px 8px\">Cím</th><th style=\"text-align:left;padding:6px 8px\">Típus</th><th style=\"text-align:left;padding:6px 8px\">Modell</th><th style=\"text-align:left;padding:6px 8px\">FW</th><th style=\"text-align:left;padding:6px 8px\">Azon.</th></tr>");
    html += F("</table></div>");
    html += F("</div>");

    // ─── Register Scan ───
    html += F("<div class=\"card\"><h2>&#128209; Regiszter Scan</h2>");
    html += F("<div class=\"fm\"><label>Slave: <input id=\"regSlave\" type=\"number\" min=\"1\" max=\"247\" value=\"1\" style=\"width:70px\"></label>");
    html += F("<label>FC: <select id=\"regFc\" style=\"width:60px\"><option value=\"3\">FC03</option><option value=\"4\">FC04</option></select></label>");
    html += F("<label>Start: <input id=\"regStart\" type=\"number\" min=\"0\" max=\"65535\" value=\"0\" style=\"width:80px\"></label>");
    html += F("<label>Count: <input id=\"regCount\" type=\"number\" min=\"1\" max=\"100\" value=\"20\" style=\"width:60px\"></label></div>");
    html += F("<div class=\"fm\"><button onclick=\"regScan()\">&#128269; Regiszter olvasás</button></div>");
    html += F("<div id=\"regProgress\" style=\"margin-top:8px;color:#8b949e;font-size:13px\"></div>");
    html += F("<div id=\"regTable\" style=\"margin-top:12px;display:none\"><table style=\"width:100%;border-collapse:collapse;font-size:13px\">");
    html += F("<tr style=\"color:#58a6ff;border-bottom:1px solid #30363d\"><th style=\"text-align:left;padding:6px 8px\">Reg</th><th style=\"text-align:left;padding:6px 8px\">Érték</th><th style=\"text-align:left;padding:6px 8px\">Hex</th></tr>");
    html += F("</table></div>");
    html += F("</div>");

    // ─── Write Slave ID ───
    html += F("<div class=\"card\"><h2>&#9999; Slave ID írás (FC06)</h2>");
    html += F("<div class=\"fm\"><label>Jelenlegi cím: <input id=\"writeOldAddr\" type=\"number\" min=\"1\" max=\"247\" value=\"1\" style=\"width:70px\"></label>");
    html += F("<label>Új cím: <input id=\"writeNewAddr\" type=\"number\" min=\"1\" max=\"247\" value=\"1\" style=\"width:70px\"></label></div>");
    html += F("<button onclick=\"writeSlaveId()\" class=\"btn-warn\" style=\"margin-top:4px\">&#9999; ID írása</button>");
    html += F("<div id=\"writeStatus\" style=\"margin-top:8px;font-size:13px\"></div>");
    html += F("</div>");

    // ─── Write Coil (FC05) ───
    html += F("<div class=\"card\"><h2>&#9889; Coil írás (FC05)</h2>");
    html += F("<div class=\"fm\"><label>Slave: <input id=\"coilSlave\" type=\"number\" min=\"1\" max=\"247\" value=\"1\" style=\"width:70px\"></label>");
    html += F("<label>Coil: <input id=\"coilAddr\" type=\"number\" min=\"0\" max=\"65535\" value=\"0\" style=\"width:80px\"></label>");
    html += F("<label>Állapot: <select id=\"coilState\" style=\"width:60px\"><option value=\"1\">ON</option><option value=\"0\">OFF</option></select></label></div>");
    html += F("<button onclick=\"writeCoil()\" class=\"btn-warn\" style=\"margin-top:4px\">&#9889; Coil írása</button>");
    html += F("<div id=\"coilStatus\" style=\"margin-top:8px;font-size:13px\"></div>");
    html += F("</div>");

    // ─── JavaScript ───
    html += F("<script>var _auth=(location.search.match(/auth=([^&]+)/)||[])[1]||'';");
    html += F("function qa(){return _auth?'&auth='+_auth:''}");
    // Slave discovery
    html += F("function startScan(){");
    html += F("var s=document.getElementById('scanStart').value||1;");
    html += F("var e=document.getElementById('scanEnd').value||247;");
    html += F("document.getElementById('scanProgress').textContent='Scan indítása...';");
    html += F("document.getElementById('btnScan').disabled=true;");
    html += F("fetch('/api/mb/scan?start='+s+'&end='+e+qa(),{method:'POST'}).then(r=>r.json()).then(d=>{");
    html += F("if(d.ok){document.getElementById('scanProgress').textContent='Folyamatban... (1-247)';setTimeout(pollScan,2000)}");
    html += F("else{document.getElementById('scanProgress').textContent='Hiba: '+(d.error||'ismeretlen');document.getElementById('btnScan').disabled=false}");
    html += F("})}");
    html += F("function pollScan(){");
    html += F("fetch('/api/mb/scan/result'+(_auth?'?auth='+_auth:'')).then(r=>r.json()).then(d=>{");
    html += F("var p=document.getElementById('scanProgress');");
    html += F("p.textContent=d.scan_active?'&#128260; Scanning... ('+d.scan_progress+'%)':('&#10003; Kész — '+d.modules_found+' eszköz találva');");
    html += F("if(!d.scan_active)document.getElementById('btnScan').disabled=false;");
    html += F("var tb=document.getElementById('scanTable');");
    html += F("if(d.devices&&d.devices.length>0){");
    html += F("var h='<tr style=\"color:#58a6ff;border-bottom:1px solid #30363d\"><th style=\"text-align:left;padding:6px 8px\">Cím</th><th style=\"text-align:left;padding:6px 8px\">Típus</th><th style=\"text-align:left;padding:6px 8px\">Modell</th><th style=\"text-align:left;padding:6px 8px\">FW</th><th style=\"text-align:left;padding:6px 8px\">Azon.</th></tr>';");
    html += F("d.devices.forEach(function(dev){");
    html += F("h+='<tr style=\"border-bottom:1px solid #21262d\"><td style=\"padding:6px 8px\">'+dev.addr+'</td><td style=\"padding:6px 8px\">'+(dev.model_id||'-')+'</td><td style=\"padding:6px 8px\">'+(dev.model_name||'-')+'</td><td style=\"padding:6px 8px\">'+(dev.firmware||'-')+'</td><td style=\"padding:6px 8px\">'+(dev.identified?\"&#10003;\":\"&#10060;\")+'</td></tr>'");
    html += F("});tb.querySelector('table').innerHTML=h;tb.style.display='block'}");
    html += F("if(d.scan_active)setTimeout(pollScan,1000)");
    html += F("})}");

    // Register scan
    html += F("function regScan(){");
    html += F("var sl=document.getElementById('regSlave').value;");
    html += F("var fc=document.getElementById('regFc').value;");
    html += F("var st=document.getElementById('regStart').value;");
    html += F("var ct=document.getElementById('regCount').value;");
    html += F("document.getElementById('regProgress').textContent='Olvasás...';");
    html += F("fetch('/api/mb/regscan?slave='+sl+'&type='+fc+'&start='+st+'&count='+ct+(_auth?'&auth='+_auth:'')).then(r=>r.json()).then(d=>{");
    html += F("if(!d.ok){document.getElementById('regProgress').textContent='Hiba: '+(d.error||'ismeretlen');return}");
    html += F("document.getElementById('regProgress').textContent='Slave '+d.slave+', FC0'+d.fc+', Reg '+d.start+'-'+(d.start+d.count-1)+', '+d.registers.length+' regiszter';");
    html += F("var h='<tr style=\"color:#58a6ff;border-bottom:1px solid #30363d\"><th style=\"text-align:left;padding:6px 8px\">Reg</th><th style=\"text-align:left;padding:6px 8px\">Érték</th><th style=\"text-align:left;padding:6px 8px\">Hex</th></tr>';");
    html += F("d.registers.forEach(function(v,i){");
    html += F("var reg=d.start+i;var hx=v!==null?'0x'+v.toString(16).toUpperCase():'-';var vv=v!==null?v:'null';");
    html += F("h+='<tr style=\"border-bottom:1px solid #21262d\"><td style=\"padding:6px 8px\">'+reg+'</td><td style=\"padding:6px 8px\">'+vv+'</td><td style=\"padding:6px 8px;color:#8b949e\">'+hx+'</td></tr>'");
    html += F("});document.getElementById('regTable').querySelector('table').innerHTML=h;document.getElementById('regTable').style.display='block'");
    html += F("})}");

    // Write slave ID
    html += F("function writeSlaveId(){");
    html += F("var oldA=document.getElementById('writeOldAddr').value;var newA=document.getElementById('writeNewAddr').value;");
    html += F("if(!confirm('Biztosan módosítod a '+oldA+' címről '+newA+'-re?'))return;");
    html += F("document.getElementById('writeStatus').textContent='Írás...';");
    html += F("fetch('/api/mb/writeid?old='+oldA+'&new='+newA+qa(),{method:'POST'}).then(r=>r.json()).then(d=>{");
    html += F("document.getElementById('writeStatus').textContent=d.ok?'&#10003; ID módosítva!':'&#10060; Hiba: '+(d.error||'ismeretlen')");
    html += F("})}");

    // Write coil
    html += F("function writeCoil(){");
    html += F("var sl=document.getElementById('coilSlave').value;var ca=document.getElementById('coilAddr').value;var st=document.getElementById('coilState').value;");
    html += F("if(!confirm('Biztosan írod a coil-t? Slave '+sl+', Coil '+ca+', State '+st))return;");
    html += F("document.getElementById('coilStatus').textContent='Írás...';");
    html += F("fetch('/api/mb/coil?slave='+sl+'&coil='+ca+'&state='+st+qa(),{method:'POST'}).then(r=>r.json()).then(d=>{");
    html += F("document.getElementById('coilStatus').textContent=d.ok?'&#10003; Coil írva!':'&#10060; Hiba: '+(d.error||'ismeretlen')");
    html += F("})}");

    html += F("</script>");

    html += pageFoot();
    WS->send(200, "text/html", html);
}

// ─── Flash Storage Browser Page ────────────────────────────
#ifdef USE_STORAGE
static void handleStorage()
{
    if (!web_auth_ok()) return;
    String authSfx = WS->hasArg("auth") ? ("&auth=" + WS->arg("auth")) : "";
    String curPath = WS->arg("path");
    if (curPath.isEmpty()) curPath = "/";

    String html;
    html.reserve(6000);
    html = pageStart(F("Modbus-MQTT Bridge — Flash Storage"), CSS_STATUS) + pageStyleEnd();
    html += F("<h1>&#128193; Flash Storage</h1>");
    html += navHtml(PG_STORAGE, WS->hasArg("auth") ? ("?auth=" + WS->arg("auth")) : "");

    // Storage info card
    uint64_t st_total = storage_total_bytes();
    uint64_t st_used  = storage_used_bytes();
    uint32_t st_total_kb = (uint32_t)(st_total / 1024);
    uint32_t st_used_kb  = (uint32_t)(st_used / 1024);
    uint8_t st_pct = st_total > 0 ? (uint8_t)((st_used * 100) / st_total) : 0;
    html += F("<div class=\"card\"><h2>&#128202; Állapot</h2>");
    html += "<div class=\"row\"><span class=\"key\">Typus</span><span class=\"val\">LittleFS (Flash)</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Összes</span><span class=\"val\">" + String(st_total_kb) + " KB</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Használt</span><span class=\"val\">" + String(st_used_kb) + " KB (" + String(st_pct) + "%)</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Szabad</span><span class=\"val\">" + String(st_total_kb - st_used_kb) + " KB</span></div>";
    html += F("</div>");

    // File listing
    {
        String heading = F("<div class=\"card\"><h2>&#128193; ");
        heading += htmlEscape(curPath);
        heading += F("</h2>");
        html += heading;
    }
    // Back button if not root
    if (curPath != "/" && curPath != "")
    {
        String parent = curPath.substring(0, curPath.lastIndexOf('/'));
        if (parent.isEmpty()) parent = "/";
        html += "<div style=\"margin-bottom:8px\"><a href=\"/storage?path=" +
                urlEncode(parent) + authSfx + "\" style=\"color:#58a6ff\">⬆️ Vissza</a></div>";
    }

    String listing;
    bool listOk = storage_list_dir(curPath.c_str(), listing);
    if (listOk)
    {
        // Parse JSON array manually for HTML rendering
        JsonDocument doc;
        deserializeJson(doc, listing);
        JsonArray arr = doc.as<JsonArray>();
        if (arr.size() == 0)
        {
            html += F("<div class=\"row\"><span class=\"key\" style=\"color:#484f58\">Üres könyvtár</span></div>");
        }
        for (JsonObject obj : arr)
        {
            String name = obj["name"].as<String>();
            bool isDir  = obj["dir"] | false;
            uint32_t size = obj["size"] | 0;
            String fullPath = curPath;
            if (!fullPath.endsWith("/") && fullPath != "/") fullPath += "/";
            fullPath += name;

            html += "<div class=\"row\"><span class=\"key\">";
            if (isDir)
            {
                html += "📁 " + htmlEscape(name);
                html += "</span><span class=\"val\"><a href=\"/storage?path=" + urlEncode(fullPath) +
                        authSfx + "\" style=\"color:#58a6ff\">Tallózás</a></span>";
            }
            else
            {
                bool isJson = name.endsWith(".json");
                html += "📄 " + htmlEscape(name);
                html += "</span><span class=\"val\">" + String(size) + " B";
                if (isJson)
                    html += " <a href=\"#\" onclick=\"viewJson('" + htmlEscape(fullPath) + "')\" style=\"color:#58a6ff\">📝 Szerkeszt</a>";
                html += " <a href=\"/api/storage/read?path=" + urlEncode(fullPath) +
                        authSfx + "\" style=\"color:#58a6ff\">⬇</a>";
                html += " <a href=\"#\" onclick=\"delFile('" + htmlEscape(fullPath) + "')\" style=\"color:#f85149\">✕</a>";
                html += "</span>";
            }
            html += "</div>";
        }
    }
    else
    {
        html += F("<div class=\"row\"><span class=\"key\" style=\"color:#f85149\">Hiba a könyvtár olvasásakor</span></div>");
    }
    html += F("</div>");

    // Upload form
    html += F("<div class=\"card\"><h2>&#128228; Fájl feltöltés</h2>");
    html += "<div class=\"fm\"><label>Útvonal: <input id=\"upath\" value=\"/profiles/\" style=\"width:200px\"></label></div>";
    html += F("<div class=\"fm\"><label>JSON tartalom:<br><textarea id=\"ubody\" rows=\"6\" style=\"width:100%;background:#0d1117;color:#c9d1d9;border:1px solid #30363d;border-radius:4px;padding:8px\"></textarea></label></div>");
    html += F("<button onclick=\"uploadFile()\" style=\"margin-top:8px\">Feltöltés</button>");
    html += F("</div>");

    // Delete form
    html += F("<div class=\"card\"><h2>&#128465; Fájl törlés</h2>");
    html += "<div class=\"fm\"><label>Útvonal: <input id=\"delpath\" style=\"width:100%\" placeholder=\"/profiles/old.json\"></label></div>";
    html += F("<button onclick=\"delFile(document.getElementById('delpath').value)\" class=\"btn-warn\">Törlés</button>");
    html += F("</div>");

    // JSON editor modal (hidden by default)
    html += F("<div id=\"jsonModal\" style=\"display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.85);z-index:1000;overflow:auto\">");
    html += F("<div style=\"max-width:800px;margin:20px auto;background:#161b22;border:1px solid #30363d;border-radius:8px;padding:16px\">");
    html += F("<h2 id=\"jsonTitle\" style=\"color:#58a6ff;margin:0 0 12px\">JSON szerkesztő</h2>");
    html += F("<textarea id=\"jsonBody\" rows=\"20\" style=\"width:100%;background:#0d1117;color:#c9d1d9;border:1px solid #30363d;border-radius:4px;padding:8px;font-family:monospace;font-size:13px;tab-size:2;resize:vertical\"></textarea>");
    html += F("<div style=\"margin-top:8px;display:flex;gap:8px;flex-wrap:wrap\">");
    html += F("<button onclick=\"saveJson()\" style=\"background:#238636;color:white;padding:8px 16px;border:none;border-radius:6px;cursor:pointer\">💾 Mentés</button>");
    html += F("<button onclick=\"formatJson()\" style=\"background:#21262d;color:#58a6ff;padding:8px 16px;border:1px solid #30363d;border-radius:6px;cursor:pointer\">📐 Formázás</button>");
    html += F("<button onclick=\"closeJson()\" style=\"background:#21262d;color:#f85149;padding:8px 16px;border:1px solid #30363d;border-radius:6px;cursor:pointer\">✕ Bezárás</button>");
    html += F("</div>");
    html += F("<div id=\"jsonStatus\" style=\"margin-top:8px;font-size:12px;color:#8b949e\"></div>");
    html += F("</div></div>");

    // JavaScript
    html += F("<script>var _auth=(location.search.match(/auth=([^&]+)/)||[])[1]||'';");
    html += F("function qa(){return _auth?'&auth='+_auth:''}");
    html += F("var _jsonPath='';");
    html += F("function viewJson(p){_jsonPath=p;");
    html += F("document.getElementById('jsonTitle').textContent=decodeURIComponent(p);");
    html += F("document.getElementById('jsonBody').value='Betöltés...';");
    html += F("document.getElementById('jsonModal').style.display='block';");
    html += F("document.getElementById('jsonStatus').textContent='';");
    html += F("fetch('/api/storage/read?path='+encodeURIComponent(p)+qa())");
    html += F(".then(r=>r.text()).then(t=>{try{var j=JSON.parse(t);document.getElementById('jsonBody').value=JSON.stringify(j,null,2);document.getElementById('jsonStatus').textContent='✅ JSON érvényes ('+t.length+' byte)';document.getElementById('jsonStatus').style.color='#3fb950'}catch(e){document.getElementById('jsonBody').value=t;document.getElementById('jsonStatus').textContent='⚠️ Nem JSON: '+e.message;document.getElementById('jsonStatus').style.color='#d29922'}})");
    html += F(".catch(e=>{document.getElementById('jsonBody').value='';document.getElementById('jsonStatus').textContent='❌ Hiba: '+e;document.getElementById('jsonStatus').style.color='#f85149'})}");
    html += F("function saveJson(){var b=document.getElementById('jsonBody').value;");
    html += F("try{JSON.parse(b)}catch(e){if(!confirm('Hibás JSON! Mentés mégis? ('+e.message+')'))return}");
    html += F("fetch('/api/storage/write?path='+encodeURIComponent(_jsonPath)+qa(),{method:'POST',headers:{'Content-Type':'application/json'},body:b})");
    html += F(".then(r=>r.json()).then(d=>{if(d.ok){document.getElementById('jsonStatus').textContent='✅ Mentve!';document.getElementById('jsonStatus').style.color='#3fb950';setTimeout(()=>location.reload(),1000)}else{document.getElementById('jsonStatus').textContent='❌ Hiba: '+(d.error||'');document.getElementById('jsonStatus').style.color='#f85149'}})}");
    html += F("function formatJson(){var b=document.getElementById('jsonBody').value;try{document.getElementById('jsonBody').value=JSON.stringify(JSON.parse(b),null,2);document.getElementById('jsonStatus').textContent='✅ Formázva';document.getElementById('jsonStatus').style.color='#3fb950'}catch(e){document.getElementById('jsonStatus').textContent='❌ Hibás JSON: '+e.message;document.getElementById('jsonStatus').style.color='#f85149'}}");
    html += F("function closeJson(){document.getElementById('jsonModal').style.display='none'}");
    html += F("function uploadFile(){var p=document.getElementById('upath').value;");
    html += F("var b=document.getElementById('ubody').value;");
    html += F("fetch('/api/storage/write?path='+encodeURIComponent(p)+qa(),{method:'POST',headers:{'Content-Type':'application/json'},body:b})");
    html += F(".then(r=>r.json()).then(d=>{alert(d.ok?'Feltöltés OK!':'Hiba: '+(d.error||'ismeretlen'));location.reload()})}");
    html += F("function delFile(p){if(!confirm('Töröljem: '+p+'?'))return;");
    html += F("fetch('/api/storage/delete?path='+encodeURIComponent(p)+qa(),{method:'POST'})");
    html += F(".then(r=>r.json()).then(d=>{alert(d.ok?'Törölve!':'Hiba');location.reload()})}");
    html += F("</script>");

    html += pageFoot();
    WS->send(200, "text/html", html);
}
#endif // USE_STORAGE

// GET /led — LED control page (Web UI)
static void handleLed()
{
    if (!web_auth_ok()) return;
    String authSfx = WS->hasArg("auth") ? ("?auth=" + WS->arg("auth")) : "";

    uint8_t r, g, b;
    led_get_color(&r, &g, &b);
    uint8_t bri = led_get_brightness();
    bool on = led_is_on();

    String html = pageStart(F("LED vezérlés"), CSS_LED) + pageStyleEnd();
    html += navHtml(PG_LED, authSfx);
    html += F("<h1>&#127764; WS2812B LED vezérlés</h1>");

    // ── LED preview circle ──
    char previewColor[32];
    snprintf(previewColor, sizeof(previewColor), "rgb(%d,%d,%d)", r, g, b);
    html += F("<div class=\"led-preview\" id=\"preview\" style=\"background:");
    html += String(on ? previewColor : "#000000");
    html += F("\"></div>");
    html += F("<p class=\"led-info\">GPIO");
    html += String(PIN_WS2812);
    html += F(" &bull; ");
    html += String(on ? "<span class=\"on\">BE</span>" : "<span class=\"off\">KI</span>");
    html += F("</p>");

    // ── ON/OFF toggle ──
    html += F("<div style=\"text-align:center;margin:8px 0\">");
    html += F("<button class=\"toggle-btn on\" onclick=\"ledCmd('ON')\">&#9654; BE</button>");
    html += F("<button class=\"toggle-btn off\" onclick=\"ledCmd('OFF')\">&#9632; KI</button>");
    html += F("</div>");

    // ── Color sliders ──
    html += F("<div class=\"led-section\"><h3>&#127912; Szín — RGB csatornák</h3>");
    html += F("<div class=\"led-controls\">");

    // Red
    html += F("<div class=\"led-row\"><label>&#128308; Piros</label>");
    html += F("<input type=\"range\" id=\"sr\" min=\"0\" max=\"255\" value=\"");
    html += String(r);
    html += F("\" oninput=\"document.getElementById('vr').textContent=this.value;sendColor()\">");
    html += F("<span class=\"val\" id=\"vr\">");
    html += String(r);
    html += F("</span></div>");

    // Green
    html += F("<div class=\"led-row\"><label>&#128994; Zöld</label>");
    html += F("<input type=\"range\" id=\"sg\" min=\"0\" max=\"255\" value=\"");
    html += String(g);
    html += F("\" oninput=\"document.getElementById('vg').textContent=this.value;sendColor()\">");
    html += F("<span class=\"val\" id=\"vg\">");
    html += String(g);
    html += F("</span></div>");

    // Blue
    html += F("<div class=\"led-row\"><label>&#128309; Kék</label>");
    html += F("<input type=\"range\" id=\"sb\" min=\"0\" max=\"255\" value=\"");
    html += String(b);
    html += F("\" oninput=\"document.getElementById('vb').textContent=this.value;sendColor()\">");
    html += F("<span class=\"val\" id=\"vb\">");
    html += String(b);
    html += F("</span></div>");

    // HTML5 color picker
    char hexColor[8];
    snprintf(hexColor, sizeof(hexColor), "#%02X%02X%02X", r, g, b);
    html += F("<div class=\"led-row\"><label>&#127912; Színválasztó</label>");
    html += F("<input type=\"color\" class=\"color-picker\" id=\"cpick\" value=\"");
    html += String(hexColor);
    html += F("\" oninput=\"colorPicked(this.value)\"></div>");

    html += F("</div></div>");

    // ── Brightness ──
    html += F("<div class=\"led-section\"><h3>&#9728; Fényerő</h3>");
    html += F("<div class=\"led-controls\">");
    html += F("<div class=\"led-row\"><label>Fényerő</label>");
    html += F("<input type=\"range\" id=\"sbri\" min=\"0\" max=\"100\" value=\"");
    html += String(bri);
    html += F("\" oninput=\"document.getElementById('vbr').textContent=this.value+'%';sendBri()\">");
    html += F("<span class=\"val\" id=\"vbr\">");
    html += String(bri);
    html += F("%</span></div>");
    html += F("</div></div>");

    // ── Preset colors ──
    html += F("<div class=\"led-section\"><h3>&#127748; Előre beállított színek</h3>");
    html += F("<div class=\"preset-grid\">");

    // Presets: name, r, g, b
    static const struct { const char *name; uint8_t r, g, b; } presets[] = {
        {"Piros", 255, 0, 0},
        {"Narancs", 255, 140, 0},
        {"Sárga", 255, 255, 0},
        {"Zöld", 0, 255, 0},
        {"Cián", 0, 255, 255},
        {"Kék", 0, 0, 255},
        {"Lila", 180, 0, 255},
        {"Rózsaszín", 255, 0, 127},
        {"Meleg fehér", 255, 200, 150},
        {"Hideg fehér", 200, 220, 255},
    };
    for (auto &p : presets)
    {
        char bg[32];
        snprintf(bg, sizeof(bg), "rgb(%d,%d,%d)", p.r, p.g, p.b);
        html += F("<button class=\"preset-btn\" style=\"background:");
        html += String(bg);
        html += F("\" title=\"");
        html += FPSTR(p.name);
        html += F("\" onclick=\"setPreset(");
        html += String(p.r) + "," + String(p.g) + "," + String(p.b);
        html += F(")\"></button>");
    }
    html += F("</div></div>");

    // ── JavaScript ──
    html += F("<script>\n");
    html += F("var authSfx='");
    html += authSfx;
    html += F("';\n");
    html += F("function ledCmd(s){fetch('/api/led/set'+authSfx,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'state='+s}).then(r=>r.json()).then(d=>refreshPreview(d))}\n");
    html += F("var colorTimer=null;\n");
    html += F("function sendColor(){if(colorTimer)clearTimeout(colorTimer);colorTimer=setTimeout(doSendColor,120)}\n");
    html += F("function doSendColor(){var r=document.getElementById('sr').value,g=document.getElementById('sg').value,b=document.getElementById('sb').value;fetch('/api/led/set'+authSfx,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'r='+r+'&g='+g+'&b='+b}).then(r=>r.json()).then(d=>refreshPreview(d))}\n");
    html += F("function sendBri(){var v=document.getElementById('sbri').value;fetch('/api/led/set'+authSfx,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'brightness='+v}).then(r=>r.json()).then(d=>refreshPreview(d))}\n");
    html += F("function setPreset(r,g,b){document.getElementById('sr').value=r;document.getElementById('sg').value=g;document.getElementById('sb').value=b;document.getElementById('vr').textContent=r;document.getElementById('vg').textContent=g;document.getElementById('vb').textContent=b;var h='#'+((1<<24)+(r<<16)+(g<<8)+b).toString(16).slice(1);document.getElementById('cpick').value=h;fetch('/api/led/set'+authSfx,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'r='+r+'&g='+g+'&b='+b}).then(r2=>r2.json()).then(d=>refreshPreview(d))}\n");
    html += F("function colorPicked(hex){var r=parseInt(hex.substr(1,2),16),g=parseInt(hex.substr(3,2),16),b=parseInt(hex.substr(5,2),16);setPreset(r,g,b)}\n");
    html += F("function refreshPreview(d){var p=document.getElementById('preview');if(d.state==='ON'){p.style.background='rgb('+d.color.r+','+d.color.g+','+d.color.b+')';p.style.boxShadow='0 0 30px rgba('+d.color.r+','+d.color.g+','+d.color.b+',0.6)'}else{p.style.background='#000';p.style.boxShadow='none'}}\n");
    html += F("</script>");

    html += pageFoot();
    WS->send(200, "text/html", html);
}
#endif
// Restore: POST JSON → writes NVRAM keys → reboot

// NVRAM keys to export — MUST match NV_KEY_* defines in modbus_mqtt_ha_bridge.h
// type: s=string, u=uint, U=UShort, B=UChar, b=bool, i=int
static const struct
{
    const char *key;
    char type;
} BACKUP_KEYS[] = {
        {"hostname", 's'}, {"ssid", 's'},   {"wpass", 's'},  {"apn", 's'},   {"appass", 's'}, {"wifimode", 'B'},
        {"wdhcp", 'b'},    {"wip", 's'},    {"wgw", 's'},    {"wmask", 's'}, {"wdns", 's'},   {"ethen", 'b'},
        {"edhcp", 'b'},    {"etype", 'B'},  {"eip", 's'},    {"egw", 's'},   {"emask", 's'},  {"edns", 's'},
        {"mhost", 's'},    {"mport", 'U'},  {"muser", 's'},  {"mpass", 's'}, {"mpfx", 's'},   {"hadisc", 'b'},
        {"mtls", 'b'},     {"mbaud", 'u'},  {"mstart", 'B'}, {"mend", 'B'},  {"mpar", 'B'},   {"mbprof", 'B'},
        {"mbcoil", 'U'},   {"mbdi", 'U'},   {"mbpoll", 'U'}, {"vmod", 'b'},  {"tcpen", 'b'},  {"tcpp", 'U'},
        {"prx", 'i'},      {"ptx", 'i'},    {"pde", 'i'},    {"pled", 'i'},  {"pbtn", 'i'},   {"pemosi", 'i'},
        {"pemiso", 'i'},   {"pesclk", 'i'}, {"pecs", 'i'},   {"peint", 'i'}, {"perst", 'i'},  {"rooms", 's'},
        {"mlist_n", 'B'},  {"wauth", 'b'},  {"wauthp", 's'},
};
#define BACKUP_KEYS_COUNT (sizeof(BACKUP_KEYS) / sizeof(BACKUP_KEYS[0]))

static void handleApiBackup()
{
    if (!web_auth_ok())
        return;
    JsonDocument doc(PsramAllocator::instance());
    Preferences nv;
    nv.begin(NV_NAMESPACE, true);

    doc["version"] = FIRMWARE_VERSION;
    doc["type"] = "modbus-mqtt-bridge-backup";

    for (uint16_t i = 0; i < BACKUP_KEYS_COUNT; i++)
    {
        const char *key = BACKUP_KEYS[i].key;
        switch (BACKUP_KEYS[i].type)
        {
        case 's':
            doc[key] = nv.getString(key, "");
            break;
        case 'u':
            doc[key] = nv.getUInt(key, 0);
            break;
        case 'U':
            doc[key] = nv.getUShort(key, 0);
            break;
        case 'B':
            doc[key] = nv.getUChar(key, 0);
            break;
        case 'b':
            doc[key] = nv.getBool(key, false);
            break;
        case 'i':
            doc[key] = nv.getInt(key, 0);
            break;
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
    for (uint8_t i = 0; i < mlist_n && i < 16; i++)
    {
        String idx = String(i);
        doc["mlist_a" + idx] = nv.getUChar(("mlist_a" + idx).c_str(), 0);
        doc["mlist_m" + idx] = nv.getUChar(("mlist_m" + idx).c_str(), 0);
    }

    // Per-module names/area + relay/DI names (only for discovered modules)
    for (uint16_t i = 0; i < module_count; i++)
    {
        uint8_t addr = modules[i].slave_addr;
        String sa = String(addr);
        String mn = nv.getString(("mn" + sa).c_str(), "");
        String hn = nv.getString(("hn" + sa).c_str(), "");
        String ar = nv.getString(("ar" + sa).c_str(), "");
        if (mn.length() > 0)
            doc["mn" + sa] = mn;
        if (hn.length() > 0)
            doc["hn" + sa] = hn;
        if (ar.length() > 0)
            doc["ar" + sa] = ar;
        for (uint8_t r = 0; r < 6; r++)
        {
            String rn_val = nv.getString(("rn" + sa + "_" + String(r)).c_str(), "");
            if (rn_val.length() > 0)
                doc["rn" + sa + "_" + String(r)] = rn_val;
            String dn_val = nv.getString(("dn" + sa + "_" + String(r)).c_str(), "");
            if (dn_val.length() > 0)
                doc["dn" + sa + "_" + String(r)] = dn_val;
        }
    }

    nv.end();
    String payload;
    serializeJson(doc, payload);
    WS->send(200, "application/json", payload);
}

static void handleApiRestore()
{
    if (!web_auth_ok())
        return;
    if (!WS->hasArg("plain"))
    {
        WS->send(400, "application/json", "{\"ok\":false,\"error\":\"no body\"}");
        return;
    }
    JsonDocument inDoc(PsramAllocator::instance());
    DeserializationError err = deserializeJson(inDoc, WS->arg("plain"));
    if (err)
    {
        WS->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid json\"}");
        return;
    }
    if (!inDoc.containsKey("type") || strcmp(inDoc["type"], "modbus-mqtt-bridge-backup") != 0)
    {
        WS->send(400, "application/json", "{\"ok\":false,\"error\":\"not a valid backup\"}");
        return;
    }

    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    uint16_t written = 0;

    // Build lookup from BACKUP_KEYS for type-correct writes
    for (JsonPair kv : inDoc.as<JsonObject>())
    {
        const char *key = kv.key().c_str();
        if (strcmp(key, "type") == 0 || strcmp(key, "version") == 0 || strcmp(key, "passwords_masked") == 0)
            continue;

        JsonVariant val = kv.value();

        // Skip masked passwords — don't overwrite with literal "***"
        if (val.is<const char *>() && strcmp(val.as<const char *>(), "***") == 0)
            continue;

        // Find type from BACKUP_KEYS table
        char ktype = 0;
        for (uint16_t i = 0; i < BACKUP_KEYS_COUNT; i++)
        {
            if (strcmp(BACKUP_KEYS[i].key, key) == 0)
            {
                ktype = BACKUP_KEYS[i].type;
                break;
            }
        }

        // Dynamic module keys: mlist_a/B, mlist_m/B, mn/s, hn/s, ar/s, rn/s, dn/s
        if (ktype == 0)
        {
            if (strncmp(key, "mlist_a", 7) == 0 || strncmp(key, "mlist_m", 7) == 0)
                ktype = 'B';
            else if (strncmp(key, "mn", 2) == 0 || strncmp(key, "hn", 2) == 0 || strncmp(key, "ar", 2) == 0 ||
                     strncmp(key, "rn", 2) == 0 || strncmp(key, "dn", 2) == 0)
                ktype = 's';
        }

        // Write using type-correct Preferences method
        if (val.is<const char *>() && (ktype == 's' || ktype == 0))
        {
            nv.putString(key, val.as<const char *>());
        }
        else if (ktype == 'B')
        {
            nv.putUChar(key, (uint8_t)(val.is<int>() ? val.as<int>() : val.as<unsigned int>()));
        }
        else if (ktype == 'U')
        {
            nv.putUShort(key, (uint16_t)(val.is<int>() ? val.as<int>() : val.as<unsigned int>()));
        }
        else if (ktype == 'b' || val.is<bool>())
        {
            nv.putBool(key, val.as<bool>());
        }
        else if (ktype == 'i')
        {
            nv.putInt(key, val.as<int>());
        }
        else if (ktype == 'u')
        {
            nv.putUInt(key, val.as<unsigned int>());
        }
        else if (val.is<int>())
        {
            nv.putInt(key, val.as<int>());
        }
        else
        {
            continue;
        }
        written++;
    }
    nv.end();

    LOG_I("[RESTORE] %d keys written, rebooting...\n", written);
    WS->send(200, "application/json", "{\"ok\":true,\"keys\":" + String(written) + ",\"action\":\"reboot\"}");
    delay(3000);
    eth_hard_reset_and_restart();
}

// ─── Init & Loop ───────────────────────────────────────────────
// ─── Admin Panel ────────────────────────────────────────────────
static void handleAdmin()
{
    if (!web_auth_ok())
        return;

    String html;
    html.reserve(3000);
    html = pageStart(F("Modbus-MQTT Bridge — Admin"), CSS_FORMS) + pageStyleEnd();

    html += F("<h1>&#128272; Admin</h1>");
    html += navHtml(PG_ADMIN, WS->hasArg("auth") ? ("?auth=" + WS->arg("auth")) : "");
    html += F("<div class=\"pri\"><b>&#9888; Figyelem!</b> Itt jelszavakat módosítasz. Ha elfelejted a jelszót, csak USB flash-sel lehet visszaállítani!</div>"
              "<form action=\"/saveadmin\" method=\"POST\">"
              "<h2>&#128100; Admin jelszó (Digest Auth)</h2>"
              "<div class=\"chk\"><label>Felhasználó: <b>admin</b> (nem módosítható)</label></div>"
              "<div class=\"fm\"><label>Jelenlegi jelszó</label><input type=\"password\" name=\"curpass\" placeholder=\"Írd be a jelenlegi jelszót\" required></div>"
              "<div class=\"row\"><div class=\"fm\"><label>Új jelszó</label><input type=\"password\" name=\"newpass\" placeholder=\"Új jelszó\" required></div>"
              "<div class=\"fm\"><label>Megerősítés</label><input type=\"password\" name=\"newpass2\" placeholder=\"Új jelszó újra\" required></div></div>"
              "<p class=\"note\">Min. 4 karakter. Üresen hagyva = auth kikapcsolva (nem ajánlott).</p>"
              "<div><button type=\"submit\" class=\"btn\" name=\"action\" value=\"changepass\">Jelszó módosítása</button>"
              "<button type=\"submit\" class=\"btn rst\" name=\"action\" value=\"resetpass\">Alapértelmezett (admin)</button></div>"
              "<h2>&#128225; AP jelszó (WiFi Access Point)</h2>"
              "<div class=\"fm\"><label>AP jelszó</label><input type=\"password\" name=\"appass\" value=\"");

    // Show AP pass as dots (current value)
    html += String(cfg.ap_pass);
    html += F("\" placeholder=\"12345678\"></div>"
              "<p class=\"note\">Az AP jelszó a saját WiFi hálózatod védelme. Min. 8 karakter! Alapértelmezett: 12345678</p>"
              "<div><button type=\"submit\" class=\"btn\" name=\"action\" value=\"changeap\">AP jelszó mentése</button>"
              "<button type=\"submit\" class=\"btn rst\" name=\"action\" value=\"resetap\">Alapértelmezett (12345678)</button></div>"
              "</form>");

    html += F("<p class=\"foot\">Modbus-MQTT Bridge v");

    html += String(FIRMWARE_VERSION);
    html += pageFoot();

    WS->send(200, "text/html", html);
}

static void handleSaveAdmin()
{
    if (!web_auth_ok())
        return;

    String action = WS->arg("action");

    if (action == "changepass" || action == "resetpass")
    {
        // Change admin password
        String curpass = WS->arg("curpass");
        String newpass = WS->arg("newpass");
        String newpass2 = WS->arg("newpass2");

        // Verify current password
        if (curpass != String(cfg.web_pass))
        {
            WS->send(403, "text/html", pageStart(F("Hiba"), CSS_ERROR) + pageStyleEnd() +
                     F("<h1 class=\"err\">&#10060; Hibás jelenlegi jelszó!</h1><p>A megadott jelszó nem egyezik.</p>"
                       "<a href=\"/admin\">Vissza</a></body></html>"));
            return;
        }

        if (action == "resetpass")
        {
            // Reset to factory default
            strlcpy(cfg.web_pass, "admin", sizeof(cfg.web_pass));
        }
        else
        {
            // Validate new password
            if (newpass != newpass2)
            {
                WS->send(400, "text/html",
                         pageStart(F("Hiba"), CSS_ERROR) + pageStyleEnd() +
                         F("<h1 class=\"err\">&#10060; A két jelszó nem egyezik!</h1><p>Próbáld újra.</p>"
                           "<a href=\"/admin\">Vissza</a></body></html>"));
                return;
            }
            if (newpass.length() < 4)
            {
                WS->send(400, "text/html",
                         pageStart(F("Hiba"), CSS_ERROR) + pageStyleEnd() +
                         F("<h1 class=\"err\">&#10060; Túl rövid jelszó!</h1><p>Min. 4 karakter szükséges.</p>"
                           "<a href=\"/admin\">Vissza</a></body></html>"));
                return;
            }
            strlcpy(cfg.web_pass, newpass.c_str(), sizeof(cfg.web_pass));
        }

        // Save to NVRAM
        Preferences nv;
        nv.begin(NV_NAMESPACE, false);
        nv.putString(NV_KEY_WEB_PASS, cfg.web_pass);
        nv.end();

        config_write_crc(); // Update config CRC

        LOG_ILN("[ADMIN] Web auth password changed");
        WS->send(200, "text/html", pageStart(F("Mentve"), CSS_MINIMAL) + pageStyleEnd() +
                 F("<h1 class=\"ok\">&#10004; Admin jelszó elmentve!</h1><p>A változás azonnal érvényes.</p>"
                   "<a href=\"/admin\">Vissza</a></body></html>"));
        return;
    }

    if (action == "changeap" || action == "resetap")
    {
        // Change AP password
        if (action == "resetap")
        {
            strlcpy(cfg.ap_pass, WIFI_AP_PASS_DEFAULT, sizeof(cfg.ap_pass));
        }
        else
        {
            String appass = WS->arg("appass");
            if (appass.length() < 8)
            {
                WS->send(400, "text/html",
                         pageStart(F("Hiba"), CSS_ERROR) + pageStyleEnd() +
                         F("<h1 class=\"err\">&#10060; Túl rövid AP jelszó!</h1><p>Min. 8 karakter szükséges a WPA2 miatt.</p>"
                           "<a href=\"/admin\">Vissza</a></body></html>"));
                return;
            }
            strlcpy(cfg.ap_pass, appass.c_str(), sizeof(cfg.ap_pass));
        }

        // Update AP immediately
        String ap_name = cfg.ap_name[0] ? String(cfg.ap_name)
                                        : String(cfg.hostname) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
        WiFi.softAP(ap_name.c_str(), cfg.ap_pass);

        // Save to NVRAM
        Preferences nv;
        nv.begin(NV_NAMESPACE, false);
        nv.putString(NV_KEY_AP_PASS, cfg.ap_pass);
        nv.end();

        config_write_crc(); // Update config CRC

        LOG_ILN("[ADMIN] AP password changed, softAP updated");
        WS->send(200, "text/html", pageStart(F("Mentve"), CSS_MINIMAL) + pageStyleEnd() +
                 F("<h1 class=\"ok\">&#10004; AP jelszó elmentve!</h1><p>Az AP azonnal újraindult az új jelszóval.</p>"
                   "<a href=\"/admin\">Vissza</a></body></html>"));
        return;
    }

    WS->send(400, "text/html", "Bad request");
}

// ─── Helper: RegHAClass → Hungarian label ───────────────────────
static const char *haClassLabel(uint8_t cls)
{
    switch (cls)
    {
    case HAC_SENSOR:      return "Szenzor";
    case HAC_TEMPERATURE: return "Hőmérséklet";
    case HAC_HUMIDITY:    return "Páratartalom";
    case HAC_POWER:       return "Teljesítmény";
    case HAC_ENERGY:      return "Energia";
    case HAC_PRESSURE:    return "Nyomás";
    case HAC_VOLTAGE:     return "Feszültség";
    case HAC_CURRENT:     return "Áram";
    case HAC_FREQUENCY:   return "Frekvencia";
    case HAC_COP:         return "COP";
    default:              return "?";
    }
}

// ─── REGISTER CONFIG PAGE ────────────────────────────────────────
static void handleRegisters()
{
    if (!web_auth_ok())
        return;
    String html;
    html.reserve(8000);
    String authSfx = WS->hasArg("auth") ? ("?auth=" + WS->arg("auth")) : String();

    html = pageStart(F("Modbus-MQTT Bridge — Regiszterek"), CSS_REGISTERS) + FPSTR(CSS_FORMS) + pageStyleEnd();

    html += F("<h1>&#128202; Regiszterek</h1>");
    html += navHtml(PG_REGISTERS, authSfx);

    html += F("<p class=\"note\">Modbus FC03/FC04 regiszterek olvasása és MQTT + Home Assistant automatikus közzététele. "
              "Max 32 regiszter konfigurálható.</p>");

    // ── Add new register form (inline) ──
    html += F("<div class=\"add-form\">");
    html += F("<h3>&#10133; Új regiszter hozzáadása</h3>");
    html += F("<div class=\"row\">");
    html += F("<div class=\"fm fm-sm\"><label>Cím</label><input id=\"r_addr\" type=\"number\" min=\"0\" max=\"65535\" placeholder=\"0\"></div>");
    html += F("<div class=\"fm fm-sm\"><label>Típus</label><select id=\"r_type\">"
              "<option value=\"3\">FC03 (Holding)</option>"
              "<option value=\"4\">FC04 (Input)</option>"
              "</select></div>");
    html += F("<div class=\"fm fm-sm\"><label>Slave</label><input id=\"r_slave\" type=\"number\" min=\"1\" max=\"247\" value=\"1\"></div>");
    html += F("<div class=\"fm fm-sm\"><label>HA Osztály</label><select id=\"r_class\">"
              "<option value=\"0\">Szenzor</option>"
              "<option value=\"1\">Hőmérséklet</option>"
              "<option value=\"2\">Páratartalom</option>"
              "<option value=\"3\">Teljesítmény</option>"
              "<option value=\"4\">Energia</option>"
              "<option value=\"5\">Nyomás</option>"
              "<option value=\"6\">Feszültség</option>"
              "<option value=\"7\">Áram</option>"
              "<option value=\"8\">Frekvencia</option>"
              "<option value=\"9\">COP</option>"
              "</select></div>");
    html += F("<div class=\"fm fm-sm\"><label>Scale</label><input id=\"r_scale\" type=\"number\" min=\"1\" max=\"65535\" value=\"1\"></div>");
    html += F("<div class=\"fm fm-lg\"><label>Név</label><input id=\"r_name\" maxlength=\"23\" placeholder=\"Hőmérséklet\"></div>");
    html += F("<div class=\"fm fm-sm\"><label>Egység</label><input id=\"r_unit\" maxlength=\"7\" placeholder=\"°C\"></div>");
    html += F("</div>");
    html += F("<button class=\"btn-add\" onclick=\"addRegister()\">&#10133; Hozzáadás</button>");
    html += F("</div>");

    // ── Register table ──
    html += F("<div class=\"card\">");
    html += F("<table class=\"rtbl\">");
    html += F("<tr><th>#</th><th>Cím</th><th>Típus</th><th>HA Osztály</th>"
              "<th>Slave</th><th>Scale</th><th>Név</th><th>Egység</th>"
              "<th>Engedélyezve</th><th>Érték</th><th>Műveletek</th></tr>");

    if (register_count == 0)
    {
        html += F("<tr><td colspan=\"11\" style=\"text-align:center;color:#8b949e;padding:12px\">"
                  "Nincs regiszter konfigurálva</td></tr>");
    }

    for (uint8_t i = 0; i < register_count; i++)
    {
        RegisterConfig &r = registers[i];
        String en_cls = r.enabled ? "on" : "off";
        String en_txt = r.enabled ? "BE" : "KI";

        html += "<tr id=\"reg_row_" + String(i) + "\">";
        html += "<td>" + String(i) + "</td>";
        html += "<td>" + String(r.addr) + "</td>";
        html += "<td>FC0" + String(r.reg_type) + "</td>";
        html += "<td>" + String(haClassLabel(r.ha_class)) + "</td>";
        html += "<td>S" + String(r.slave_addr) + "</td>";
        html += "<td>" + String(r.scale) + "</td>";
        html += "<td>" + htmlEscape(r.name) + "</td>";
        html += "<td>" + htmlEscape(r.unit) + "</td>";
        html += "<td><span class=\"badge " + en_cls + "\" id=\"reg_en_" + String(i) + "\">" + en_txt + "</span></td>";

        // Value display
        if (r.published)
        {
            char vbuf[16];
            if (r.scale > 1)
                snprintf(vbuf, sizeof(vbuf), "%.2f", r.last_value / (float)r.scale);
            else
                snprintf(vbuf, sizeof(vbuf), "%.0f", r.last_value);
            html += "<td class=\"val\">" + String(vbuf) + "</td>";
        }
        else
        {
            html += "<td style=\"color:#484f58\">—</td>";
        }

        // Action buttons
        html += "<td>";
        html += "<button class=\"btn-sm btn-tog\" onclick=\"toggleRegister(" + String(i) + ")\">&#128260;</button> ";
        html += "<button class=\"btn-sm btn-del\" onclick=\"deleteRegister(" + String(i) + ")\">&#128465;</button>";
        html += "</td>";
        html += "</tr>";
    }

    html += F("</table></div>");

    html += pageFoot();

    // JavaScript for AJAX operations
    html += R"rawliteral(<script>
var _auth=(location.search.match(/auth=([^&]+)/)||[])[1]||'';
function apiPost(url,cb){
  if(_auth)url+=(url.indexOf('?')<0?'?':'&')+'auth='+_auth;
  fetch(url,{method:'POST'}).then(r=>r.json()).then(cb).catch(e=>{alert('Hálózati hiba');});
}
function addRegister(){
  var a=document.getElementById('r_addr').value;
  var t=document.getElementById('r_type').value;
  var s=document.getElementById('r_slave').value;
  var c=document.getElementById('r_class').value;
  var sc=document.getElementById('r_scale').value;
  var n=document.getElementById('r_name').value;
  var u=document.getElementById('r_unit').value;
  if(!a){alert('Cím megadása kötelező');return;}
  var url='/api/register/add?addr='+a+'&type='+t+'&slave='+s+'&class='+c+'&scale='+sc+'&name='+encodeURIComponent(n)+'&unit='+encodeURIComponent(u)+'&enabled=1';
  apiPost(url,function(d){
    if(d.ok){location.reload();}
    else{alert('Hiba: '+(d.error||'ismeretlen'));}
  });
}
function deleteRegister(idx){
  if(!confirm('Biztosan törlöd a regisztert #'+idx+'?'))return;
  apiPost('/api/register/delete?index='+idx,function(d){
    if(d.ok){location.reload();}
    else{alert('Hiba: '+(d.error||'ismeretlen'));}
  });
}
function toggleRegister(idx){
  apiPost('/api/register/toggle?index='+idx,function(d){
    if(d.ok){location.reload();}
    else{alert('Hiba: '+(d.error||'ismeretlen'));}
  });
}
</script></body></html>)rawliteral";

    WS->send(200, "text/html", html);
}

// ─── API: GET /api/registers ──────────────────────────────────────
static void handleApiRegisters()
{
    if (!web_auth_ok())
        return;
    JsonDocument doc(PsramAllocator::instance());
    JsonArray arr = doc.to<JsonArray>();
    for (uint8_t i = 0; i < register_count; i++)
    {
        RegisterConfig &r = registers[i];
        JsonObject obj = arr.add<JsonObject>();
        obj["index"] = i;
        obj["addr"] = r.addr;
        obj["type"] = (uint8_t)r.reg_type;
        obj["ha_class"] = (uint8_t)r.ha_class;
        obj["ha_class_label"] = haClassLabel(r.ha_class);
        obj["slave"] = r.slave_addr;
        obj["scale"] = r.scale;
        obj["name"] = r.name;
        obj["unit"] = r.unit;
        obj["enabled"] = r.enabled;
        obj["last_value"] = r.last_value;
        obj["published"] = r.published;
    }
    String payload;
    serializeJson(doc, payload);
    WS->send(200, "application/json", payload);
}

// ─── API: POST /api/register/add ──────────────────────────────────
static void handleApiRegisterAdd()
{
    if (!web_auth_ok())
        return;
    if (register_count >= MAX_REGISTERS)
    {
        WS->send(400, "application/json", "{\"ok\":false,\"error\":\"max registers reached\"}");
        return;
    }
    if (!WS->hasArg("addr"))
    {
        WS->send(400, "application/json", "{\"ok\":false,\"error\":\"missing addr\"}");
        return;
    }

    RegisterConfig &r = registers[register_count];
    memset(&r, 0, sizeof(RegisterConfig));

    r.addr = (uint16_t)WS->arg("addr").toInt();
    r.reg_type = WS->hasArg("type") ? (RegType)WS->arg("type").toInt() : REG_HOLDING;
    if (r.reg_type != REG_INPUT)
        r.reg_type = REG_HOLDING;
    r.slave_addr = WS->hasArg("slave") ? (uint8_t)WS->arg("slave").toInt() : 1;
    r.ha_class = WS->hasArg("class") ? (RegHAClass)WS->arg("class").toInt() : HAC_SENSOR;
    r.scale = WS->hasArg("scale") ? (uint16_t)WS->arg("scale").toInt() : 1;
    if (r.scale == 0) r.scale = 1;
    strlcpy(r.name, WS->hasArg("name") ? WS->arg("name").c_str() : "", sizeof(r.name));
    strlcpy(r.unit, WS->hasArg("unit") ? WS->arg("unit").c_str() : "", sizeof(r.unit));
    r.enabled = WS->hasArg("enabled") ? (WS->arg("enabled") == "1") : true;

    // Runtime fields
    r.last_value = 0;
    r.published = false;
    r.last_read_ms = 0;

    register_count++;
    config_save_registers();

    // Publish HA discovery immediately so the entity appears in HA
    if (mqtt_is_connected())
        mqtt_publish_register_discovery(&registers[register_count - 1]);

    LOG_I("[REG] Added register #%u: addr=%u type=FC0%u slave=S%u class=%s name=%s\n",
          register_count - 1, r.addr, r.reg_type, r.slave_addr, haClassLabel(r.ha_class), r.name);

    WS->send(200, "application/json", "{\"ok\":true,\"index\":" + String(register_count - 1) + "}");
}

// ─── API: POST /api/register/edit ─────────────────────────────────
static void handleApiRegisterEdit()
{
    if (!web_auth_ok())
        return;
    if (!WS->hasArg("index"))
    {
        WS->send(400, "application/json", "{\"ok\":false,\"error\":\"missing index\"}");
        return;
    }
    uint8_t idx = (uint8_t)WS->arg("index").toInt();
    if (idx >= register_count)
    {
        WS->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid index\"}");
        return;
    }

    RegisterConfig &r = registers[idx];
    if (WS->hasArg("addr"))
        r.addr = (uint16_t)WS->arg("addr").toInt();
    if (WS->hasArg("type"))
    {
        uint8_t t = (uint8_t)WS->arg("type").toInt();
        r.reg_type = (t == REG_INPUT) ? REG_INPUT : REG_HOLDING;
    }
    if (WS->hasArg("slave"))
        r.slave_addr = (uint8_t)WS->arg("slave").toInt();
    if (WS->hasArg("class"))
        r.ha_class = (RegHAClass)WS->arg("class").toInt();
    if (WS->hasArg("scale"))
    {
        uint16_t s = (uint16_t)WS->arg("scale").toInt();
        r.scale = (s > 0) ? s : 1;
    }
    if (WS->hasArg("name"))
        strlcpy(r.name, WS->arg("name").c_str(), sizeof(r.name));
    if (WS->hasArg("unit"))
        strlcpy(r.unit, WS->arg("unit").c_str(), sizeof(r.unit));
    if (WS->hasArg("enabled"))
        r.enabled = (WS->arg("enabled") == "1");

    config_save_registers();

    LOG_I("[REG] Edited register #%u: addr=%u type=FC0%u slave=S%u\n",
          idx, r.addr, r.reg_type, r.slave_addr);

    WS->send(200, "application/json", "{\"ok\":true,\"index\":" + String(idx) + "}");
}

// ─── API: POST /api/register/delete ───────────────────────────────
static void handleApiRegisterDelete()
{
    if (!web_auth_ok())
        return;
    if (!WS->hasArg("index"))
    {
        WS->send(400, "application/json", "{\"ok\":false,\"error\":\"missing index\"}");
        return;
    }
    uint8_t idx = (uint8_t)WS->arg("index").toInt();
    if (idx >= register_count)
    {
        WS->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid index\"}");
        return;
    }

    LOG_I("[REG] Deleting register #%u: addr=%u name=%s\n",
          idx, registers[idx].addr, registers[idx].name);

    // Shift remaining registers down
    for (uint8_t i = idx; i < register_count - 1; i++)
    {
        registers[i] = registers[i + 1];
    }
    register_count--;

    // Clear last slot
    memset(&registers[register_count], 0, sizeof(RegisterConfig));

    config_save_registers();

    WS->send(200, "application/json", "{\"ok\":true}");
}

// ─── API: POST /api/register/toggle ───────────────────────────────
static void handleApiRegisterToggle()
{
    if (!web_auth_ok())
        return;
    if (!WS->hasArg("index"))
    {
        WS->send(400, "application/json", "{\"ok\":false,\"error\":\"missing index\"}");
        return;
    }
    uint8_t idx = (uint8_t)WS->arg("index").toInt();
    if (idx >= register_count)
    {
        WS->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid index\"}");
        return;
    }

    registers[idx].enabled = !registers[idx].enabled;
    config_save_registers();

    LOG_I("[REG] Toggled register #%u: enabled=%s\n",
          idx, registers[idx].enabled ? "true" : "false");

    String resp = "{\"ok\":true,\"index\":" + String(idx) +
                  ",\"enabled\":" + String(registers[idx].enabled ? "true" : "false") + "}";
    WS->send(200, "application/json", resp);
}

void web_server_init()
{
    web.on("/", HTTP_GET, handleStatus);
    web.on("/scanstatus", HTTP_GET, handleApiScan);
    web.on("/config", HTTP_GET, handleConfig);
    web.on("/pins", HTTP_GET, handlePins);
    web.on("/modules", HTTP_GET, handleModules);
    web.on("/registers", HTTP_GET, handleRegisters);
    web.on("/admin", HTTP_GET, handleAdmin);
    web.on("/saveadmin", HTTP_POST, handleSaveAdmin);
    web.on("/ota", HTTP_GET, handleOtaPage);
    web.on("/otaupload", HTTP_POST, handleOtaUpload, handleOtaUpload);
    web.on("/otaurl", HTTP_GET, handleOtaFromURL);  // ESP32 downloads firmware from URL (safe OTA)
    web.on("/api/ota/raw", HTTP_POST, handleApiOtaRaw); // Raw binary POST (curl-friendly, works on LAN+WiFi)
    web.on("/save", HTTP_POST, handleSave);
    web.on("/savepins", HTTP_POST, handleSavePins);
    web.on("/savemodules", HTTP_POST, handleSaveModules);
    web.on("/savemodlist", HTTP_POST, handleSaveModList);
    web.on("/addroom", HTTP_GET, handleAddRoom);
    web.on("/delroom", HTTP_GET, handleDelRoom);
    web.on("/togglevmod", HTTP_GET, handleToggleVMod);
    web.on("/relay", HTTP_GET, handleRelay);
    web.on("/di", HTTP_GET, handleDI);
    web.on("/rescan", HTTP_POST, handleRescan);
    web.on("/restart", HTTP_GET, handleRestart);
    web.on("/logout", HTTP_GET, handleLogout);
    web.on("/api/backup", HTTP_GET, handleApiBackup);
    web.on("/api/restore", HTTP_POST, handleApiRestore);
    // Aliases for clarity: /api/export = /api/backup, /api/import = /api/restore
    web.on("/api/export", HTTP_GET, handleApiBackup);
    web.on("/api/import", HTTP_POST, handleApiRestore);
    // ── JSON API endpoints (scripting-friendly) ───────────────
    web.on("/api/status", HTTP_GET, handleApiStatus);
    web.on("/api/config", HTTP_GET, handleApiConfig);
    web.on("/api/modules", HTTP_GET, handleApiModules);
    web.on("/api/direlay", HTTP_POST, handleApiDiRelay);
    web.on("/api/diedge", HTTP_POST, handleApiDiEdge);
    // ── Register config API ──────────────────────────────────
    web.on("/api/registers", HTTP_GET, handleApiRegisters);
    web.on("/api/register/add", HTTP_POST, handleApiRegisterAdd);
    web.on("/api/register/edit", HTTP_POST, handleApiRegisterEdit);
    web.on("/api/register/delete", HTTP_POST, handleApiRegisterDelete);
    web.on("/api/register/toggle", HTTP_POST, handleApiRegisterToggle);
    web.on("/api/sd/save", HTTP_POST, handleApiSdSave);
    web.on("/api/sd/list", HTTP_GET, handleApiSdList);
    web.on("/api/sd/read", HTTP_GET, handleApiSdRead);
    web.on("/api/sd/del", HTTP_DELETE, handleApiSdDel);
    web.on("/api/sd/toggle", HTTP_POST, handleApiSdToggle);
    web.on("/api/sd/init", HTTP_POST, handleApiSdInit);
    // ── SD Card browser page & API ──────────────────────────
    web.on("/sd", HTTP_GET, handleSdCard);
#ifdef USE_STORAGE
    web.on("/storage", HTTP_GET, handleStorage);
#endif
    web.on("/scan", HTTP_GET, handleScan);
    web.on("/api/sd/browse", HTTP_GET, handleApiSdBrowse);
    web.on("/api/sd/view", HTTP_GET, handleApiSdView);
    web.on("/api/sd/remove", HTTP_POST, handleApiSdRemove);
    web.on("/api/sd/mkdir", HTTP_POST, handleApiSdMkdir);
    web.on("/api/sd/format", HTTP_POST, handleApiSdFormat);
    web.on("/api/sd/upload", HTTP_POST, handleApiSdUpload, handleApiSdUpload);
    web.on("/api/lan", HTTP_GET, handleApiLan);
    web.on("/api/diag", HTTP_GET, handleApiDiag);
#ifdef USE_WS2812
    web.on("/api/led", HTTP_GET, handleApiLed);
    web.on("/api/led/set", HTTP_POST, handleApiLedSet);
    web.on("/led", HTTP_GET, handleLed);
#endif
#ifdef USE_SD
    web.on("/api/sd/gpio", HTTP_GET, handleApiSdGpio);
    web.on("/api/sd/test", HTTP_GET, handleApiSdTest);
#endif
#ifdef USE_STORAGE
    web.on("/api/storage/list", HTTP_GET, handleApiStorageList);
    web.on("/api/storage/read", HTTP_GET, handleApiStorageRead);
    web.on("/api/storage/write", HTTP_POST, handleApiStorageWrite);
    web.on("/api/storage/delete", HTTP_POST, handleApiStorageDelete);
#endif
    web.on("/api/mb/write", HTTP_POST, handleApiMbWrite);
    web.on("/api/mb/writeid", HTTP_POST, handleApiMbWriteId);
    web.on("/api/mb/scan", HTTP_POST, handleApiMbScan);
    web.on("/api/mb/scan/result", HTTP_GET, handleApiMbScanResult);
    web.on("/api/mb/regscan", HTTP_GET, handleApiMbRegScan);
    web.on("/api/mb/coil", HTTP_POST, handleApiMbCoilWrite);
    web.begin();
    LOG_ILN("[WEB] Status & Config server started on port 80");

#ifdef USE_W5500
    // ── Register LAN routes (same handlers, different transport) ──
    ethWeb.on("/", ETH_HTTP_GET, handleStatus);
    ethWeb.on("/config", ETH_HTTP_GET, handleConfig);
    ethWeb.on("/pins", ETH_HTTP_GET, handlePins);
    ethWeb.on("/modules", ETH_HTTP_GET, handleModules);
    ethWeb.on("/registers", ETH_HTTP_GET, handleRegisters);
    ethWeb.on("/admin", ETH_HTTP_GET, handleAdmin);
    ethWeb.on("/ota", ETH_HTTP_GET, handleOtaPage);
    ethWeb.on("/otaurl", ETH_HTTP_GET, handleOtaFromURL);
    ethWeb.on("/save", ETH_HTTP_POST, handleSave);
    ethWeb.on("/savepins", ETH_HTTP_POST, handleSavePins);
    ethWeb.on("/savemodules", ETH_HTTP_POST, handleSaveModules);
    ethWeb.on("/savemodlist", ETH_HTTP_POST, handleSaveModList);
    ethWeb.on("/saveadmin", ETH_HTTP_POST, handleSaveAdmin);
    ethWeb.on("/addroom", ETH_HTTP_GET, handleAddRoom);
    ethWeb.on("/delroom", ETH_HTTP_GET, handleDelRoom);
    ethWeb.on("/togglevmod", ETH_HTTP_GET, handleToggleVMod);
    ethWeb.on("/relay", ETH_HTTP_GET, handleRelay);
    ethWeb.on("/di", ETH_HTTP_GET, handleDI);
    ethWeb.on("/rescan", ETH_HTTP_POST, handleRescan);
    ethWeb.on("/restart", ETH_HTTP_GET, handleRestart);
    ethWeb.on("/logout", ETH_HTTP_GET, handleLogout);
    ethWeb.on("/scanstatus", ETH_HTTP_GET, handleApiScan);
    ethWeb.on("/api/status", ETH_HTTP_GET, handleApiStatus);
    ethWeb.on("/api/config", ETH_HTTP_GET, handleApiConfig);
    ethWeb.on("/api/modules", ETH_HTTP_GET, handleApiModules);
    ethWeb.on("/api/direlay", ETH_HTTP_POST, handleApiDiRelay);
    ethWeb.on("/api/diedge", ETH_HTTP_POST, handleApiDiEdge);
    // ── Register config API (LAN) ────────────────────────────
    ethWeb.on("/api/registers", ETH_HTTP_GET, handleApiRegisters);
    ethWeb.on("/api/register/add", ETH_HTTP_POST, handleApiRegisterAdd);
    ethWeb.on("/api/register/edit", ETH_HTTP_POST, handleApiRegisterEdit);
    ethWeb.on("/api/register/delete", ETH_HTTP_POST, handleApiRegisterDelete);
    ethWeb.on("/api/register/toggle", ETH_HTTP_POST, handleApiRegisterToggle);
    ethWeb.on("/api/sd/save", ETH_HTTP_POST, handleApiSdSave);
    ethWeb.on("/api/sd/list", ETH_HTTP_GET, handleApiSdList);
    ethWeb.on("/api/sd/read", ETH_HTTP_GET, handleApiSdRead);
    ethWeb.on("/api/sd/del", ETH_HTTP_POST, handleApiSdDel); // POST (ETH server has no HTTP_DELETE)
    ethWeb.on("/api/sd/toggle", ETH_HTTP_POST, handleApiSdToggle);
    ethWeb.on("/api/sd/init", ETH_HTTP_POST, handleApiSdInit);
    // ── SD Card browser page & API (LAN) ──────────────────
    ethWeb.on("/sd", ETH_HTTP_GET, handleSdCard);
#ifdef USE_STORAGE
    ethWeb.on("/storage", ETH_HTTP_GET, handleStorage);
#endif
    ethWeb.on("/scan", ETH_HTTP_GET, handleScan);
    ethWeb.on("/api/sd/browse", ETH_HTTP_GET, handleApiSdBrowse);
    ethWeb.on("/api/sd/view", ETH_HTTP_GET, handleApiSdView);
    ethWeb.on("/api/sd/remove", ETH_HTTP_POST, handleApiSdRemove);
    ethWeb.on("/api/sd/mkdir", ETH_HTTP_POST, handleApiSdMkdir);
    ethWeb.on("/api/sd/format", ETH_HTTP_POST, handleApiSdFormat);
    ethWeb.on("/api/sd/upload", ETH_HTTP_POST, handleApiSdUpload);
    ethWeb.on("/api/lan", ETH_HTTP_GET, handleApiLan);
    ethWeb.on("/api/diag", ETH_HTTP_GET, handleApiDiag);
#ifdef USE_WS2812
    ethWeb.on("/api/led", ETH_HTTP_GET, handleApiLed);
    ethWeb.on("/api/led/set", ETH_HTTP_POST, handleApiLedSet);
    ethWeb.on("/led", ETH_HTTP_GET, handleLed);
#endif
#ifdef USE_SD
    ethWeb.on("/api/sd/gpio", ETH_HTTP_GET, handleApiSdGpio);
    ethWeb.on("/api/sd/test", ETH_HTTP_GET, handleApiSdTest);
#endif
#ifdef USE_STORAGE
    ethWeb.on("/api/storage/list", ETH_HTTP_GET, handleApiStorageList);
    ethWeb.on("/api/storage/read", ETH_HTTP_GET, handleApiStorageRead);
    ethWeb.on("/api/storage/write", ETH_HTTP_POST, handleApiStorageWrite);
    ethWeb.on("/api/storage/delete", ETH_HTTP_POST, handleApiStorageDelete);
#endif
    ethWeb.on("/api/mb/write", ETH_HTTP_POST, handleApiMbWrite);
    ethWeb.on("/api/mb/writeid", ETH_HTTP_POST, handleApiMbWriteId);
    ethWeb.on("/api/mb/scan", ETH_HTTP_POST, handleApiMbScan);
    ethWeb.on("/api/mb/scan/result", ETH_HTTP_GET, handleApiMbScanResult);
    ethWeb.on("/api/mb/regscan", ETH_HTTP_GET, handleApiMbRegScan);
    ethWeb.on("/api/mb/coil", ETH_HTTP_POST, handleApiMbCoilWrite);
    ethWeb.on("/api/backup", ETH_HTTP_GET, handleApiBackup);
    ethWeb.on("/api/restore", ETH_HTTP_POST, handleApiRestore);
    ethWeb.on("/api/export", ETH_HTTP_GET, handleApiBackup);
    ethWeb.on("/api/import", ETH_HTTP_POST, handleApiRestore);
    // ethWeb.begin() is called in eth_init() after W5500 is up
    LOG_ILN("[WEB] LAN routes registered on EthWebServer");
#endif
}

void web_server_loop()
{
    web.handleClient();
    eth_web_loop(); // W5500 Ethernet web server
    // Feed task WDT after web server processing (large pages can take several seconds)
    esp_task_wdt_reset();
}