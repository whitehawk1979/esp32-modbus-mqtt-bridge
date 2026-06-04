/**
 * watchdog.cpp — Software watchdog & health monitor
 *
 * Monitors:
 *  1. Loop liveness — if loop() doesn't complete within 30s, reboot
 *  2. Bus busy stall — if bus_busy stays true for 10s, reset Modbus
 *  3. MQTT stale — if no publish in 5 min while connected, reconnect
 *  4. Modbus error flood — if >80% errors in last 100 transactions, warn
 *  5. Heap low — if free heap < 20KB, warn + log
 */

#include "modbus_mqtt_ha_bridge.h"
#include <Preferences.h>

// ─── Thresholds ─────────────────────────────────────────────────
#define WDT_LOOP_TIMEOUT_MS 30000 // Reboot if loop stuck 30s
#define WDT_BUS_STALL_MS 10000    // Bus stuck 10s → reset
#define WDT_MQTT_STALE_MS 300000  // 5 min no publish → reconnect
#define WDT_HEAP_WARN 20480       // 20KB free heap warning
#define WDT_ERR_FLOOD_PCT 80      // >80% error rate → warn
#define NV_KEY_WDT_REBOOTS "wdt_rb"

// ─── State ──────────────────────────────────────────────────────
static uint32_t wdt_loop_tick = 0;
static uint32_t wdt_last_publish = 0;
static uint32_t wdt_bus_busy_since = 0;
static uint32_t wdt_reboots = 0;
static bool wdt_initialized = false;

// ─── Public API ─────────────────────────────────────────────────

void wdt_init()
{
    Preferences nv;
    nv.begin(NV_NAMESPACE, true);
    wdt_reboots = nv.getUInt(NV_KEY_WDT_REBOOTS, 0);
    nv.end();

    wdt_loop_tick = millis();
    wdt_last_publish = millis();
    wdt_initialized = true;

    LOG_I("[WDT] Initialized (reboots=%u)\n", wdt_reboots);
}

void wdt_loop_tick_reset()
{
    wdt_loop_tick = millis();
}

void wdt_notify_publish()
{
    wdt_last_publish = millis();
}

void wdt_check()
{
    if (!wdt_initialized)
        return;

    uint32_t now = millis();

    // ── 1. Loop liveness ────────────────────────────────────
    if (now - wdt_loop_tick > WDT_LOOP_TIMEOUT_MS)
    {
        LOG_E("[WDT] ⚠ LOOP STUCK for %us! Rebooting...\n", (now - wdt_loop_tick) / 1000);
        wdt_reboot("loop_stuck");
        return;
    }

    // ── 2. Bus busy stall ────────────────────────────────────
    if (bus_busy)
    {
        if (wdt_bus_busy_since == 0)
        {
            wdt_bus_busy_since = now;
        }
        else if (now - wdt_bus_busy_since > WDT_BUS_STALL_MS)
        {
            LOG_E("[WDT] ⚠ BUS STUCK for %us! Resetting Modbus...\n", (now - wdt_bus_busy_since) / 1000);
            bus_busy = false;
            wdt_bus_busy_since = 0;
            modbus_init();
            mb_stats.err_count++;
        }
    }
    else
    {
        wdt_bus_busy_since = 0;
    }

    // ── 3. MQTT stale check ──────────────────────────────────
    if (mqtt_is_connected() && (now - wdt_last_publish > WDT_MQTT_STALE_MS))
    {
        LOG_E("[WDT] ⚠ MQTT STALE for %us! Forcing reconnect...\n", (now - wdt_last_publish) / 1000);
        wdt_last_publish = now;
        mqtt_force_disconnect();
    }

    // ── 4. Modbus error flood ────────────────────────────────
    uint32_t total = mb_stats.tx_count;
    if (total > 100)
    {
        uint32_t err_pct = (mb_stats.err_count * 100) / total;
        if (err_pct > WDT_ERR_FLOOD_PCT && mb_stats.last_err_time > 0 && now - mb_stats.last_err_time < 30000)
        {
            LOG_E("[WDT] ⚠ MODBUS ERROR FLOOD: %u%% errors (%u/%u)\n", err_pct, mb_stats.err_count, total);
        }
    }

    // ── 5. Heap low ─────────────────────────────────────────
    uint32_t free_heap = ESP.getFreeHeap();
    if (free_heap < WDT_HEAP_WARN)
    {
        LOG_E("[WDT] ⚠ LOW HEAP: %u bytes\n", free_heap);
    }
}

void wdt_reboot(const char *reason)
{
    LOG_E("[WDT] Rebooting: %s\n", reason);

    wdt_reboots++;
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    nv.putUInt(NV_KEY_WDT_REBOOTS, wdt_reboots);
    nv.end();

    delay(100);
    eth_hard_reset_and_restart();
}

uint32_t wdt_get_reboots()
{
    return wdt_reboots;
}