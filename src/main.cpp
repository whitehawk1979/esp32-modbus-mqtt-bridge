/**
 * main.cpp — Universal Modbus-MQTT Bridge v2.0
 *
 * Waveshare ESP32-S3-ETH V1.0 | ESP32-S3 | ESPHome-independent
 *
 * Network: LAN primary, WiFi fallback, auto-switch back
 * Modbus: Unlimited modules, auto-detect, 6DI+6R per module
 * HA: Auto-discovery via MQTT
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include "modbus_mqtt_ha_bridge.h"
#include "nibe_profile.h"
#include "kincony_profile.h"
#include "sabiana_profile.h"
#ifdef USE_STORAGE
#include "ota_storage.h"
#endif

// ─── Global State (heap-allocated for unlimited modules) ───────
Slave_Module *modules = nullptr;
uint16_t module_count = 0;
uint16_t modules_capacity = 0;
bool scanning_done = false;

// ─── Register Config State ──────────────────────────────────────
RegisterConfig registers[MAX_REGISTERS];
uint8_t register_count = 0;

// ─── Network State ─────────────────────────────────────────────
bool net_connected = false;
String active_ip = "0.0.0.0";
static bool mqtt_needs_reinit = false;

// ─── Timings ───────────────────────────────────────────────────
static uint32_t last_poll_ms = 0;
static uint32_t last_mqtt_ms = 0;
static uint32_t last_scan_ms = 0;
static uint32_t last_stats_ms = 0;
static uint32_t last_relay_sync = 0;

// ─── Dynamic Module Allocation ─────────────────────────────────
static bool alloc_module(Slave_Module **slot)
{
    if (module_count >= modules_capacity)
    {
        uint16_t new_cap = modules_capacity == 0 ? 16 : modules_capacity * 2;
        if (new_cap > MODBUS_MAX_SLAVES)
            new_cap = MODBUS_MAX_SLAVES;
        Slave_Module *new_arr = reinterpret_cast<Slave_Module *>(realloc(modules, new_cap * sizeof(Slave_Module)));
        if (!new_arr)
        {
            LOG_ELN("[ALLOC] Out of memory!");
            return false;
        }
        modules = new_arr;
        modules_capacity = new_cap;
    }
    *slot = &modules[module_count];
    return true;
}

// ─── LAN-primary network management ────────────────────────────
//  Logic: LAN is always primary when available.
//  1. LAN up   → use LAN, disconnect WiFi STA (save power)
//  2. LAN down → auto-switch to WiFi (fallback)
//  3. LAN back → auto-switch back to LAN, keep WiFi as standby
static void update_network()
{
    bool lan_ok = eth_is_connected();
    bool wifi_ok = (WiFi.status() == WL_CONNECTED);

    static uint32_t dbg_timer = 0;
    if (millis() - dbg_timer > 5000)
    {
        LOG_D("[NET-DBG] lan_en=%d lan_ok=%d wifi_ok=%d active_if=%d net=%d\n",
              cfg.lan_enabled,
              lan_ok,
              wifi_ok,
              cfg.active_if,
              net_connected);
        dbg_timer = millis();
    }

    NetInterface prev_if __attribute__((unused)) = cfg.active_if;

    if (cfg.lan_enabled && lan_ok)
    {
        // ── LAN is UP → always prefer LAN ─────────────────────
        if (cfg.active_if != NET_IF_LAN)
        {
            cfg.active_if = NET_IF_LAN;
            active_ip = eth_get_ip();
            net_connected = true;
            mqtt_needs_reinit = true;
            LOG_I("[NET] ⚡ Switched to LAN: %s\n", active_ip.c_str());
        }
        // Dual-stack: keep WiFi STA connected for fallback readiness + MQTT
    }
    else if (wifi_ok)
    {
        // ── LAN down or disabled → WiFi fallback ──────────────
        if (cfg.active_if != NET_IF_WIFI || !net_connected)
        {
            bool switched = (cfg.active_if != NET_IF_WIFI);
            cfg.active_if = NET_IF_WIFI;
            active_ip = WiFi.localIP().toString();
            net_connected = true;
            if (switched)
            {
                mqtt_needs_reinit = true;
                LOG_I("[NET] 📶 Switched to WiFi fallback: %s\n", active_ip.c_str());
            }
        }
    }
    else
    {
        // ── Both down ─────────────────────────────────────────
        if (net_connected)
        {
            LOG_ELN("[NET] ✗ All interfaces down!");
            net_connected = false;
            cfg.active_if = NET_IF_NONE;
            active_ip = "0.0.0.0";
        }
    }

    // Re-init MQTT when interface changes
    if (mqtt_needs_reinit && net_connected)
    {
        mqtt_needs_reinit = false;
        mqtt_init();

        // Re-publish discovery if needed
        if (scanning_done && cfg.ha_discovery)
        {
            for (uint16_t i = 0; i < module_count; i++)
            {
                if (!modules[i].discovered)
                {
                    mqtt_publish_discovery(&modules[i]);
                    mqtt_subscribe_commands(&modules[i]);
                    modules[i].discovered = true;
                }
            }
        }
    }
}

// ─── WiFi Connect (fallback + AP always on) ────────────────────
static void wifi_connect()
{
    // AP always starts so device is reachable even without router
    String ap_name = cfg.ap_name[0] ? String(cfg.ap_name)
                                    : String(cfg.hostname) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);

    // Set mode FIRST, then start AP (order matters on ESP32-S3!)
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_name.c_str(), cfg.ap_pass);
    LOG_I("[WiFi] AP started: %s, IP: %s\n", ap_name.c_str(), WiFi.softAPIP().toString().c_str());

    if (strlen(cfg.wifi_ssid) == 0)
    {
        LOG_ILN("[WiFi] No SSID configured — AP only mode");
        return;
    }

    if (cfg.wifi_mode == WIFICFG_APSTA)
    {
        // Switch to AP+STA (keep existing AP, add STA)
        WiFi.mode(WIFI_AP_STA);
        // Re-ensure AP is running after mode change
        WiFi.softAP(ap_name.c_str(), cfg.ap_pass);
        LOG_I("[WiFi] Mode: AP+STA (always reachable via AP)\n");
    }
    else
    {
        WiFi.mode(WIFI_STA);
        WiFi.softAPdisconnect(true);
        LOG_I("[WiFi] Mode: STA only\n");
    }
    // WiFi power save: disabled during boot for stability.
    // Will be enabled later in wifi_maintain() after stable WiFi connection.
    WiFi.setSleep(false);

    if (!cfg.wifi_dhcp && strlen(cfg.wifi_ip) > 0)
    {
        IPAddress ip, gw, mask, dns_addr;
        ip.fromString(cfg.wifi_ip);
        gw.fromString(cfg.wifi_gw);
        mask.fromString(cfg.wifi_mask);
        dns_addr.fromString(cfg.wifi_dns);
        WiFi.config(ip, gw, dns_addr, mask);
        LOG_I(" (static: %s)", cfg.wifi_ip);
    }

    LOG_I("[WiFi] Connecting STA to %s", cfg.wifi_ssid);
    WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
    // Do NOT block — STA connects in background, web server starts immediately
    LOG_ILN(" (connecting in background)");
}

// ─── WiFi auto-reconnect (when LAN is down) ────────────────────
static void wifi_maintain()
{
    // Ensure AP is running if AP+STA mode
    if (cfg.wifi_mode == WIFICFG_APSTA)
    {
        if (WiFi.getMode() != WIFI_AP_STA && WiFi.getMode() != WIFI_AP)
        {
            WiFi.mode(WIFI_AP_STA);
            String ap_name = cfg.ap_name[0] ? String(cfg.ap_name)
                                            : String(cfg.hostname) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
            WiFi.softAP(ap_name.c_str(), cfg.ap_pass);
        }
    }

    if (strlen(cfg.wifi_ssid) == 0)
        return;

    // Always try to keep WiFi STA connected for dual-stack (both WiFi + LAN)
    // Even when LAN is up, WiFi provides MQTT, AP access, and fallback
    if (WiFi.status() != WL_CONNECTED)
    {
        static uint32_t last_reconnect = 0;
        if (millis() - last_reconnect > 5000)
        {
            LOG_ILN("[WiFi] Reconnecting (LAN fallback)...");
            WiFi.reconnect();
            last_reconnect = millis();
        }
    }
    else
    {
        // Enable WiFi power save after stable connection (~30-40mA saving)
        // Only enable once after boot + successful WiFi connect
        static bool wifi_ps_enabled = false;
        if (!wifi_ps_enabled && millis() > 30000) // 30s after boot
        {
            WiFi.setSleep(true); // WIFI_PS_MIN_MODEM
            wifi_ps_enabled = true;
            LOG_ILN("[WiFi] Power save enabled (MIN_MODEM)");
        }
    }
}

// ─── Status LED ─────────────────────────────────────────────────
static void update_status_led()
{
    if (cfg.pin_status_led < 0)
        return; // LED disabled
    static uint32_t last_blink = 0;
    static bool led_state = false;
    uint32_t interval;

    if (!net_connected)
        interval = 200; // Fast blink: no network
    else if (!scanning_done)
        interval = 500; // Medium: scanning
    else if (module_count == 0)
        interval = 1500; // Slow: no modules
    else
    {
        // Solid on with pulse per active interface
        bool pulse = (millis() % 3000) < 100;
        digitalWrite(cfg.pin_status_led, pulse ? !led_state : HIGH);
        return;
    }

    if (millis() - last_blink > interval)
    {
        led_state = !led_state;
        digitalWrite(cfg.pin_status_led, led_state);
        last_blink = millis();
    }
}

// ─── Module Auto-Discovery ─────────────────────────────────────
// ─── Non-blocking Modbus scan (one address per call) ──────────
uint16_t scan_addr = 0;   // current scan address
ScanResult scan_results[MAX_SCAN_RESULTS] = {};
uint8_t scan_result_count = 0;
bool scan_active = false; // scanning in progress

// ─── Add module from saved list (fast boot) ──────────────────
void module_add_from_scan(uint8_t addr, uint8_t model_id)
{
    Slave_Module *mod = nullptr;
    if (!alloc_module(&mod))
        return;
    memset(mod, 0, sizeof(Slave_Module));
    memset(mod->di_relay_map, 0xFF, HA_V2_DI_COUNT); // 255 = no mapping (legacy)
    // Initialize EdgeEvent map: no relay, no actions
    for (uint8_t d = 0; d < HA_V2_DI_COUNT; d++)
    {
        mod->di_edge_map[d].relay = 0xFF;
        mod->di_edge_map[d].rising_action = DI_EDGE_NONE;
        mod->di_edge_map[d].falling_action = DI_EDGE_NONE;
    }
    mod->slave_addr = addr;
    mod->online = true;
    mod->last_seen_ms = millis();
    mod->model.model_id = model_id;
    // Fill model name from known ID
    if (model_id == 1)
        strlcpy(mod->model.model_name, "KC868-HA V1", sizeof(mod->model.model_name));
    else if (model_id == 2)
        strlcpy(mod->model.model_name, "KC868-HA V2", sizeof(mod->model.model_name));
    else
        snprintf(mod->model.model_name, sizeof(mod->model.model_name), "Unknown (ID=%u)", model_id);
    mod->profile = 0;
    mod->active = true;
    mod->discovered = false;
    click_init(mod->inputs, HA_V2_DI_COUNT);
    LOG_I("[BOOT] Loaded saved module: S%d %s\n", addr, mod->model.model_name);
}

// ─── Add virtual module (testing) ─────────────────────────────
void module_add_virtual()
{
    Slave_Module *vmod = nullptr;
    if (!alloc_module(&vmod))
        return;
    memset(vmod, 0, sizeof(Slave_Module));
    memset(vmod->di_relay_map, 0xFF, HA_V2_DI_COUNT); // 255 = no mapping (legacy)
    // Initialize EdgeEvent map: no relay, no actions
    for (uint8_t d = 0; d < HA_V2_DI_COUNT; d++)
    {
        vmod->di_edge_map[d].relay = 0xFF;
        vmod->di_edge_map[d].rising_action = DI_EDGE_NONE;
        vmod->di_edge_map[d].falling_action = DI_EDGE_NONE;
    }
    vmod->slave_addr = 200;
    vmod->online = true;
    vmod->last_seen_ms = millis();
    vmod->model.model_id = 2;      // KC868-HA2
    vmod->model.firmware_ver = 99; // Virtual
    vmod->model.serial_number = 99999;
    strlcpy(vmod->model.model_name, "KC868-HA V2 (SIM)", sizeof(vmod->model.model_name));
    vmod->profile = 0;
    vmod->active = true;
    vmod->discovered = false;
    vmod->is_virtual = true;
    vmod->relays[0].state = true;
    vmod->relays[0].published = false;
    vmod->inputs[0].current = true;
    click_init(vmod->inputs, HA_V2_DI_COUNT);
    module_count++;
    LOG_ILN("[BOOT] Virtual module inserted at addr 200");
}

void scan_modbus_start()
{
    scan_addr = cfg.mb_scan_start;
    scan_active = true;
    module_count = 0;
    modbus_set_timeout(200); // Fast timeout during scan (200ms vs 2000ms)
    scan_result_count = 0;
    memset(scan_results, 0, sizeof(scan_results));
    LOG_I("[SCAN] Starting scan %d-%d (profile=%d, timeout=200ms)...\n",
          cfg.mb_scan_start,
          cfg.mb_scan_end,
          cfg.mb_profile);
}

static bool scan_modbus_next()
{
    if (!scan_active)
        return true; // not scanning
    if (scan_addr > cfg.mb_scan_end)
    {
        scan_active = false;
        scanning_done = true;
        modbus_set_timeout(2000); // Restore normal timeout after scan

        // Insert virtual module if enabled (address 200)
        if (cfg.virtual_module)
        {
            module_add_virtual();
        }

        LOG_I("[SCAN] Done: %d module(s) found\n", module_count);
        // Auto-save module list for fast boot on next restart
        if (module_count > 0)
        {
            config_save_module_list();
        }
        // Publish discovery for found modules
        if (net_connected && cfg.ha_discovery)
        {
            for (uint16_t i = 0; i < module_count; i++)
            {
                mqtt_publish_discovery(&modules[i]);
                mqtt_subscribe_commands(&modules[i]);
                modules[i].discovered = true;
            }
        }
        return true; // scan complete
    }

    // Probe one address
    HA_Model model;
    if (modbus_read_identification(scan_addr, model))
    {
        Slave_Module *mod = nullptr;
        if (alloc_module(&mod))
        {
            memset(mod, 0, sizeof(Slave_Module));
            mod->slave_addr = scan_addr;
            mod->online = true;
            mod->last_seen_ms = millis();
            mod->model = model;
            mod->profile = 0; // Use global profile
            mod->active = true;
            mod->discovered = false;

            click_init(mod->inputs, HA_V2_DI_COUNT);

            LOG_I("[SCAN] Addr %d: %s (6R, 6DI, FW=%d, SN=%u)\n",
                  scan_addr,
                  model.model_name,
                  model.firmware_ver,
                  model.serial_number);

            module_count++;

            // Also add to scan result buffer
            if (scan_result_count < MAX_SCAN_RESULTS) {
                ScanResult &sr = scan_results[scan_result_count++];
                sr.addr = scan_addr;
                sr.model_id = model.model_id;
                sr.firmware_ver = model.firmware_ver;
                strncpy(sr.model_name, model.model_name, sizeof(sr.model_name) - 1);
                sr.identified = true;
            }
        }
    }
    scan_addr++;
    return false; // scan not yet complete
}

// ─── Execute DI→Relay Edge Action ───────────────────────────────
// Applies the specified edge action (ON/OFF/TOGGLE) to the relay
// mapped by di_edge_map[di_idx]. Called on rising and falling DI edges.
// For physical modules: writes relay via Modbus. For virtual: MQTT only.
void di_execute_edge_action(Slave_Module *mod, uint8_t di_idx, uint8_t action, bool is_virtual)
{
    if (action == DI_EDGE_NONE)
        return;

    DI_EdgeAction &ea = mod->di_edge_map[di_idx];
    if (ea.relay == 0xFF || ea.relay >= HA_V2_RELAY_COUNT)
        return;

    uint8_t r = ea.relay;
    bool new_state = mod->relays[r].state;

    switch (action)
    {
    case DI_EDGE_ON:
        new_state = true;
        break;
    case DI_EDGE_OFF:
        new_state = false;
        break;
    case DI_EDGE_TOG:
        new_state = !mod->relays[r].state;
        break;
    default:
        return; // DI_EDGE_NONE or invalid
    }

    if (new_state == mod->relays[r].state && action != DI_EDGE_TOG)
        return; // Already in target state (skip redundant ON→ON or OFF→OFF)

    mod->relays[r].state = new_state;
    mod->relays[r].published = false;

    if (!is_virtual)
    {
        modbus_write_coil(mod->slave_addr, r, new_state);
    }

    LOG_I("[DI→RELAY] S%d DI%d→R%d %s: %s (%s)\n",
          mod->slave_addr, di_idx + 1, r + 1,
          di_edge_action_str(action),
          new_state ? "ON" : "OFF",
          is_virtual ? "virtual" : "hw");
}

const char *di_edge_action_str(uint8_t action)
{
    switch (action)
    {
    case DI_EDGE_NONE: return "NONE";
    case DI_EDGE_ON:   return "ON";
    case DI_EDGE_OFF:  return "OFF";
    case DI_EDGE_TOG:  return "TOG";
    default:           return "?";
    }
}

// ─── Polling ─────────────────────────────────────────────────────
static void poll_all_modules()
{
    for (uint16_t i = 0; i < module_count; i++)
    {
        Slave_Module &mod = modules[i];

        // Virtual module: skip Modbus, keep current state
        if (mod.is_virtual)
        {
            mqtt_set_click_module(&mod);
            // Publish initial states if not yet published
            for (uint8_t r = 0; r < HA_V2_RELAY_COUNT; r++)
            {
                if (!mod.relays[r].published)
                {
                    mqtt_publish_relay_state(&mod, r);
                    mod.relays[r].published = true;
                }
            }
            for (uint8_t d = 0; d < HA_V2_DI_COUNT; d++)
            {
                if (!mod.inputs[d].published)
                {
                    mqtt_publish_di_state(&mod, d, mod.inputs[d].current);
                    mod.inputs[d].published = true;
                }
            }
            // Publish online status
            mqtt_publish_module_online(&mod, true);
            mqtt_set_click_module(nullptr);
            continue;
        }

        // Set current module context for click callback
        mqtt_set_click_module(&mod);

        // ── Backoff: skip offline/failing modules ──
        // Exponential backoff based on fail_count:
        //   0: no delay (online), 1-5: 2×poll, 6-15: 4×, 16+: 8×
        if (mod.fail_count > 0)
        {
            uint32_t backoff_interval = cfg.mb_poll_ms;
            if (mod.fail_count <= 5)
                backoff_interval *= 2;
            else if (mod.fail_count <= 15)
                backoff_interval *= 4;
            else
                backoff_interval *= 8;

            if (millis() - mod.last_seen_ms < backoff_interval)
            {
                mqtt_set_click_module(nullptr);
                continue; // Skip — backoff period not elapsed
            }
        }
        // NOTE: mod.last_seen_ms is set to millis() on each poll attempt (success OR fail)
        // by the fail_count handler below, so backoff counts from last attempt.

        uint8_t profile = effective_profile(&mod);

        // ── Adaptive Modbus timeout ──
        modbus_set_timeout_for_module(mod.online);

        // ── KC868-HA V2: try combined DI+DO read (FC03 @ 0x00C8) ──
        bool di_ok = false;
        bool relay_ok = false;

        if (profile != MB_PROFILE_GENERIC)
        {
            // Combined read: 1 FC03 transaction instead of FC01+FC02
            bool di_states[HA_V2_DI_COUNT] = {0};
            bool coil_states[HA_V2_RELAY_COUNT] = {0};

            if (modbus_read_di_do_combined(mod.slave_addr, HA_V2_DI_COUNT, HA_V2_RELAY_COUNT, di_states, coil_states))
            {
                mod.online = true;
                mod.last_seen_ms = millis();
                mod.fail_count = 0;
                di_ok = true;
                relay_ok = true;

                // Process DI states (click detection + publish changes)
                for (uint8_t d = 0; d < HA_V2_DI_COUNT; d++)
                {
                    bool prev_state = mod.inputs[d].current;
                    click_update(&mod.inputs[d], d, di_states[d]);
                    ClickType ct = click_get_event(&mod.inputs[d], d);
                    if (ct != CLICK_NONE)
                    {
                        mqtt_publish_click_event(&mod, d, ct);
                        LOG_D("[CLICK] S%d DI%d: %s\n", mod.slave_addr, d + 1, click_type_str(ct));
                    }
                    if (di_states[d] != prev_state)
                    {
                        // ── DI→Relay EdgeEvent mapping (v2.8+) ──
                        // Rising edge (DI went OFF→ON = button pressed)
                        if (di_states[d] && !prev_state)
                        {
                            di_execute_edge_action(&mod, d, mod.di_edge_map[d].rising_action, false);
                        }
                        // Falling edge (DI went ON→OFF = button released)
                        if (!di_states[d] && prev_state)
                        {
                            di_execute_edge_action(&mod, d, mod.di_edge_map[d].falling_action, false);
                        }
                        // Legacy di_relay_map: toggle on any change (backward compat)
                        if (mod.di_edge_map[d].relay == 0xFF)
                        {
                            uint8_t mapped = mod.di_relay_map[d];
                            if (mapped != 0xFF && mapped < HA_V2_RELAY_COUNT)
                            {
                                bool new_relay = !mod.relays[mapped].state;
                                mod.relays[mapped].state = new_relay;
                                mod.relays[mapped].published = false;
                                modbus_write_coil(mod.slave_addr, mapped, new_relay);
                                LOG_I("[DI→RELAY] S%d DI%d→R%d: %s (legacy)\n",
                                      mod.slave_addr, d + 1, mapped + 1,
                                      new_relay ? "ON" : "OFF");
                            }
                        }
                        mqtt_publish_di_state(&mod, d, di_states[d]);
                    }
                }

                // Process relay state changes
                for (uint8_t r = 0; r < HA_V2_RELAY_COUNT; r++)
                {
                    if (mod.relays[r].state != coil_states[r])
                    {
                        mod.relays[r].state = coil_states[r];
                        mqtt_publish_relay_state(&mod, r);
                    }
                }
                // Mark relay sync done — no need for separate FC01 read
                last_relay_sync = millis();
            }
        }

        // ── Fallback: separate FC01+FC02 reads ──
        // Used for: generic profile, or when combined read fails
        if (!di_ok)
        {
            bool di_states[HA_V2_DI_COUNT] = {0};
            if (modbus_read_discrete_inputs(mod.slave_addr, HA_V2_DI_COUNT, di_states))
            {
                mod.online = true;
                mod.last_seen_ms = millis();
                mod.fail_count = 0;

                for (uint8_t d = 0; d < HA_V2_DI_COUNT; d++)
                {
                    bool prev_state = mod.inputs[d].current;
                    click_update(&mod.inputs[d], d, di_states[d]);
                    ClickType ct = click_get_event(&mod.inputs[d], d);
                    if (ct != CLICK_NONE)
                    {
                        mqtt_publish_click_event(&mod, d, ct);
                        LOG_D("[CLICK] S%d DI%d: %s\n", mod.slave_addr, d + 1, click_type_str(ct));
                    }
                    if (di_states[d] != prev_state)
                    {
                        // ── DI→Relay EdgeEvent mapping (v2.8+) ──
                        // Rising edge (DI went OFF→ON = button pressed)
                        if (di_states[d] && !prev_state)
                        {
                            di_execute_edge_action(&mod, d, mod.di_edge_map[d].rising_action, false);
                        }
                        // Falling edge (DI went ON→OFF = button released)
                        if (!di_states[d] && prev_state)
                        {
                            di_execute_edge_action(&mod, d, mod.di_edge_map[d].falling_action, false);
                        }
                        // Legacy di_relay_map: toggle on any change (backward compat)
                        if (mod.di_edge_map[d].relay == 0xFF)
                        {
                            uint8_t mapped = mod.di_relay_map[d];
                            if (mapped != 0xFF && mapped < HA_V2_RELAY_COUNT)
                            {
                                bool new_relay = !mod.relays[mapped].state;
                                mod.relays[mapped].state = new_relay;
                                mod.relays[mapped].published = false;
                                modbus_write_coil(mod.slave_addr, mapped, new_relay);
                                LOG_I("[DI→RELAY] S%d DI%d→R%d: %s (legacy)\n",
                                      mod.slave_addr, d + 1, mapped + 1,
                                      new_relay ? "ON" : "OFF");
                            }
                        }
                        mqtt_publish_di_state(&mod, d, di_states[d]);
                    }
                }
            }
            else
            {
                mod.fail_count++;
                mod.last_seen_ms = millis(); // Update for backoff timer
                if (mod.fail_count >= 5 && mod.online)
                {
                    mod.online = false;
                    mqtt_publish_module_online(&mod, false);
                }
            }
        }

        if (!relay_ok && millis() - last_relay_sync > cfg.mb_poll_ms)
        {
            bool coil_states[HA_V2_RELAY_COUNT] = {0};
            if (modbus_read_coils(mod.slave_addr, HA_V2_RELAY_COUNT, coil_states))
            {
                for (uint8_t r = 0; r < HA_V2_RELAY_COUNT; r++)
                {
                    if (mod.relays[r].state != coil_states[r])
                    {
                        mod.relays[r].state = coil_states[r];
                        mqtt_publish_relay_state(&mod, r);
                    }
                }
            }
            last_relay_sync = millis();
        }
        delay(5);
    }

    // Clear module context after polling
    mqtt_set_click_module(nullptr);
}

// ─── Periodic Rescan for offline modules ───────────────────────
static void check_rescan()
{
    if (!scanning_done || millis() - last_scan_ms < 60000)
        return;
    last_scan_ms = millis();

    for (uint16_t i = 0; i < module_count; i++)
    {
        if (!modules[i].online)
        {
            HA_Model model;
            if (modbus_read_identification(modules[i].slave_addr, model))
            {
                modules[i].online = true;
                modules[i].last_seen_ms = millis();
                modules[i].fail_count = 0;
                mqtt_publish_module_online(&modules[i], true);
                LOG_I("[SCAN] Module %d back online\n", modules[i].slave_addr);
            }
        }
    }
}

// ─── Setup ──────────────────────────────────────────────────────
static bool setup_done = false;
static uint8_t setup_stage = 0;

void setup()
{
    disableCore0WDT();
    disableCore1WDT();

    Serial.begin(115200);
    delay(500);
    LOG_ILN("\n\n╔═════════════════════════════════════════╗");
    LOG_I("║  Modbus-MQTT Bridge v%s — ESP32-S3-ETH ║\n", FIRMWARE_VERSION);
    LOG_ILN("║  ESP32-S3 | LAN-primary + WiFi fallback   ║");
    LOG_ILN("╚═════════════════════════════════════════╝\n");

    config_init();
    config_load();
    config_load_registers();

    // ── Auto-load device profile if no registers configured ──
    if (register_count == 0)
    {
        switch (cfg.mb_profile)
        {
        case MB_PROFILE_NIBE:
            nibe_load_profile(registers, &register_count);
            config_save_registers();
            LOG_I("[PROFILE] Auto-loaded NIBE S1156-18 profile: %u registers\n", register_count);
            break;
        case MB_PROFILE_SABIANA:
            // Load Sabiana profile for each fan-coil slave (1-5)
            for (uint8_t s = 1; s <= SABIANA_SLAVE_COUNT; s++)
            {
                sabiana_load_profile(registers, &register_count, s);
            }
            config_save_registers();
            LOG_I("[PROFILE] Auto-loaded Sabiana profile: %u registers (%u slaves)\n",
                  register_count, SABIANA_SLAVE_COUNT);
            break;
        // MB_PROFILE_KC868_HA uses DI/Relay polling, not registers
        // MB_PROFILE_GENERIC: user configures manually
        default:
            LOG_I("[PROFILE] No auto-profile for mode %u\n", cfg.mb_profile);
            break;
        }
    }
    else
    {
        LOG_I("[PROFILE] %u registers already configured — skipping auto-load\n", register_count);
    }

    // ── Watchdog init (before anything else) ──
    wdt_init();

    // ── Set device hostname (mDNS, DHCP name, MQTT client ID) ──
    WiFi.setHostname(cfg.hostname);
    LOG_I("[SYS] Hostname: %s\n", cfg.hostname);

    // ── Start mDNS responder (modbusmqtt.local) ──
    if (MDNS.begin(cfg.hostname))
    {
        MDNS.addService("http", "tcp", 80);    // Web UI
        MDNS.addService("ws", "tcp", 81);      // WebSocket
        MDNS.addService("modbus", "tcp", 502); // Modbus TCP gateway
        LOG_I("[SYS] ✓ mDNS: %s.local (http:80, ws:81, modbus:502)\n", cfg.hostname);
    }
    else
    {
        LOG_ELN("[SYS] ✗ mDNS start failed!");
    }

    // ── GPIO init (uses configured pin values) ──
    if (cfg.pin_status_led >= 0)
        pinMode(cfg.pin_status_led, OUTPUT);
    if (cfg.pin_config_btn >= 0)
        pinMode(cfg.pin_config_btn, INPUT_PULLUP);
    if (cfg.pin_rs485_de >= 0)
    {
        pinMode(cfg.pin_rs485_de, OUTPUT);
        digitalWrite(cfg.pin_rs485_de, LOW);
    }

    // ── WiFi AP+STA first — so web server is reachable immediately ──
    wifi_connect(); // AP+STA mode (always starts AP)

    // ── Start web server NOW — loop() will handle requests ──
    web_server_init();
    LOG_ILN("[SETUP] ✓ Web server running on AP IP");

    // ── Start WebSocket server on port 81 ──
    ws_init();
    LOG_ILN("[SETUP] Remaining init continues in loop()...");

    enableCore0WDT();
    enableCore1WDT();
    // setup() DONE — loop() starts immediately, web_server_loop() runs!
}

// ─── Staged init: runs in loop() one step at a time ─────────────
static void do_staged_init()
{
    // Keep network alive during all init stages
    eth_loop();
    wifi_maintain();
    update_network();

    switch (setup_stage)
    {
    case 0:
        setup_stage = 1; // done, nothing more
        break;
    case 1:
        LOG_ILN("[INIT] Modbus...");
        modbus_init();
        LOG_ILN("[INIT] Modbus OK");
        setup_stage = 2;
        break;
    case 2:
        LOG_ILN("[INIT] LAN...");
        eth_init();
        cfg.active_if = NET_IF_NONE;
        update_network();
        LOG_ILN("[INIT] LAN done");
        setup_stage = 3;
        break;
    case 3:
        LOG_ILN("[INIT] MQTT...");
        mqtt_init(); // always init — mqtt_loop() will reconnect when net is available
        LOG_ILN("[INIT] TCP bridge...");
        tcp_init();
        LOG_ILN("[INIT] OTA...");
        ota_init();
        // Register click callback for MQTT publishing
        LOG_ILN("[INIT] Registering click callback...");
        click_set_callback([](uint8_t di_idx, uint8_t click_type) {
            // This callback is invoked from click_update()
            // The current module context is set via mqtt_set_click_module()
            // before calling click_update() in poll_all_modules()
            // Click events are published both here and via mqtt_publish_click_event
            // in poll_all_modules, so we don't need to double-publish.
            // The callback mechanism exists so external code can hook in.
        });
#ifdef USE_SD
        // ── SD Card init (after LAN, shares FSPI bus) ──
        if (cfg.sd_enabled)
        {
            LOG_ILN("[INIT] SD card...");
            if (sd_init(cfg.pin_sd_cs, "auto"))
                LOG_ILN("[INIT] SD OK");
            else
                LOG_ELN("[INIT] SD FAILED — card present?");
        }
#endif
#ifdef USE_STORAGE
        // ── LittleFS Flash Storage init ──
        LOG_ILN("[INIT] Flash Storage (LittleFS)...");
        if (storage_init())
        {
            LOG_ILN("[INIT] Storage OK");
            // Restore pins from /active/pins.json (overrides NVS defaults)
            storage_restore_pins();
#ifdef USE_STORAGE
            // Initialize storage-based OTA directory
            ota_storage_init();
#endif
        }
        else
            LOG_ELN("[INIT] Storage FAILED");
#endif
#ifdef USE_WS2812
        // ── WS2812B RGB LED init ──
        LOG_ILN("[INIT] WS2812B LED...");
        led_init();
        LOG_ILN("[INIT] LED OK");
#endif
        setup_stage = 4;
        break;
    case 4:
    {
        // Check for saved module list (fast boot)
        uint8_t saved_count = config_load_module_list();
        if (saved_count > 0)
        {
            // Bootstrap modules from NVRAM — skip scan
            Preferences nv;
            nv.begin(NV_NAMESPACE, true);
            for (uint8_t i = 0; i < saved_count; i++)
            {
                char akey[12], mkey[12], drkey[12], edkey[12];
                snprintf(akey, sizeof(akey), "%s%u", NV_KEY_MOD_ADDR, i);
                snprintf(mkey, sizeof(mkey), "%s%u", NV_KEY_MOD_MODEL, i);
                snprintf(drkey, sizeof(drkey), "%s%u", NV_KEY_MOD_DIRM, i);
                snprintf(edkey, sizeof(edkey), "%s%u", NV_KEY_MOD_EDGE, i);
                uint8_t addr = nv.getUChar(akey, 0);
                uint8_t model_id = nv.getUChar(mkey, 0);
                module_add_from_scan(addr, model_id);
                // Restore DI→Relay mapping from NVRAM (legacy)
                uint8_t drmap[HA_V2_DI_COUNT];
                nv.getBytes(drkey, drmap, HA_V2_DI_COUNT);
                if (modules[i].active)
                {
                    memcpy(modules[i].di_relay_map, drmap, HA_V2_DI_COUNT);
                    // Restore DI→Edge mapping from NVRAM (v2.8+)
                    uint8_t edge_blob[HA_V2_DI_COUNT * 3];
                    nv.getBytes(edkey, edge_blob, sizeof(edge_blob));
                    for (uint8_t d = 0; d < HA_V2_DI_COUNT; d++)
                    {
                        modules[i].di_edge_map[d].relay = edge_blob[d * 3 + 0];
                        modules[i].di_edge_map[d].rising_action = edge_blob[d * 3 + 1];
                        modules[i].di_edge_map[d].falling_action = edge_blob[d * 3 + 2];
                    }
                }
            }
            nv.end();
            // Add virtual module if enabled
            if (cfg.virtual_module)
            {
                module_add_virtual();
            }
            scanning_done = true;
            const char *if_name = cfg.active_if == NET_IF_LAN ? "LAN" : cfg.active_if == NET_IF_WIFI ? "WiFi" : "NONE";
            LOG_I("[INIT] ✓ Fast boot from saved list. Interface: %s, IP: %s, Modules: %d (scan skipped)\n\n",
                  if_name,
                  active_ip.c_str(),
                  module_count);
            setup_stage = 6;
        }
        else
        {
            // No saved list — start normal scan
            scan_modbus_start();
            setup_stage = 5;
        }
        break;
    }
    case 5:
        // Continue scanning one address per loop iteration
        if (scan_modbus_next())
        {
            // Scan complete
            const char *if_name = cfg.active_if == NET_IF_LAN ? "LAN" : cfg.active_if == NET_IF_WIFI ? "WiFi" : "NONE";
            LOG_I("[INIT] ✓ Ready. Interface: %s, IP: %s, Modules: %d\n\n", if_name, active_ip.c_str(), module_count);
            setup_stage = 6;
        }
        // else: still scanning, will continue next loop
        break;
    case 6:
        setup_done = true;
        break;
    }
}

// ─── Main Loop ──────────────────────────────────────────────────
void loop()
{
    // ── Feed watchdog ──────────────────────────────────────────
    wdt_loop_tick_reset();

    // ── Web server ALWAYS runs (even in AP-only mode) ────────────
    web_server_loop();

    // ── WebSocket real-time updates ────────────────────────────
    ws_loop();

    // ── ArduinoOTA (PlatformIO espota) ────────────────────────
    ota_loop();

    // ── Staged init: one step per loop iteration ──────────────
    if (!setup_done)
    {
        do_staged_init();
        delay(50); // small delay between stages
        return;    // web_server_loop() already ran above
    }

    // ── Network: LAN priority with auto-fallback ───────────────
    eth_loop();       // Check LAN link status
    wifi_maintain();  // Keep WiFi alive as fallback
    update_network(); // Auto-switch: LAN↔WiFi

    // ── MQTT & TCP (only when connected) ────────────────────
    mqtt_loop();
    tcp_loop();

    // ── Deferred discovery: publish if modules exist but not yet discovered ──
    if (scanning_done && mqtt_is_connected() && cfg.ha_discovery && module_count > 0)
    {
        for (uint16_t i = 0; i < module_count; i++)
        {
            if (!modules[i].discovered)
            {
                mqtt_publish_discovery(&modules[i]);
                mqtt_subscribe_commands(&modules[i]);
                modules[i].discovered = true;
                // Feed task WDT after each module discovery (prevents WDT timeout)
                esp_task_wdt_reset();
                yield();
            }
        }
    }

    // ── Modbus scan (rescan triggered from web UI) ──────────
    // Scan multiple addresses per loop iteration for faster bus scanning
    // Skip scan if Modbus is paused (SD exclusive mode)
    if (scan_active && !modbus_is_paused())
    {
        for (int i = 0; i < 10 && scan_active; i++)
        {
            scan_modbus_next();
        }
    }

    // ── Modbus polling ──────────────────────────────────────
    if (scanning_done && module_count > 0)
    {
        if (millis() - last_poll_ms >= cfg.mb_poll_ms && !bus_busy && !modbus_is_paused())
        {
            poll_all_modules();
            last_poll_ms = millis();
        }
    }

    // ── Register polling (ONE register per loop iteration to prevent WDT timeout) ──
    static uint8_t reg_poll_idx = 0;
    if (register_count > 0 && scanning_done && mqtt_is_connected() && !bus_busy && !modbus_is_paused())
    {
        // Find next register to poll (round-robin, one per loop)
        for (uint8_t attempts = 0; attempts < register_count; attempts++)
        {
            RegisterConfig &reg = registers[reg_poll_idx];
            reg_poll_idx = (reg_poll_idx + 1) % register_count;

            if (!reg.enabled)
                continue;
            if (millis() - reg.last_read_ms < cfg.mb_poll_ms)
                continue;

            uint16_t raw_value = 0;
            if (modbus_read_register(reg.slave_addr, reg.reg_type, reg.addr, &raw_value))
            {
                float new_value = (reg.scale > 0) ? (float)raw_value / (float)reg.scale : (float)raw_value;
                bool changed = !reg.published || (fabsf(new_value - reg.last_value) > 0.001f);

                reg.last_value = new_value;
                reg.last_read_ms = millis();

                // Publish discovery on first successful read
                if (!reg.published)
                {
                    mqtt_publish_register_discovery(&reg);
                }

                // Publish value if changed or not yet published
                if (changed || !reg.published)
                {
                    mqtt_publish_register_value(&reg);
                }
            }
            else
            {
                // Modbus read failed — record timestamp to respect poll interval
                reg.last_read_ms = millis();
            }

            // Feed task WDT after each register (Modbus timeout can block 500ms+)
            esp_task_wdt_reset();
            yield();
            break; // Only ONE register per loop iteration
        }
    }

    // ── Modbus stats & bridge state every 30s ──────────────
    if (millis() - last_stats_ms >= 30000)
    {
        mqtt_publish_stats();
        mqtt_publish_bridge_state();
        last_stats_ms = millis();
    }

    check_rescan();
    update_status_led();

    // ── Watchdog health check ────────────────────────────────
    wdt_check();

    delay(1);
}