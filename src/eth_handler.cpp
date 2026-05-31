/**
 * eth_handler.cpp — LAN/Ethernet Support (W5500 SPI / LAN8720 RMII / IP101 EMAC)
 *
 * v2.2: LAN is ALWAYS primary. Auto-fallback to WiFi when LAN is down.
 * Auto-switch back to LAN when it comes back up.
 *
 * cfg.lan_type: 0=W5500 SPI (Waveshare ESP32-S3-ETH), 1=LAN8720 RMII, 2=IP101 EMAC
 *
 * Waveshare ESP32-S3-ETH v1.0 uses W5500 SPI chip (NOT IP101!)
 * W5500 pinout: CS=GPIO14, RST=GPIO9, INT=GPIO10, MISO=GPIO12, MOSI=GPIO11, SCK=GPIO13
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include "modbus_mqtt_ha_bridge.h"

// ─── URL Decode helper (for /api/save-wifi query params) ──────
static String urlDecode(const String &s)
{
    String out;
    out.reserve(s.length());
    for (unsigned i = 0; i < s.length(); i++)
    {
        char c = s.charAt(i);
        if (c == '%' && i + 2 < s.length())
        {
            char hex[3] = {s.charAt(i + 1), s.charAt(i + 2), 0};
            out += (char)strtol(hex, nullptr, 16);
            i += 2;
        }
        else if (c == '+')
        {
            out += ' ';
        }
        else
        {
            out += c;
        }
    }
    return out;
}

// ─── State ─────────────────────────────────────────────────────
static bool lan_connected = false;
static bool lan_started = false;
static volatile bool w5500_task_done = false; // set by FreeRTOS task when init complete
static volatile bool w5500_task_ok = false;   // true if DHCP/Static succeeded

// ─── Built-in EMAC Support (LAN8720 / IP101) ──────────────────
#if __has_include(<ETH.h>)
#include <ETH.h>
#define HAS_EMAC
#endif

// ─── W5500 Support (SPI Ethernet) ─────────────────────────────
// Use USE_W5500 build flag instead of __has_include — lib_deps headers
// may not be in __has_include search path even though they compile fine.
#ifdef USE_W5500
#include <SPI.h>
#include <Ethernet.h>
#define HAS_W5500

static byte eth_mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
static EthernetClient eth_tcp_client;

// EthernetServer subclass to satisfy ESP32 Arduino core's pure virtual begin(uint16_t)
class EthServer : public EthernetServer {
  public:
    explicit EthServer(uint16_t port) : EthernetServer(port) {}
    void begin(uint16_t) override { EthernetServer::begin(); }
    using EthernetServer::begin; // expose EthernetServer::begin() without args
};

static EthServer eth_web_server(80);
#endif

// ─── ETH Event Handler (LAN8720 & IP101) ──────────────────────
#ifdef HAS_EMAC
void onEthEvent(arduino_event_id_t event)
{
    switch (event)
    {
    case ARDUINO_EVENT_ETH_GOT_IP:
        LOG_I("[LAN] ✓ Got IP: %s\n", ETH.localIP().toString().c_str());
        if (ETH.localIP() != IPAddress(0, 0, 0, 0))
        {
            lan_connected = true;
        }
        break;
    case ARDUINO_EVENT_ETH_CONNECTED:
        LOG_ILN("[LAN] Link up");
        break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
        LOG_ILN("[LAN] ✗ Link down — falling back to WiFi");
        lan_connected = false;
        break;
    default:
        break;
    }
}
#endif

void eth_init()
{
    if (!cfg.lan_enabled)
    {
        LOG_ILN("[LAN] Disabled in config — WiFi only mode");
        return;
    }

    const char *type_name = cfg.lan_type == 0 ? "W5500-SPI" : cfg.lan_type == 1 ? "LAN8720-RMII" : "IP101-EMAC";
    LOG_I("[LAN] Initializing type=%s\n", type_name);

    if (cfg.lan_type == 0)
    {
        // ── W5500 via SPI — init happens in FreeRTOS task (no WDT conflict) ──
#ifdef HAS_W5500
        LOG_ILN("[LAN] W5500 init will start via FreeRTOS task in eth_loop()");
#else
        LOG_ELN("[LAN] W5500 lib not available! Install Ethernet lib.");
        return;
#endif
    }
    else if (cfg.lan_type == 1)
    {
        // ── LAN8720 via RMII ────────────────────────────────────
#ifdef HAS_EMAC
        WiFi.onEvent(onEthEvent);

        ETH.begin(1, cfg.pin_eth_rst >= 0 ? cfg.pin_eth_rst : -1, 23, 18, ETH_PHY_LAN8720);

        if (!cfg.lan_dhcp && strlen(cfg.lan_ip) > 0)
        {
            IPAddress ip, gw, mask, dns;
            ip.fromString(cfg.lan_ip);
            gw.fromString(cfg.lan_gw);
            mask.fromString(cfg.lan_mask);
            dns.fromString(cfg.lan_dns);
            ETH.config(ip, gw, mask, dns);
        }

        lan_started = true;
        // No blocking wait — event-driven
#else
        LOG_ELN("[LAN] LAN8720 lib not available!");
        return;
#endif
    }
    else if (cfg.lan_type == 2)
    {
        // ── IP101 via built-in EMAC (Waveshare ESP32-S3-ETH) ────
#ifdef HAS_EMAC
        WiFi.onEvent(onEthEvent);

        ETH.begin(1, cfg.pin_eth_rst >= 0 ? cfg.pin_eth_rst : -1, 23, 18, ETH_PHY_IP101);

        if (!cfg.lan_dhcp && strlen(cfg.lan_ip) > 0)
        {
            IPAddress ip, gw, mask, dns;
            ip.fromString(cfg.lan_ip);
            gw.fromString(cfg.lan_gw);
            mask.fromString(cfg.lan_mask);
            dns.fromString(cfg.lan_dns);
            ETH.config(ip, gw, mask, dns);
        }

        lan_started = true;
        // No blocking wait — event-driven
#else
        LOG_ELN("[LAN] IP101 lib not available!");
        return;
#endif
    }

    if (lan_connected)
    {
        LOG_I("[LAN] ✓ Connected! IP: %s (PRIMARY)\n", eth_get_ip().c_str());
    }
    else
    {
        LOG_ILN("[LAN] ⚠ No link yet — WiFi will be used as fallback");
    }
}

void eth_loop()
{
    if (!cfg.lan_enabled)
        return;

    // ── W5500: Launch FreeRTOS task for blocking Ethernet.begin() ──
    if (cfg.lan_type == 0 && !lan_started && !w5500_task_done)
    {
#ifdef HAS_W5500
        static bool task_created = false;
        if (!task_created)
        {
            // Wait 3s for WiFi/MQTT to stabilize first
            static uint32_t boot_time = 0;
            if (boot_time == 0)
                boot_time = millis();
            if (millis() - boot_time < 3000)
                return;

            LOG_ILN("[LAN] Creating W5500 init task...");
            xTaskCreate(
                    [](void *param) {
                        // Step 1: RST pulse
                        if (cfg.pin_eth_rst >= 0)
                        {
                            pinMode(cfg.pin_eth_rst, OUTPUT);
                            digitalWrite(cfg.pin_eth_rst, LOW);
                            vTaskDelay(pdMS_TO_TICKS(10));
                            digitalWrite(cfg.pin_eth_rst, HIGH);
                            vTaskDelay(pdMS_TO_TICKS(150));
                        }
                        LOG_ILN("[LAN-TASK] RST OK");

                        // Step 2: SPI + init
                        SPI.begin(cfg.pin_eth_sclk, cfg.pin_eth_miso, cfg.pin_eth_mosi, cfg.pin_eth_cs);
                        Ethernet.init(cfg.pin_eth_cs);
                        LOG_ILN("[LAN-TASK] SPI init OK");

                        // Step 3: DHCP or Static IP (blocking call — safe in dedicated task)
                        bool ok = false;
                        if (cfg.lan_dhcp)
                        {
                            int result = Ethernet.begin(eth_mac, 5000, 2000);
                            if (result == 0)
                            {
                                LOG_ELN("[LAN-TASK] DHCP failed");
                            }
                            else
                            {
                                LOG_I("[LAN-TASK] ✓ DHCP OK! IP: %s\n", Ethernet.localIP().toString().c_str());
                                ok = true;
                            }
                        }
                        else
                        {
                            IPAddress ip, gw, mask, dns;
                            ip.fromString(cfg.lan_ip);
                            gw.fromString(cfg.lan_gw);
                            mask.fromString(cfg.lan_mask);
                            dns.fromString(cfg.lan_dns);
                            Ethernet.begin(eth_mac, ip, dns, gw, mask);
                            ok = true;
                            LOG_I("[LAN-TASK] ✓ Static IP: %s\n", Ethernet.localIP().toString().c_str());
                        }

                        w5500_task_ok = ok;
                        w5500_task_done = true;
                        LOG_I("[LAN-TASK] hw=%s link=%s — task done, deleting self\n",
                              Ethernet.hardwareStatus() == EthernetW5500 ? "W5500" : "NO_HW",
                              Ethernet.linkStatus() == LinkON ? "ON" : "OFF");
                        vTaskDelete(NULL); // self-delete
                    },
                    "w5500_init",
                    4096, // stack size
                    NULL,
                    1, // low priority (don't starve main loop)
                    NULL);
            task_created = true;
        }
        // Task is running — just return, eth_loop() will check w5500_task_done next iteration
        return;
#endif // HAS_W5500
    }

    // ── W5500: Task completed — finalize setup ──
    if (cfg.lan_type == 0 && !lan_started && w5500_task_done)
    {
#ifdef HAS_W5500
        if (w5500_task_ok)
        {
            lan_connected = true;
        }
        lan_started = true;
        LOG_I("[LAN] W5500 init complete: ip=%s connected=%s\n",
              Ethernet.localIP().toString().c_str(),
              lan_connected ? "true" : "false");

        // SPI.begin() in the FreeRTOS task can disrupt WiFi — force reconnect
        if (WiFi.status() != WL_CONNECTED)
        {
            LOG_ILN("[LAN] WiFi disrupted by SPI init — reconnecting...");
            WiFi.reconnect();
        }
#endif
    }

    // ── W5500 link monitoring + DHCP maintain ──
    static uint32_t last_check = 0;
    if (millis() - last_check > 3000)
    {
        if (cfg.lan_type == 0 && lan_started)
        {
#ifdef HAS_W5500
            Ethernet.maintain(); // DHCP lease renewal

            bool link = (Ethernet.linkStatus() == LinkON);
            if (link && !lan_connected)
            {
                if (Ethernet.localIP() != IPAddress(0, 0, 0, 0))
                {
                    lan_connected = true;
                    LOG_I("[LAN] ✓ W5500 UP! IP: %s — switching to LAN\n", Ethernet.localIP().toString().c_str());
                }
            }
            else if (!link && lan_connected)
            {
                lan_connected = false;
                LOG_ILN("[LAN] ✗ W5500 link DOWN — falling back to WiFi");
            }
#endif
        }
        // LAN8720 and IP101 events handled by onEthEvent callback
        last_check = millis();
    }

    // ── W5500 DHCP retry every 30s (via FreeRTOS task to avoid blocking loop) ──
    if (cfg.lan_type == 0 && lan_started && !lan_connected)
    {
#ifdef HAS_W5500
        static uint32_t last_dhcp_retry = 0;
        if (millis() - last_dhcp_retry > 30000)
        {
            if (Ethernet.linkStatus() == LinkON && Ethernet.localIP() == IPAddress(0, 0, 0, 0))
            {
                LOG_ILN("[LAN] W5500 retrying DHCP via task...");
                static volatile bool retry_task_running = false;
                if (!retry_task_running)
                {
                    retry_task_running = true;
                    xTaskCreate(
                            [](void *param) {
                                int result = Ethernet.begin(eth_mac, 5000, 2000);
                                if (result == 0)
                                {
                                    LOG_ELN("[LAN-RETRY] DHCP failed");
                                }
                                else
                                {
                                    LOG_I("[LAN-RETRY] ✓ DHCP OK! IP: %s\n", Ethernet.localIP().toString().c_str());
                                    lan_connected = true;
                                }
                                retry_task_running = false;
                                vTaskDelete(NULL);
                            },
                            "w5500_retry",
                            4096,
                            NULL,
                            1,
                            NULL);
                }
            }
            last_dhcp_retry = millis();
        }
#endif
    }
}

bool eth_is_connected()
{
    return lan_connected;
}

bool eth_is_started()
{
    return lan_started;
}

void eth_set_connected(bool v)
{
    lan_connected = v;
}

void eth_set_started(bool v)
{
    lan_started = v;
    // Start Ethernet web server when W5500 comes online
#ifdef HAS_W5500
    if (v && cfg.lan_type == 0)
    {
        eth_web_server.begin();
        LOG_I("[LAN] Web server started on %s:80\n", Ethernet.localIP().toString().c_str());
    }
#endif
}

String eth_get_ip()
{
    if (!lan_connected || !lan_started)
        return "0.0.0.0";

    if (cfg.lan_type == 0)
    {
#ifdef HAS_W5500
        return Ethernet.localIP().toString();
#endif
    }
    else if (cfg.lan_type == 1 || cfg.lan_type == 2)
    {
#ifdef HAS_EMAC
        return ETH.localIP().toString();
#endif
    }
    return "0.0.0.0";
}

// ─── Ethernet Web Server (W5500 LAN interface) ──────────────────
// Handles HTTP requests arriving via the W5500 Ethernet interface.
// The WiFi WebServer (WebServer web(80)) only listens on WiFi — it
// cannot see W5500 connections because the Arduino Ethernet library
// uses its own socket stack, not lwIP. So we run a separate
// EthernetServer on port 80 that mirrors the key API endpoints.

#ifdef HAS_W5500
static void eth_send_json(EthernetClient &client, int code, const char *json)
{
    client.printf(
            "HTTP/1.1 %d OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: %d\r\n\r\n%s",
            code,
            strlen(json),
            json);
    client.stop();
}

static void eth_send_text(EthernetClient &client, int code, const char *text)
{
    client.printf("HTTP/1.1 %d OK\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: %d\r\n\r\n%s",
                  code,
                  strlen(text),
                  text);
    client.stop();
}

static void eth_send_redirect_wifi(EthernetClient &client)
{
    // Redirect browser to WiFi IP for full HTML pages
    char buf[128];
    snprintf(buf,
             sizeof(buf),
             "HTTP/1.1 302 Found\r\nLocation: http://%s/\r\nConnection: close\r\nContent-Length: 0\r\n\r\n",
             WiFi.localIP().toString().c_str());
    client.print(buf);
    client.stop();
}

// ── LAN auth check (simple query-param auth for raw EthernetClient) ──
// Returns true if auth is OK or auth is disabled.
// query = full path including ?query string
static bool eth_auth_ok(EthernetClient &client, const String &path)
{
    if (!cfg.web_auth || strlen(cfg.web_pass) == 0)
        return true; // Auth disabled
    // Look for auth=PASSWORD in query string
    String authParam = "auth=" + String(cfg.web_pass);
    if (path.indexOf(authParam) >= 0)
        return true;
    // Auth failed — return 401
    client.println("HTTP/1.1 401 Unauthorized");
    client.println("Content-Type: application/json");
    client.println("Connection: close");
    client.println();
    client.println("{\"error\":\"unauthorized\",\"hint\":\"add ?auth=PASSWORD to request\"}");
    client.stop();
    return false;
}

static void eth_handle_api_status(EthernetClient &client)
{
    // Mirror of handleApiStatus() — JSON status response
    client.printf("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                  "{\"firmware\":\"%s\",\"hostname\":\"%s\","
                  "\"interface\":\"LAN\","
                  "\"ip\":\"%s\",\"wifi_ip\":\"%s\","
                  "\"wifi_rssi\":%d,"
                  "\"mqtt_connected\":%s,"
                  "\"mqtt_tls\":%s,"
                  "\"uptime_s\":%lu,"
                  "\"lan_started\":%s,\"lan_connected\":%s,"
                  "\"lan_hw\":\"%s\",\"lan_link\":\"%s\","
                  "\"modules\":%d,"
                  "\"modbus\":{\"poll_ms\":%u,\"errors\":%lu},"
                  "\"tcp_bridge\":%s,"
                  "\"free_heap\":%lu,"
                  "\"lan_type\":%d}\n",
                  FIRMWARE_VERSION,
                  cfg.hostname,
                  Ethernet.localIP().toString().c_str(),
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI(),
                  mqtt_is_connected() ? "true" : "false",
                  cfg.mqtt_tls ? "true" : "false",
                  (unsigned long)(millis() / 1000),
                  lan_started ? "true" : "false",
                  lan_connected ? "true" : "false",
                  Ethernet.hardwareStatus() == EthernetW5500 ? "W5500" : "NO_HW",
                  Ethernet.linkStatus() == LinkON ? "ON" : "OFF",
                  module_count,
                  cfg.mb_poll_ms,
                  // modbus_error_count — approximate
                  (unsigned long)0,
                  cfg.tcp_enabled ? "true" : "false",
                  (unsigned long)ESP.getFreeHeap(),
                  cfg.lan_type);
    client.stop();
}

static void eth_handle_api_lan(EthernetClient &client)
{
    // Mirror of handleApiLan() — LAN debug info
    client.printf("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
                  "{\"lan_enabled\":%s,\"lan_type\":%d,\"lan_started\":%s,\"lan_connected\":%s,"
                  "\"hw\":\"%s\",\"link\":\"%s\","
                  "\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\","
                  "\"pins\":{\"rst\":%d,\"cs\":%d,\"sclk\":%d,\"miso\":%d,\"mosi\":%d,\"int\":%d}}\n",
                  cfg.lan_enabled ? "true" : "false",
                  cfg.lan_type,
                  lan_started ? "true" : "false",
                  lan_connected ? "true" : "false",
                  Ethernet.hardwareStatus() == EthernetW5500 ? "W5500" : "NO_HW",
                  Ethernet.linkStatus() == LinkON ? "ON" : "OFF",
                  Ethernet.localIP().toString().c_str(),
                  Ethernet.subnetMask().toString().c_str(),
                  Ethernet.gatewayIP().toString().c_str(),
                  cfg.pin_eth_rst,
                  cfg.pin_eth_cs,
                  cfg.pin_eth_sclk,
                  cfg.pin_eth_miso,
                  cfg.pin_eth_mosi,
                  cfg.pin_eth_int);
    client.stop();
}
#endif

void eth_web_loop()
{
    if (!lan_started || !lan_connected)
        return;

#ifdef HAS_W5500
    EthernetClient client = eth_web_server.available();
    if (!client)
        return;

    // Read HTTP request line (first line only — GET /path HTTP/1.1)
    String req = client.readStringUntil('\n');
    req.trim();

    // Skip remaining headers (with timeout)
    uint32_t t = millis();
    while (client.connected() && millis() - t < 500)
    {
        String hdr = client.readStringUntil('\n');
        if (hdr.length() <= 1)
            break; // empty line = end of headers
    }

    // Parse: METHOD PATH HTTP/1.x
    int sp1 = req.indexOf(' ');
    int sp2 = req.indexOf(' ', sp1 + 1);
    if (sp1 < 0 || sp2 < 0)
    {
        eth_send_text(client, 400, "Bad Request");
        return;
    }
    String method = req.substring(0, sp1);
    String fullPath = req.substring(sp1 + 1, sp2);
    // Split path and query string
    int qi = fullPath.indexOf('?');
    String path = (qi > 0) ? fullPath.substring(0, qi) : fullPath;
    String query = (qi > 0) ? fullPath.substring(qi + 1) : "";

    LOG_D("[LAN-WEB] %s %s?%s\n", method.c_str(), path.c_str(), query.c_str());

    // ── Route ──
    // All routes require auth when web_auth is enabled
    // Pass fullPath (path+query) to eth_auth_ok for ?auth=PASSWORD check
    if (path == "/api/status")
    {
        if (!eth_auth_ok(client, fullPath))
            return;
        eth_handle_api_status(client);
    }
    else if (path == "/api/lan")
    {
        if (!eth_auth_ok(client, fullPath))
            return;
        eth_handle_api_lan(client);
    }
    else if (path.startsWith("/api/"))
    {
        // ── Extended API: /api/reboot, /api/wifi-reconnect, /api/save-wifi ──
        if (path == "/api/reboot")
        {
            if (!eth_auth_ok(client, fullPath))
                return;
            eth_send_text(client, 200, "Rebooting...");
            delay(500);
            ESP.restart();
        }
        else if (path == "/api/wifi-reconnect")
        {
            WiFi.reconnect();
            eth_send_json(client, 200, "{\"status\":\"wifi_reconnect_sent\"}");
        }
        else if (path.startsWith("/api/save-wifi"))
        {
            // ── Save WiFi+MQTT config via query params (for LAN-only access) ──
            // See /api/save-wifi?ssid=X&wpass=Y&...
            if (!eth_auth_ok(client, fullPath))
                return;
            Preferences nv;
            nv.begin(NV_NAMESPACE, false);
            if (query.length() > 0)
            {
                String qs = query;
                // Simple query string parser: key=value&key=value
                int pos = 0;
                while (pos < (int)qs.length())
                {
                    int eq = qs.indexOf('=', pos);
                    if (eq < 0)
                        break;
                    int amp = qs.indexOf('&', eq);
                    String key = urlDecode(qs.substring(pos, eq));
                    String val = (amp > 0) ? urlDecode(qs.substring(eq + 1, amp)) : urlDecode(qs.substring(eq + 1));
                    LOG_I("[LAN-WEB] save-wifi: key=[%s] val=[%s]\n", key.c_str(), val.c_str());
                    // Map web.arg names to NVRAM keys (same as /save handler)
                    if (key == "ssid")
                        nv.putString(NV_KEY_WIFI_SSID, val);
                    else if (key == "wpass")
                        nv.putString(NV_KEY_WIFI_PASS, val);
                    else if (key == "hostname")
                        nv.putString(NV_KEY_HOSTNAME, val);
                    else if (key == "mhost")
                        nv.putString(NV_KEY_MQTT_HOST, val);
                    else if (key == "mport")
                        nv.putUInt(NV_KEY_MQTT_PORT, val.toInt());
                    else if (key == "muser")
                        nv.putString(NV_KEY_MQTT_USER, val);
                    else if (key == "mpass")
                        nv.putString(NV_KEY_MQTT_PASS, val);
                    else if (key == "mpfx")
                        nv.putString(NV_KEY_MQTT_PREFIX, val);
                    else if (key == "lanen")
                        nv.putBool(NV_KEY_ETH_EN, val == "1");
                    else if (key == "landhcp")
                        nv.putBool(NV_KEY_ETH_DHCP, val == "1");
                    else if (key == "hadisc")
                        nv.putBool(NV_KEY_HA_DISC, val == "1");
                    else if (key == "vmod")
                        nv.putBool(NV_KEY_VIRTUAL_MOD, val == "1");
                    else if (key == "mbprof")
                        nv.putUChar(NV_KEY_MB_PROFILE, (uint8_t)val.toInt());
                    else if (key == "mbpoll")
                        nv.putUInt(NV_KEY_MB_POLL_MS, val.toInt());
                    else if (key == "mstart")
                        nv.putUChar(NV_KEY_MB_START, (uint8_t)val.toInt());
                    else if (key == "mend")
                        nv.putUChar(NV_KEY_MB_END, (uint8_t)val.toInt());
                    else if (key == "tcpen")
                        nv.putBool(NV_KEY_TCP_EN, val == "1");
                    else if (key == "tcpp")
                        nv.putUInt(NV_KEY_TCP_PORT, val.toInt());
                    pos = (amp > 0) ? amp + 1 : qs.length();
                }
            }
            nv.end();
            // Reload config from NVRAM
            config_load();
            LOG_ILN("[LAN-WEB] WiFi+MQTT config saved via /api/save-wifi");
            eth_send_json(client, 200, "{\"status\":\"saved\",\"reboot\":true}");
            delay(500);
            ESP.restart();
        }
        else if (path == "/api/nvram")
        {
            // ── Debug: dump key NVRAM values ──
            Preferences nv;
            nv.begin(NV_NAMESPACE, true);
            char ssid[64] = {};
            nv.getString(NV_KEY_WIFI_SSID, ssid, sizeof(ssid));
            char wpass[64] = {};
            nv.getString(NV_KEY_WIFI_PASS, wpass, 4); // only first 3 chars for security
            char mhost[64] = {};
            nv.getString(NV_KEY_MQTT_HOST, mhost, sizeof(mhost));
            uint16_t mport = nv.getUInt(NV_KEY_MQTT_PORT, 0);
            bool lanen = nv.getBool(NV_KEY_ETH_EN, false);
            char hostname[64] = {};
            nv.getString(NV_KEY_HOSTNAME, hostname, sizeof(hostname));
            nv.end();
            char buf[512];
            snprintf(buf,
                     sizeof(buf),
                     "{\"ssid\":\"%s\",\"wpass_ok\":%s,\"mhost\":\"%s\",\"mport\":%u,\"lanen\":%s,\"hostname\":\"%s\"}",
                     ssid,
                     (wpass[0] ? "\"true\"" : "\"false\""),
                     mhost,
                     mport,
                     lanen ? "true" : "false",
                     hostname);
            eth_send_json(client, 200, buf);

            // ── /api/export — Full config export (all NVRAM keys + module names) ──
        }
        else if (path.startsWith("/api/export"))
        {
            if (!eth_auth_ok(client, fullPath))
                return;
            Preferences nv;
            nv.begin(NV_NAMESPACE, true);

            // String NVRAM keys (config values)
            const char *str_keys[] = {NV_KEY_WIFI_SSID,
                                      NV_KEY_WIFI_PASS,
                                      NV_KEY_HOSTNAME,
                                      NV_KEY_MQTT_HOST,
                                      NV_KEY_MQTT_USER,
                                      NV_KEY_MQTT_PASS,
                                      NV_KEY_MQTT_PREFIX,
                                      NV_KEY_AP_NAME,
                                      NV_KEY_WIFI_IP,
                                      NV_KEY_WIFI_GW,
                                      NV_KEY_WIFI_MASK,
                                      NV_KEY_WIFI_DNS,
                                      NV_KEY_ETH_IP,
                                      NV_KEY_ETH_GW,
                                      NV_KEY_ETH_MASK,
                                      NV_KEY_ETH_DNS,
                                      NV_KEY_WEB_PASS};
            const char *str_names[] = {"ssid",
                                       "wpass",
                                       "hostname",
                                       "mhost",
                                       "muser",
                                       "mpass",
                                       "mpfx",
                                       "apn",
                                       "wip",
                                       "wgw",
                                       "wmask",
                                       "wdns",
                                       "eip",
                                       "egw",
                                       "emask",
                                       "edns",
                                       "wauthp"};
            const int str_count = sizeof(str_keys) / sizeof(str_keys[0]);

            // UInt NVRAM keys
            const char *uint_keys[] = {NV_KEY_MQTT_PORT,    NV_KEY_MB_BAUD,      NV_KEY_MB_POLL_MS,
                                       NV_KEY_MB_START,     NV_KEY_MB_END,       NV_KEY_MB_PARITY,
                                       NV_KEY_TCP_PORT,     NV_KEY_PIN_RS485_RX, NV_KEY_PIN_RS485_TX,
                                       NV_KEY_PIN_RS485_DE, NV_KEY_PIN_LED,      NV_KEY_PIN_BTN,
                                       NV_KEY_PIN_ETH_MOSI, NV_KEY_PIN_ETH_MISO, NV_KEY_PIN_ETH_SCLK,
                                       NV_KEY_PIN_ETH_CS,   NV_KEY_PIN_ETH_INT,  NV_KEY_PIN_ETH_RST,
                                       NV_KEY_MB_PROFILE,   NV_KEY_MB_REG_COIL,  NV_KEY_MB_REG_DI};
            const char *uint_names[] = {"mport",  "mbaud", "mbpoll", "mstart", "mend",   "mpar",   "tcpp",
                                        "prx",    "ptx",   "pde",    "pled",   "pbtn",   "pemosi", "pemiso",
                                        "pesclk", "pecs",  "peint",  "perst",  "mbprof", "mbcoil", "mbdi"};
            const int uint_count = sizeof(uint_keys) / sizeof(uint_keys[0]);

            // Bool NVRAM keys
            const char *bool_keys[] = {NV_KEY_ETH_EN,
                                       NV_KEY_ETH_DHCP,
                                       NV_KEY_WIFI_DHCP,
                                       NV_KEY_TCP_EN,
                                       NV_KEY_HA_DISC,
                                       NV_KEY_VIRTUAL_MOD,
                                       NV_KEY_MQTT_TLS,
                                       NV_KEY_WEB_AUTH};
            const char *bool_names[] = {"ethen", "edhcp", "wdhcp", "tcpen", "hadisc", "vmod", "mtls", "wauth"};
            const int bool_count = sizeof(bool_keys) / sizeof(bool_keys[0]);

            // Ethernet type (special uint8)
            uint8_t etype = nv.getUChar(NV_KEY_ETH_TYPE, 0);

            // WiFi mode
            uint8_t wmode = nv.getUChar(NV_KEY_WIFI_MODE, 0);

            // Net priority
            uint8_t nprio = nv.getUChar(NV_KEY_NET_PRIO, 0);

            // Module list
            uint8_t mlist_n = nv.getUChar(NV_KEY_MOD_LIST_N, 0);

            // Build JSON — use a large buffer since we have module names too
            String json = "{";

            // String keys
            for (int i = 0; i < str_count; i++)
            {
                char val[128] = {};
                nv.getString(str_keys[i], val, sizeof(val));
                // Escape quotes in values
                String escaped = String(val);
                escaped.replace("\\", "\\\\");
                escaped.replace("\"", "\\\"");
                if (i > 0)
                    json += ",";
                json += "\"" + String(str_names[i]) + "\":\"" + escaped + "\"";
            }

            // UInt keys
            for (int i = 0; i < uint_count; i++)
            {
                uint16_t val = nv.getUInt(uint_keys[i], 0);
                char buf[8];
                snprintf(buf, sizeof(buf), "%u", val);
                json += ",\"" + String(uint_names[i]) + "\":" + String(buf);
            }

            // Bool keys
            for (int i = 0; i < bool_count; i++)
            {
                bool val = nv.getBool(bool_keys[i], false);
                json += ",\"" + String(bool_names[i]) + "\":" + (val ? "true" : "false");
            }

            // Special uint8 keys
            char tmp[8];
            snprintf(tmp, sizeof(tmp), "%u", etype);
            json += ",\"etype\":" + String(tmp);
            snprintf(tmp, sizeof(tmp), "%u", wmode);
            json += ",\"wifimode\":" + String(tmp);
            snprintf(tmp, sizeof(tmp), "%u", nprio);
            json += ",\"nprio\":" + String(tmp);
            snprintf(tmp, sizeof(tmp), "%u", mlist_n);
            json += ",\"mlist_n\":" + String(tmp);

            // Module list addresses and model IDs
            json += ",\"mlist_a\":[";
            for (uint8_t i = 0; i < mlist_n && i < 16; i++)
            {
                char akey[12];
                snprintf(akey, sizeof(akey), "%s%u", NV_KEY_MOD_ADDR, i);
                if (i > 0)
                    json += ",";
                json += String(nv.getUChar(akey, 0));
            }
            json += "],\"mlist_m\":[";
            for (uint8_t i = 0; i < mlist_n && i < 16; i++)
            {
                char mkey[12];
                snprintf(mkey, sizeof(mkey), "%s%u", NV_KEY_MOD_MODEL, i);
                if (i > 0)
                    json += ",";
                json += String(nv.getUChar(mkey, 0));
            }
            json += "]";

            // Per-module names (mn<addr>, hn<addr>, ar<addr>, rn<addr>_<idx>, dn<addr>_<idx>)
            json += ",\"modules\":{";
            bool first_mod = true;
            for (uint8_t mi = 0; mi < mlist_n && mi < 16; mi++)
            {
                char akey[12];
                snprintf(akey, sizeof(akey), "%s%u", NV_KEY_MOD_ADDR, mi);
                uint8_t addr = nv.getUChar(akey, 0);
                if (addr == 0)
                    continue;

                // Get model to know relay/DI counts
                char mkey[12];
                snprintf(mkey, sizeof(mkey), "%s%u", NV_KEY_MOD_MODEL, mi);
                uint8_t mid = nv.getUChar(mkey, 0);
                // Lookup max counts from model (conservative: read up to 16 each)
                uint8_t max_r = 16, max_di = 16;
                // Use known max for common models
                if (mid == 200)
                {
                    max_r = 6;
                    max_di = 6;
                } // KinCony F16
                else if (mid == 100)
                {
                    max_r = 8;
                    max_di = 8;
                } // KinCony A8
                else if (mid == 201)
                {
                    max_r = 16;
                    max_di = 16;
                } // KinCony A16
                else
                {
                    max_r = 6;
                    max_di = 6;
                } // default

                if (!first_mod)
                    json += ",";
                first_mod = false;

                char mod_key[8];
                snprintf(mod_key, sizeof(mod_key), "%u", addr);
                json += "\"" + String(mod_key) + "\":{";

                // Module name (mn<addr>)
                char mnk[8];
                snprintf(mnk, sizeof(mnk), "mn%u", addr);
                char mnv[64] = {};
                nv.getString(mnk, mnv, sizeof(mnv));
                String escaped_mn = String(mnv);
                escaped_mn.replace("\\", "\\\\");
                escaped_mn.replace("\"", "\\\"");
                json += "\"name\":\"" + escaped_mn + "\"";

                // HA name (hn<addr>)
                char hnk[8];
                snprintf(hnk, sizeof(hnk), "hn%u", addr);
                char hnv[64] = {};
                nv.getString(hnk, hnv, sizeof(hnv));
                String escaped_hn = String(hnv);
                escaped_hn.replace("\\", "\\\\");
                escaped_hn.replace("\"", "\\\"");
                json += ",\"ha_name\":\"" + escaped_hn + "\"";

                // Area (ar<addr>)
                char ark[10];
                snprintf(ark, sizeof(ark), "ar%u", addr);
                char arv[64] = {};
                nv.getString(ark, arv, sizeof(arv));
                String escaped_ar = String(arv);
                escaped_ar.replace("\\", "\\\\");
                escaped_ar.replace("\"", "\\\"");
                json += ",\"area\":\"" + escaped_ar + "\"";

                // Relay names
                json += ",\"relays\":[";
                for (uint8_t r = 0; r < max_r; r++)
                {
                    char rnk[12];
                    snprintf(rnk, sizeof(rnk), "rn%u_%u", addr, r);
                    char rnv[64] = {};
                    nv.getString(rnk, rnv, sizeof(rnv));
                    String escaped_rn = String(rnv);
                    escaped_rn.replace("\\", "\\\\");
                    escaped_rn.replace("\"", "\\\"");
                    if (r > 0)
                        json += ",";
                    json += "\"" + escaped_rn + "\"";
                }
                json += "]";

                // DI names
                json += ",\"dis\":[";
                for (uint8_t d = 0; d < max_di; d++)
                {
                    char dnk[12];
                    snprintf(dnk, sizeof(dnk), "dn%u_%u", addr, d);
                    char dnv[64] = {};
                    nv.getString(dnk, dnv, sizeof(dnv));
                    String escaped_dn = String(dnv);
                    escaped_dn.replace("\\", "\\\\");
                    escaped_dn.replace("\"", "\\\"");
                    if (d > 0)
                        json += ",";
                    json += "\"" + escaped_dn + "\"";
                }
                json += "]";

                json += "}"; // end module
            }
            json += "}"; // end modules

            // Firmware version
            json += ",\"fw_version\":\"" + String(FIRMWARE_VERSION) + "\"";

            // Password masking
            json += ",\"passwords_masked\":true";

            json += "}"; // end root
            nv.end();

            // Replace real passwords with "***" in the output
            // (passwords are at known JSON positions — simple string replace)
            json.replace(String("\"wpass\":\"") + nv.getString(NV_KEY_WIFI_PASS, "") + "\"", "\"wpass\":\"***\"");
            json.replace(String("\"mpass\":\"") + nv.getString(NV_KEY_MQTT_PASS, "") + "\"", "\"mpass\":\"***\"");
            json.replace(String("\"wauthp\":\"") + nv.getString(NV_KEY_WEB_PASS, "") + "\"", "\"wauthp\":\"***\"");

            eth_send_json(client, 200, json.c_str());

            // ── /api/import — Full config import from JSON query param body ──
        }
        else if (path.startsWith("/api/import"))
        {
            if (!eth_auth_ok(client, fullPath))
                return;
            // Parse query string from URL for import data
            if (query.length() == 0)
            {
                eth_send_json(client, 400, "{\"error\":\"missing_query_params\"}");
            }
            else
            {
                String qs = query;
                Preferences nv;
                nv.begin(NV_NAMESPACE, false);

                int pos = 0;
                int restored = 0;
                while (pos < (int)qs.length())
                {
                    int amp = qs.indexOf('&', pos);
                    String pair = (amp > 0) ? qs.substring(pos, amp) : qs.substring(pos);
                    pos = (amp > 0) ? amp + 1 : qs.length();

                    int eq = pair.indexOf('=');
                    if (eq < 0)
                        continue;
                    String key = urlDecode(pair.substring(0, eq));
                    String raw_val = urlDecode(pair.substring(eq + 1));

                    // Skip non-config keys
                    if (key == "fw_version" || key.startsWith("mlist_") || key == "modules")
                        continue;

                    // String config keys → putString
                    if (key == "ssid")
                    {
                        nv.putString(NV_KEY_WIFI_SSID, raw_val);
                        restored++;
                    }
                    else if (key == "wpass")
                    {
                        nv.putString(NV_KEY_WIFI_PASS, raw_val);
                        restored++;
                    }
                    else if (key == "hostname")
                    {
                        nv.putString(NV_KEY_HOSTNAME, raw_val);
                        restored++;
                    }
                    else if (key == "mhost")
                    {
                        nv.putString(NV_KEY_MQTT_HOST, raw_val);
                        restored++;
                    }
                    else if (key == "muser")
                    {
                        nv.putString(NV_KEY_MQTT_USER, raw_val);
                        restored++;
                    }
                    else if (key == "mpass")
                    {
                        nv.putString(NV_KEY_MQTT_PASS, raw_val);
                        restored++;
                    }
                    else if (key == "mpfx")
                    {
                        nv.putString(NV_KEY_MQTT_PREFIX, raw_val);
                        restored++;
                    }
                    // UInt config keys → putUInt
                    else if (key == "mport")
                    {
                        nv.putUInt(NV_KEY_MQTT_PORT, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "mbaud")
                    {
                        nv.putUInt(NV_KEY_MB_BAUD, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "mbpoll")
                    {
                        nv.putUInt(NV_KEY_MB_POLL_MS, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "mstart")
                    {
                        nv.putUInt(NV_KEY_MB_START, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "mend")
                    {
                        nv.putUInt(NV_KEY_MB_END, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "mpar")
                    {
                        nv.putUInt(NV_KEY_MB_PARITY, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "tcpp")
                    {
                        nv.putUInt(NV_KEY_TCP_PORT, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "prx")
                    {
                        nv.putUInt(NV_KEY_PIN_RS485_RX, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "ptx")
                    {
                        nv.putUInt(NV_KEY_PIN_RS485_TX, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "pde")
                    {
                        nv.putUInt(NV_KEY_PIN_RS485_DE, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "pled")
                    {
                        nv.putUInt(NV_KEY_PIN_LED, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "pbtn")
                    {
                        nv.putUInt(NV_KEY_PIN_BTN, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "pemosi")
                    {
                        nv.putUInt(NV_KEY_PIN_ETH_MOSI, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "pemiso")
                    {
                        nv.putUInt(NV_KEY_PIN_ETH_MISO, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "pesclk")
                    {
                        nv.putUInt(NV_KEY_PIN_ETH_SCLK, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "pecs")
                    {
                        nv.putUInt(NV_KEY_PIN_ETH_CS, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "peint")
                    {
                        nv.putUInt(NV_KEY_PIN_ETH_INT, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "perst")
                    {
                        nv.putUInt(NV_KEY_PIN_ETH_RST, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "mbprof")
                    {
                        nv.putUInt(NV_KEY_MB_PROFILE, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "mbcoil")
                    {
                        nv.putUInt(NV_KEY_MB_REG_COIL, raw_val.toInt());
                        restored++;
                    }
                    else if (key == "mbdi")
                    {
                        nv.putUInt(NV_KEY_MB_REG_DI, raw_val.toInt());
                        restored++;
                    }
                    // Bool config keys → putBool
                    else if (key == "ethen")
                    {
                        nv.putBool(NV_KEY_ETH_EN, raw_val == "true" || raw_val == "1");
                        restored++;
                    }
                    else if (key == "edhcp")
                    {
                        nv.putBool(NV_KEY_ETH_DHCP, raw_val == "true" || raw_val == "1");
                        restored++;
                    }
                    else if (key == "lanip")
                    {
                        nv.putString(NV_KEY_ETH_IP, raw_val);
                        restored++;
                    }
                    else if (key == "langw")
                    {
                        nv.putString(NV_KEY_ETH_GW, raw_val);
                        restored++;
                    }
                    else if (key == "lanmask")
                    {
                        nv.putString(NV_KEY_ETH_MASK, raw_val);
                        restored++;
                    }
                    else if (key == "landns")
                    {
                        nv.putString(NV_KEY_ETH_DNS, raw_val);
                        restored++;
                    }
                    else if (key == "wdhcp")
                    {
                        nv.putBool(NV_KEY_WIFI_DHCP, raw_val == "true" || raw_val == "1");
                        restored++;
                    }
                    else if (key == "wip")
                    {
                        nv.putString(NV_KEY_WIFI_IP, raw_val);
                        restored++;
                    }
                    else if (key == "wgw")
                    {
                        nv.putString(NV_KEY_WIFI_GW, raw_val);
                        restored++;
                    }
                    else if (key == "wmask")
                    {
                        nv.putString(NV_KEY_WIFI_MASK, raw_val);
                        restored++;
                    }
                    else if (key == "wdns")
                    {
                        nv.putString(NV_KEY_WIFI_DNS, raw_val);
                        restored++;
                    }
                    else if (key == "tcpen")
                    {
                        nv.putBool(NV_KEY_TCP_EN, raw_val == "true" || raw_val == "1");
                        restored++;
                    }
                    else if (key == "hadisc")
                    {
                        nv.putBool(NV_KEY_HA_DISC, raw_val == "true" || raw_val == "1");
                        restored++;
                    }
                    else if (key == "vmod")
                    {
                        nv.putBool(NV_KEY_VIRTUAL_MOD, raw_val == "true" || raw_val == "1");
                        restored++;
                    }
                    else if (key == "mtls")
                    {
                        nv.putBool(NV_KEY_MQTT_TLS, raw_val == "true" || raw_val == "1");
                        restored++;
                    }
                    // UInt8 keys → putUChar
                    else if (key == "etype")
                    {
                        nv.putUChar(NV_KEY_ETH_TYPE, (uint8_t)raw_val.toInt());
                        restored++;
                    }
                    else if (key == "wifimode")
                    {
                        nv.putUChar(NV_KEY_WIFI_MODE, (uint8_t)raw_val.toInt());
                        restored++;
                    }
                    else if (key == "nprio")
                    {
                        nv.putUChar(NV_KEY_NET_PRIO, (uint8_t)raw_val.toInt());
                        restored++;
                    }
                    // Module names (mn<addr>, hn<addr>, ar<addr>, rn<addr>_<idx>, dn<addr>_<idx>)
                    else if (key.startsWith("mn"))
                    {
                        nv.putString(key.c_str(), raw_val);
                        restored++;
                    }
                    else if (key.startsWith("hn"))
                    {
                        nv.putString(key.c_str(), raw_val);
                        restored++;
                    }
                    else if (key.startsWith("ar"))
                    {
                        nv.putString(key.c_str(), raw_val);
                        restored++;
                    }
                    else if (key.startsWith("rn"))
                    {
                        nv.putString(key.c_str(), raw_val);
                        restored++;
                    }
                    else if (key.startsWith("dn"))
                    {
                        nv.putString(key.c_str(), raw_val);
                        restored++;
                    }
                    // Module list
                    else if (key == "mlist_n")
                    {
                        nv.putUChar(NV_KEY_MOD_LIST_N, (uint8_t)raw_val.toInt());
                        restored++;
                    }
                    else if (key.startsWith("mlist_a"))
                    {
                        nv.putUChar(key.c_str(), (uint8_t)raw_val.toInt());
                        restored++;
                    }
                    else if (key.startsWith("mlist_m"))
                    {
                        nv.putUChar(key.c_str(), (uint8_t)raw_val.toInt());
                        restored++;
                    }
                }

                nv.end();

                // Reboot to apply all changes
                char resp[64];
                snprintf(resp, sizeof(resp), "{\"status\":\"imported\",\"keys\":%d,\"reboot\":true}", restored);
                eth_send_json(client, 200, resp);
                delay(500);
                ESP.restart();
            }
        }
        else
        {
            eth_send_json(client, 404, "{\"error\":\"not_found\"}");
        }
    }
    else
    {
        // HTML pages → redirect to WiFi IP (full UI is WiFi-only)
        eth_send_redirect_wifi(client);
    }
#endif
}