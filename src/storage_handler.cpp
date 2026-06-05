/**
 * storage_handler.cpp — LittleFS Storage on 10MB Flash Partition
 *
 * Waveshare ESP32-S3-ETH V1.0 Modbus-MQTT Bridge
 * Mounts LittleFS on the 'storage' partition (0x620000, ~10MB).
 *
 * Uses ESP32 Arduino core's LittleFS with partition label support.
 * The partition label 'storage' must have type 'spiffs' (0x82) for
 * LittleFS to find it — 'fat' (0x01) will NOT work.
 *
 * Directories created on first mount:
 *   /profiles/  — device profile storage
 *   /active/   — active configuration snapshots
 *
 * Compile with -DUSE_STORAGE to enable.
 */

#include "modbus_mqtt_ha_bridge.h"

#ifdef USE_STORAGE

#include <LittleFS.h>
#include <ArduinoJson.h>

// ─── State ─────────────────────────────────────────────────────
static bool storage_mounted = false;

// ─── Internal: ensure directory exists ────────────────────────
static bool ensure_dir(const char *path)
{
    if (!LittleFS.exists(path))
    {
        if (LittleFS.mkdir(path))
        {
            LOG_I("[STORAGE] Created directory: %s\n", path);
            return true;
        }
        else
        {
            LOG_E("[STORAGE] Failed to create directory: %s\n", path);
            return false;
        }
    }
    return true;
}

// ─── Mount LittleFS ────────────────────────────────────────────
bool storage_init()
{
    if (storage_mounted)
    {
        LOG_I("[STORAGE] Already mounted\n");
        return true;
    }

    LOG_I("[STORAGE] Mounting LittleFS on 'storage' partition...\n");

    // LittleFS.begin(true) = format on fail (first use)
    // Uses default partition label — ESP32 Arduino core searches
    // for a 'spiffs' type partition. If 'storage' is typed 'fat',
    // we need to specify the partition explicitly.
    //
    // Try with explicit partition label first:
    if (!LittleFS.begin(true, "/storage", 10, "storage"))
    {
        // Fallback: try default mount (looks for "spiffs" label)
        LOG_I("[STORAGE] Explicit label failed, trying default mount...\n");
        if (!LittleFS.begin(true))
        {
            LOG_E("[STORAGE] LittleFS mount FAILED — check partition type is 'spiffs' not 'fat'\n");
            return false;
        }
    }

    storage_mounted = true;

    uint64_t total = LittleFS.totalBytes();
    uint64_t used  = LittleFS.usedBytes();
    LOG_I("[STORAGE] LittleFS mounted — Total: %llu KB, Used: %llu KB\n",
          total / 1024, used / 1024);

    // Create default directories
    ensure_dir("/profiles");
    ensure_dir("/active");

    return true;
}

// ─── List directory as JSON array ──────────────────────────────
bool storage_list_dir(const char *path, String &json_output)
{
    if (!storage_mounted)
    {
        json_output = "[]";
        return false;
    }

    File root = LittleFS.open(path);
    if (!root || !root.isDirectory())
    {
        LOG_E("[STORAGE] Cannot open directory: %s\n", path);
        json_output = "[]";
        return false;
    }

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    File entry = root.openNextFile();
    while (entry)
    {
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = String(entry.name());
        if (!entry.isDirectory())
        {
            obj["size"] = (uint32_t)entry.size();
        }
        else
        {
            obj["size"] = 0;
            obj["dir"] = true;
        }
        entry = root.openNextFile();
    }

    serializeJson(arr, json_output);
    return true;
}

// ─── Read file content ────────────────────────────────────────
bool storage_read_file(const char *path, String &content)
{
    if (!storage_mounted)
        return false;

    if (!LittleFS.exists(path))
    {
        LOG_E("[STORAGE] File not found: %s\n", path);
        return false;
    }

    File file = LittleFS.open(path, "r");
    if (!file)
    {
        LOG_E("[STORAGE] Failed to open for read: %s\n", path);
        return false;
    }

    content = file.readString();
    file.close();

    LOG_D("[STORAGE] Read %d bytes from %s\n", content.length(), path);
    return true;
}

// ─── Write file content ────────────────────────────────────────
bool storage_write_file(const char *path, const char *content, size_t len)
{
    if (!storage_mounted)
        return false;

    File file = LittleFS.open(path, "w");
    if (!file)
    {
        LOG_E("[STORAGE] Failed to open for write: %s\n", path);
        return false;
    }

    size_t written = file.write((const uint8_t *)content, len);
    file.close();

    if (written != len)
    {
        LOG_E("[STORAGE] Write incomplete: %s (wrote %u of %u)\n",
              path, (unsigned)written, (unsigned)len);
        return false;
    }

    LOG_I("[STORAGE] Wrote %u bytes to %s\n", (unsigned)len, path);
    return true;
}

// ─── Delete file ──────────────────────────────────────────────
bool storage_delete_file(const char *path)
{
    if (!storage_mounted)
        return false;

    if (!LittleFS.exists(path))
    {
        LOG_E("[STORAGE] File not found: %s\n", path);
        return false;
    }

    if (!LittleFS.remove(path))
    {
        LOG_E("[STORAGE] Failed to delete: %s\n", path);
        return false;
    }

    LOG_I("[STORAGE] Deleted: %s\n", path);
    return true;
}

// ─── Check if file exists ─────────────────────────────────────
bool storage_exists(const char *path)
{
    if (!storage_mounted)
        return false;

    return LittleFS.exists(path);
}

// ─── Total space ──────────────────────────────────────────────
uint64_t storage_total_bytes()
{
    if (!storage_mounted)
        return 0;

    return LittleFS.totalBytes();
}

// ─── Used space ───────────────────────────────────────────────
uint64_t storage_used_bytes()
{
    if (!storage_mounted)
        return 0;

    return LittleFS.usedBytes();
}

#endif // USE_STORAGE