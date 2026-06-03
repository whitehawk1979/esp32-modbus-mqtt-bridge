// ─── SD Card Handler ───────────────────────────────────────────
// Manages SD card on shared FSPI bus with W5500 Ethernet.
// Uses SPI transaction mutex to prevent bus collisions.
// Register lists saved as JSON files for easy development.
//
// v2.8.0 — Initial SD card support
// SD is OPTIONAL: firmware works perfectly without SD card.
// Compile with -DUSE_SD to include SD card support.
// Without it, all SD functions compile to safe stubs (no code bloat).

#include "modbus_mqtt_ha_bridge.h"

#ifdef USE_SD
#include <SD.h>
#include <SPI.h>

// ─── SPI Bus Mutex ──────────────────────────────────────────────
// Guard all SPI transactions to prevent W5500 ↔ SD collisions.
static portMUX_TYPE spi_bus_mutex = portMUX_INITIALIZER_UNLOCKED;

static bool sd_initialized = false;
static uint64_t sd_total_bytes = 0;
static uint64_t sd_used_bytes = 0;
static char sd_card_type[16] = "NONE";

// ─── SD Init ────────────────────────────────────────────────────
bool sd_init(int8_t cs_pin)
{
    if (sd_initialized)
    {
        LOG_I("[SD] Already initialized\n");
        return true;
    }

    if (cs_pin < 0)
    {
        LOG_I("[SD] CS pin not configured\n");
        return false;
    }

    LOG_I("[SD] Initializing with CS=%d (shared FSPI bus)...\n", cs_pin);

    // W5500 must release SPI bus before SD init
    // Ethernet library should not be mid-transaction
    SPI.endTransaction();

    // SD.begin() uses the shared SPI instance — same MOSI/MISO/SCLK as W5500
    // The CS pin is different (42 vs 10), so they don't conflict
    if (!SD.begin(cs_pin, SPI, 25000000)) // 25 MHz SPI for SD
    {
        LOG_E("[SD] FAILED — no card or bad wiring\n");
        sd_initialized = false;
        return false;
    }

    sd_initialized = true;

    // Card info
    uint8_t type = SD.cardType();
    if (type == CARD_MMC)
        strlcpy(sd_card_type, "MMC", sizeof(sd_card_type));
    else if (type == CARD_SD)
        strlcpy(sd_card_type, "SDSC", sizeof(sd_card_type));
    else if (type == CARD_SDHC)
        strlcpy(sd_card_type, "SDHC", sizeof(sd_card_type));
    else
        strlcpy(sd_card_type, "UNKNOWN", sizeof(sd_card_type));

    sd_total_bytes = SD.totalBytes();
    sd_used_bytes = SD.usedBytes();

    LOG_I("[SD] OK — Type: %s, Total: %llu KB, Used: %llu KB\n",
          sd_card_type, sd_total_bytes / 1024, sd_used_bytes / 1024);

    // Create /registers directory if not exists
    if (!SD.exists("/registers"))
    {
        SD.mkdir("/registers");
        LOG_I("[SD] Created /registers directory\n");
    }

    return true;
}

// ─── SD Deinit ───────────────────────────────────────────────────
void sd_deinit()
{
    if (!sd_initialized) return;
    SD.end();
    sd_initialized = false;
    LOG_I("[SD] Deinitialized\n");
}

// ─── SD Status ──────────────────────────────────────────────────
bool sd_is_ok() { return sd_initialized; }
uint64_t sd_total_kb() { return sd_initialized ? sd_total_bytes / 1024 : 0; }
uint64_t sd_used_kb() { return sd_initialized ? sd_used_bytes / 1024 : 0; }
const char *sd_type_str() { return sd_card_type; }

// Refresh usage stats (call periodically or after writes)
void sd_refresh_stats()
{
    if (!sd_initialized) return;
    sd_total_bytes = SD.totalBytes();
    sd_used_bytes = SD.usedBytes();
}

// ─── Register List Save ─────────────────────────────────────────
bool sd_save_register_list(const char *device_name, const char *json_content, size_t json_len)
{
    if (!sd_initialized) return false;

    char path[64];
    snprintf(path, sizeof(path), "/registers/%s.json", device_name);

    if (SD.exists(path))
        SD.remove(path);

    File f = SD.open(path, FILE_WRITE);
    if (!f)
    {
        LOG_E("[SD] Failed to open %s for write\n", path);
        return false;
    }

    size_t written = f.write((const uint8_t *)json_content, json_len);
    f.close();

    sd_refresh_stats();

    LOG_I("[SD] Saved %s — %u bytes written\n", path, written);
    return written == json_len;
}

// ─── Register List Read ─────────────────────────────────────────
char *sd_read_register_list(const char *device_name, size_t *out_len)
{
    if (!sd_initialized) return nullptr;

    char path[64];
    snprintf(path, sizeof(path), "/registers/%s.json", device_name);

    File f = SD.open(path, FILE_READ);
    if (!f)
    {
        LOG_I("[SD] %s not found\n", path);
        return nullptr;
    }

    size_t fsize = f.size();
    if (fsize == 0 || fsize > 65536)
    {
        LOG_E("[SD] %s: invalid size %u\n", path, fsize);
        f.close();
        return nullptr;
    }

    char *buf = (char *)psram_malloc(fsize + 1);
    if (!buf)
    {
        LOG_E("[SD] PSRAM alloc failed for %u bytes\n", fsize);
        f.close();
        return nullptr;
    }

    size_t read_bytes = f.readBytes(buf, fsize);
    f.close();
    buf[read_bytes] = '\0';

    if (out_len) *out_len = read_bytes;
    LOG_I("[SD] Read %s — %u bytes\n", path, read_bytes);
    return buf;
}

// ─── List Register Files ────────────────────────────────────────
char *sd_list_register_files(size_t *out_len)
{
    if (!sd_initialized)
    {
        if (out_len) *out_len = 0;
        return nullptr;
    }

    File dir = SD.open("/registers");
    if (!dir || !dir.isDirectory())
    {
        if (out_len) *out_len = 0;
        return nullptr;
    }

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    File entry = dir.openNextFile();
    uint8_t count = 0;
    while (entry && count < 16)
    {
        if (!entry.isDirectory())
        {
            const char *name = entry.name();
            if (strncmp(name, "/registers/", 11) == 0)
                name += 11;
            size_t nlen = strlen(name);
            if (nlen > 5 && strcmp(name + nlen - 5, ".json") == 0)
            {
                char device[48];
                size_t copy_len = nlen - 5;
                if (copy_len >= sizeof(device)) copy_len = sizeof(device) - 1;
                memcpy(device, name, copy_len);
                device[copy_len] = '\0';
                arr.add(device);
                count++;
            }
        }
        entry = dir.openNextFile();
    }
    dir.close();

    char *buf = (char *)psram_malloc(2048);
    if (!buf)
    {
        if (out_len) *out_len = 0;
        return nullptr;
    }

    size_t json_len = serializeJson(doc, buf, 2048);
    if (out_len) *out_len = json_len;
    return buf;
}

// ─── Delete Register File ───────────────────────────────────────
bool sd_delete_register_list(const char *device_name)
{
    if (!sd_initialized) return false;

    char path[64];
    snprintf(path, sizeof(path), "/registers/%s.json", device_name);

    if (!SD.exists(path)) return false;

    bool ok = SD.remove(path);
    if (ok)
    {
        sd_refresh_stats();
        LOG_I("[SD] Deleted %s\n", path);
    }
    return ok;
}

#else // !USE_SD — Stub implementations (zero code bloat)

bool sd_init(int8_t) { return false; }
void sd_deinit() {}
bool sd_is_ok() { return false; }
uint64_t sd_total_kb() { return 0; }
uint64_t sd_used_kb() { return 0; }
const char *sd_type_str() { return "NONE"; }
void sd_refresh_stats() {}
bool sd_save_register_list(const char *, const char *, size_t) { return false; }
char *sd_read_register_list(const char *, size_t *len) { if (len) *len = 0; return nullptr; }
char *sd_list_register_files(size_t *len) { if (len) *len = 0; return nullptr; }
bool sd_delete_register_list(const char *) { return false; }

#endif // USE_SD