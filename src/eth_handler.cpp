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
static bool w5500_reinit_giveup = false;
static volatile bool w5500_task_done = false; // set by FreeRTOS task when init complete
static volatile bool w5500_task_ok = false;   // true if DHCP/Static succeeded

// ─── Built-in EMAC Support (LAN8720 / IP101) ──────────────────
// Only include Arduino ETH.h for EMAC PHY (LAN8720/IP101) — NOT for W5500 SPI
// W5500 uses Arduino Ethernet library (FSPI bus, same as v2.8.0)
#ifndef USE_W5500
#if __has_include(<ETH.h>)
#include <ETH.h>
#define HAS_EMAC
#endif
#endif

// ─── W5500 Support (SPI Ethernet via Arduino Ethernet library) ──
// Arduino Ethernet library uses W5100 class which talks directly to W5500
// via SPI.beginTransaction() on the FSPI bus — same approach as v2.8.0 which worked!
// NOTE: Do NOT use ETHClass2 (ESP-IDF ETH stack, SPI3_HOST) — it fails on this board!
#ifdef USE_W5500
#include <SPI.h>
#include <Ethernet.h>        // Arduino Ethernet library (patched: SPI.begin() removed in w5100.cpp)
#include "EthWebServer.h"
#include "web_adapter.h"
#define HAS_W5500

// EthernetClient for TCP connections over W5500
EthernetClient eth_tcp_client;

// W5500 link state — checked via Ethernet.linkStatus() / hardwareStatus()
static EthernetHardwareStatus w5500_hw_status = EthernetNoHardware;
static EthernetLinkStatus w5500_link_status = Unknown;

void onW5500Event(arduino_event_id_t event)
{
    // Not used with Arduino Ethernet library — it's not event-driven
    // Link status is polled via Ethernet.linkStatus() in eth_loop()
}
// NOTE: #endif for USE_W5500 is at END of file — Ethernet obj must be visible
// in all HAS_W5500 sections below

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
        // ── W5500 via Arduino Ethernet library (FSPI bus, same as v2.8.0!) ──
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

            LOG_ILN("[LAN] Creating W5500 Ethernet init task...");
            xTaskCreate(
                    [](void *param) {
                        // Step 1: RST pulse
                        if (cfg.pin_eth_rst >= 0)
                        {
                            LOG_I("[LAN-TASK] RST pin=%d → LOW\n", cfg.pin_eth_rst);
                            pinMode(cfg.pin_eth_rst, OUTPUT);
                            digitalWrite(cfg.pin_eth_rst, LOW);
                            vTaskDelay(pdMS_TO_TICKS(10));
                            digitalWrite(cfg.pin_eth_rst, HIGH);
                            vTaskDelay(pdMS_TO_TICKS(150));
                            LOG_ILN("[LAN-TASK] RST pulse done → HIGH");
                        }
                        else
                        {
                            LOG_ILN("[LAN-TASK] WARNING: RST pin not configured (-1)");
                        }

                        // Step 2: Initialize SPI bus with W5500 pins on FSPI
                        // CRITICAL: SPI.end() first to release any prior init (WiFi/SD/etc may
                        // have called SPI.begin() with different pins). Without end(), the
                        // second begin() silently returns and leaves wrong pins configured!
                        LOG_I("[LAN-TASK] SPI.end() — releasing any prior SPI init\n");
                        SPI.end();
                        LOG_I("[LAN-TASK] SPI.begin(sclk=%d, miso=%d, mosi=%d, cs=%d)\n",
                              cfg.pin_eth_sclk, cfg.pin_eth_miso, cfg.pin_eth_mosi, cfg.pin_eth_cs);
                        SPI.begin(cfg.pin_eth_sclk, cfg.pin_eth_miso, cfg.pin_eth_mosi, cfg.pin_eth_cs);

                        // Step 3: Set CS pin and initialize Ethernet
                        LOG_I("[LAN-TASK] Ethernet.init(cs=%d)\n", cfg.pin_eth_cs);
                        Ethernet.init(cfg.pin_eth_cs);

                        // Step 4: MAC address (use default from W5500 chip, or set custom)
                        static byte eth_mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};

                        // Step 5: Start Ethernet
                        if (cfg.lan_dhcp)
                        {
                            LOG_ILN("[LAN-TASK] Ethernet.begin(mac) — DHCP...");
                            int ret = Ethernet.begin(eth_mac, 30000, 4000);
                            LOG_I("[LAN-TASK] Ethernet.begin() = %d\n", ret);
                            if (ret == 0)
                            {
                                LOG_ELN("[LAN-TASK] ✗ DHCP FAILED — no IP acquired");
                                w5500_task_ok = false;
                                w5500_task_done = true;
                                vTaskDelete(NULL);
                                return;
                            }
                            LOG_I("[LAN-TASK] ✓ DHCP OK! IP: %s\n", Ethernet.localIP().toString().c_str());
                            w5500_task_ok = true;
                            lan_connected = true;
                        }
                        else
                        {
                            IPAddress ip, gw, mask, dns;
                            ip.fromString(cfg.lan_ip);
                            gw.fromString(cfg.lan_gw);
                            mask.fromString(cfg.lan_mask);
                            dns.fromString(cfg.lan_dns);
                            Ethernet.begin(eth_mac, ip, dns, gw, mask);
                            LOG_I("[LAN-TASK] ✓ Static IP: %s\n", Ethernet.localIP().toString().c_str());
                            w5500_task_ok = true;
                            lan_connected = true;
                        }

                        w5500_task_done = true;
                        w5500_hw_status = Ethernet.hardwareStatus();
                        w5500_link_status = Ethernet.linkStatus();
                        LOG_I("[LAN-TASK] hw=%s link=%s IP=%s — task done\n",
                              w5500_hw_status == EthernetW5500 ? "W5500" :
                              w5500_hw_status == EthernetW5200 ? "W5200" :
                              w5500_hw_status == EthernetW5100 ? "W5100" : "NO_HW",
                              w5500_link_status == LinkON ? "ON" :
                              w5500_link_status == LinkOFF ? "OFF" : "UNKNOWN",
                              Ethernet.localIP().toString().c_str());
                        vTaskDelete(NULL); // self-delete
                    },
                    "w5500_init",
                    8192,
                    NULL,
                    1,
                    NULL);
            task_created = true;
        }
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
        w5500_hw_status = Ethernet.hardwareStatus();
        w5500_link_status = Ethernet.linkStatus();
        LOG_I("[LAN] W5500 Ethernet init complete: hw=%s link=%s ip=%s connected=%s\n",
              w5500_hw_status == EthernetW5500 ? "W5500" : "NO_HW",
              w5500_link_status == LinkON ? "ON" : "OFF",
              Ethernet.localIP().toString().c_str(),
              lan_connected ? "true" : "false");
#endif
    }

    // ── W5500 link monitoring ──
    static uint32_t last_check = 0;
    if (millis() - last_check > 3000)
    {
        if (cfg.lan_type == 0 && lan_started)
        {
#ifdef HAS_W5500
            w5500_link_status = Ethernet.linkStatus();
            w5500_hw_status = Ethernet.hardwareStatus();
            bool link = (w5500_link_status == LinkON);
            // Periodic debug
            {
                static uint32_t last_dbg = 0;
                if (millis() - last_dbg > 10000)
                {
                    LOG_I("[LAN] monitor: hw=%s link=%s IP=%s connected=%s\n",
                          w5500_hw_status == EthernetW5500 ? "W5500" :
                          w5500_hw_status == EthernetW5200 ? "W5200" :
                          w5500_hw_status == EthernetW5100 ? "W5100" : "NO_HW",
                          w5500_link_status == LinkON ? "ON" :
                          w5500_link_status == LinkOFF ? "OFF" : "UNKNOWN",
                          Ethernet.localIP().toString().c_str(),
                          lan_connected ? "Y" : "N");
                    last_dbg = millis();
                }
            }
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

            // ── W5500 auto-recovery: reinit chip if hardware lost or link down >60s ──
            {
                static bool w5500_lost = false;
                static uint32_t link_down_since = 0;

                if (w5500_hw_status == EthernetNoHardware)
                {
                    // Chip not responding — schedule full reinit
                    if (!w5500_lost)
                    {
                        LOG_ELN("[LAN] ⚠ W5500 hardware lost! Scheduling full reinit...");
                        w5500_lost = true;
                        link_down_since = millis();
                    }
                }
                else if (!link)
                {
                    // Link down — track duration
                    if (link_down_since == 0)
                        link_down_since = millis();
                    // If link down > 60s, schedule reinit
                    if (millis() - link_down_since > 60000 && !w5500_lost)
                    {
                        LOG_I("[LAN] W5500 link down for %lus — scheduling reinit\n",
                              (unsigned long)((millis() - link_down_since) / 1000));
                        w5500_lost = true;
                    }
                }
                else
                {
                    // Link ON, hw OK — clear recovery state
                    w5500_lost = false;
                    link_down_since = 0;
                }

                if (w5500_lost)
                {
                    static uint32_t last_reinit = 0;
                    static int reinit_count = 0;
                    const int MAX_REINITS = 20;  // Allow many retries — hardware may recover
                    const unsigned long REINIT_INTERVAL = 30000;  // 30s base
                    if (reinit_count < MAX_REINITS && millis() - last_reinit > REINIT_INTERVAL)
                    {
                        last_reinit = millis();
                        reinit_count++;
                        LOG_I("[LAN] Initiating W5500 full reinit (attempt %d/%d)...\n", reinit_count, MAX_REINITS);
                        w5500_reinit();
                        w5500_lost = false;
                        link_down_since = 0;
                    }
                    else if (reinit_count >= MAX_REINITS && !w5500_reinit_giveup)
                    {
                        w5500_reinit_giveup = true;
                        LOG_I("[LAN] W5500 reinit limit reached (%d). Will retry after 5 min cooldown.\n", MAX_REINITS);
                    }
                    else if (w5500_reinit_giveup && millis() - last_reinit > 300000)
                    {
                        // After 5 min cooldown, reset counters and try again
                        w5500_reinit_giveup = false;
                        reinit_count = 0;
                        last_reinit = 0;
                        LOG_I("[LAN] W5500 reinit cooldown expired — will retry.\n");
                    }
                }
            }
#endif
        }
        // LAN8720 and IP101 events handled by onEthEvent callback
        last_check = millis();
    }

    // ── W5500 DHCP retry every 30s ──
    if (cfg.lan_type == 0 && lan_started && !lan_connected)
    {
#ifdef HAS_W5500
        static uint32_t last_dhcp_retry = 0;
        if (millis() - last_dhcp_retry > 30000)
        {
            EthernetLinkStatus ls = Ethernet.linkStatus();
            if (ls == LinkON && Ethernet.localIP() == IPAddress(0, 0, 0, 0))
            {
                LOG_I("[LAN] W5500 retrying DHCP — link=ON IP=0.0.0.0\n");
                static volatile bool retry_task_running = false;
                if (!retry_task_running)
                {
                    retry_task_running = true;
                    xTaskCreate(
                            [](void *param) {
                                static byte eth_mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
                                int ret = Ethernet.begin(eth_mac, 15000, 4000);
                                LOG_I("[LAN-RETRY] DHCP result=%d hw=%s link=%s IP=%s\n",
                                      ret,
                                      Ethernet.hardwareStatus() == EthernetW5500 ? "W5500" : "NO_HW",
                                      Ethernet.linkStatus() == LinkON ? "ON" : "OFF",
                                      Ethernet.localIP().toString().c_str());
                                if (ret == 0)
                                {
                                    LOG_ELN("[LAN-RETRY] DHCP FAILED");
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

// ── W5500 Full Reinit (called from eth_loop when chip lost or link down >60s) ──
// Launches a FreeRTOS task to avoid blocking the main loop.
void w5500_reinit()
{
#ifdef HAS_W5500
    static volatile bool reinit_task_running = false;
    if (reinit_task_running)
        return;

    reinit_task_running = true;
    lan_connected = false;

    xTaskCreate(
            [](void *param) {
                LOG_ILN("[LAN-REINIT] Task started — full W5500 reinit");

                // Step 1: RST pulse
                if (cfg.pin_eth_rst >= 0)
                {
                    LOG_I("[LAN-REINIT] RST pin=%d → LOW\n", cfg.pin_eth_rst);
                    pinMode(cfg.pin_eth_rst, OUTPUT);
                    digitalWrite(cfg.pin_eth_rst, LOW);
                    vTaskDelay(pdMS_TO_TICKS(50));
                    digitalWrite(cfg.pin_eth_rst, HIGH);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    LOG_ILN("[LAN-REINIT] RST pulse done → HIGH");
                }

                // Step 2: Re-init SPI and Ethernet
                LOG_ILN("[LAN-REINIT] Re-initializing W5500 via Arduino Ethernet lib (FSPI)...");
                SPI.end(); // Release any prior SPI config
                SPI.begin(cfg.pin_eth_sclk, cfg.pin_eth_miso, cfg.pin_eth_mosi, cfg.pin_eth_cs);
                Ethernet.init(cfg.pin_eth_cs);
                vTaskDelay(pdMS_TO_TICKS(100));

                // Step 3: Check hardware
                EthernetHardwareStatus hw = Ethernet.hardwareStatus();
                if (hw == EthernetNoHardware)
                {
                    LOG_ELN("[LAN-REINIT] W5500 not detected after reinit. Aborting.");
                    reinit_task_running = false;
                    vTaskDelete(NULL);
                }
                LOG_I("[LAN-REINIT] W5500 hardware OK (hw=%d)\n", hw);

                // Step 4: DHCP or Static IP
                static byte eth_mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
                bool ok = false;
                if (cfg.lan_dhcp)
                {
                    LOG_ILN("[LAN-REINIT] Starting DHCP (15s timeout)...");
                    int ret = Ethernet.begin(eth_mac, 15000, 4000);
                    if (ret == 0)
                    {
                        LOG_ELN("[LAN-REINIT] DHCP FAILED — no IP acquired");
                    }
                    else
                    {
                        LOG_I("[LAN-REINIT] ✓ DHCP OK! IP: %s\n", Ethernet.localIP().toString().c_str());
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
                    LOG_I("[LAN-REINIT] ✓ Static IP: %s\n", Ethernet.localIP().toString().c_str());
                }

                if (ok)
                {
                    lan_connected = true;
                    LOG_I("[LAN-REINIT] ✓ W5500 recovered! IP: %s link=%s\n",
                          Ethernet.localIP().toString().c_str(),
                          Ethernet.linkStatus() == LinkON ? "ON" : "OFF");
                }
                else
                {
                    LOG_ELN("[LAN-REINIT] ✗ W5500 reinit failed — WiFi fallback active");
                }

                reinit_task_running = false;
                vTaskDelete(NULL);
            },
            "w5500_reinit",
            4096,
            NULL,
            1,
            NULL);
#endif
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

// ─── W5500 Hard Reset + System Restart ─────────────────────────
// Call this INSTEAD of ESP.restart() to ensure W5500 chip is properly
// reset before reboot. Without this, the W5500 may be in a bad state
// after soft restart and fail to get DHCP on next boot.
void eth_hard_reset_and_restart()
{
    LOG_ILN("[LAN] W5500 hard reset before restart...");

    if (cfg.lan_type == 0 && cfg.pin_eth_rst >= 0)
    {
        // Hard reset W5500 chip: RST LOW for 100ms, then leave LOW
        // On next boot, eth_init() will drive RST HIGH and re-init
        pinMode(cfg.pin_eth_rst, OUTPUT);
        digitalWrite(cfg.pin_eth_rst, LOW);
        delay(100);
        // Keep RST LOW — don't drive HIGH, let boot code re-init
    }

    // Use esp_restart() (not ESP.restart) for cleaner peripheral teardown
    // Then trigger a real hardware reset via RTC WDT
    esp_restart();
}

#endif // USE_W5500 — end of W5500 conditional block (started at top of file)
