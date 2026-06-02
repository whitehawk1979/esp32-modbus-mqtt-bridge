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
#include <HTTPClient.h>
#include <Update.h>
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
#include "EthWebServer.h"
#include "web_adapter.h"
#define HAS_W5500

static byte eth_mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
EthernetClient eth_tcp_client;

// EthernetServer subclass removed — replaced by EthWebServer (web_adapter.h)
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
        extern EthWebServer ethWeb;
        ethWeb.begin();
        LOG_I("[LAN] EthWebServer started on %s:80\n", Ethernet.localIP().toString().c_str());
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
                  "\"mqtt_transport\":\"%s\","
                  "\"mqtt_tls\":%s,"
                  "\"uptime_s\":%lu,"
                  "\"lan_started\":%s,\"lan_connected\":%s,"
                  "\"lan_hw\":\"%s\",\"lan_link\":\"%s\","
                  "\"modules\":%d,"
                  "\"modbus\":{\"poll_ms\":%u,\"errors\":%lu},"
                  "\"tcp_bridge\":%s,"
                  "\"free_heap\":%lu,"
                  "\"psram_free\":%lu,\"psram_total\":%lu,"
                  "\"lan_type\":%d}\n",
                  FIRMWARE_VERSION,
                  cfg.hostname,
                  Ethernet.localIP().toString().c_str(),
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI(),
                  mqtt_is_connected() ? "true" : "false",
                  mqtt_is_on_lan() ? "LAN" : "WiFi",
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
                  (unsigned long)psram_free(),
                  (unsigned long)psram_total(),
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
    // ── Route LAN requests through EthWebServer → same handlers as WiFi ──
    extern WebInterface *WS;
    extern EthWebAdapter lanAdapter;
    WS = &lanAdapter;           // Redirect handler calls to LAN adapter
    ethWeb.handleClient();      // Parse HTTP, match route, call handler
    WS = &wifiAdapter;          // Restore WiFi adapter for next WiFi cycle
#endif
}
