// ─── SD Card Handler ───────────────────────────────────────────
// SD card on SEPARATE SPI bus from W5500 (Waveshare ESP32-S3-ETH V1.0)
// W5500 uses FSPI (MOSI=11, MISO=12, SCLK=13, CS=14)
// SD card uses SPI2 (MOSI=6, MISO=5, SCLK=7, CS=4)
// No bus collision — separate hardware SPI peripherals.
// ⚠️ HARDWARE WARNING: GPIO4 = RS485 DE = SD CS on Waveshare board!
//    SD and Modbus CANNOT be used simultaneously on this board.
//    If cfg.pin_sd_cs == cfg.pin_rs485_de, SD init is BLOCKED.
//
// v2.8.0 — Initial SD card support
// SD is OPTIONAL: firmware works perfectly without SD card.
// Compile with -DUSE_SD to include SD card support.
// Without it, all SD functions compile to safe stubs (no code bloat).

#include "modbus_mqtt_ha_bridge.h"

#ifdef USE_SD
#include <SD.h>
#include <SPI.h>

// ─── SD SPI Bus (separate from W5500 FSPI!) ──────────────────
// Waveshare ESP32-S3-ETH V1.0: SD is on SPI2, NOT shared with W5500
static SPIClass *sd_spi = nullptr;

static bool sd_initialized = false;
static bool sd_pin_conflict = false;  // SD CS == RS485 DE
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

    // ⚠️ HARDWARE PIN CONFLICT CHECK: GPIO4 = RS485 DE = SD CS
    // On Waveshare ESP32-S3-ETH, these share the same GPIO!
    if (cs_pin == cfg.pin_rs485_de && cfg.pin_rs485_de >= 0)
    {
        sd_pin_conflict = true;
        // If Modbus is paused (exclusive mode), we CAN proceed safely
        // because DE is held LOW and Modbus won't touch the pin.
        if (!modbus_is_paused())
        {
            LOG_E("[SD] BLOCKED — GPIO%d is RS485 DE AND SD CS! Cannot use both.\n", cs_pin);
            LOG_E("[SD] Change SD CS or RS485 DE pin to resolve conflict.\n");
            return false;
        }
        LOG_I("[SD] Pin conflict GPIO%d (DE=CS) but Modbus paused — proceeding in exclusive mode\n", cs_pin);
    }
    else
    {
        sd_pin_conflict = false;
    }

    LOG_I("[SD] Initializing with CS=%d on SEPARATE SPI bus (MOSI=6,MISO=5,SCLK=7)...\n", cs_pin);

    // Create separate SPI bus for SD — does NOT interfere with W5500 FSPI
    if (!sd_spi)
    {
        sd_spi = new SPIClass(HSPI);  // HSPI = SPI2, separate from FSPI used by W5500
        if (!sd_spi)
        {
            LOG_E("[SD] Failed to allocate SPI bus\n");
            return false;
        }
    }

    // Initialize SD SPI bus with correct Waveshare pin mapping
    sd_spi->begin(7, 5, 6, cs_pin);  // SCLK=7, MISO=5, MOSI=6, CS=cs_pin

    // Explicitly set CS pin HIGH before SD.begin
    pinMode(cs_pin, OUTPUT);
    digitalWrite(cs_pin, HIGH);
    delay(10);

    // Start with low speed for init (SD spec requires ≤400kHz for CMD0)
    if (!SD.begin(cs_pin, *sd_spi, 400000))  // 400 kHz initial — SD spec
    {
        LOG_E("[SD] FAILED at 400kHz — trying 1MHz...\n");
        delay(100);
        if (!SD.begin(cs_pin, *sd_spi, 1000000))  // try 1 MHz
        {
            LOG_E("[SD] FAILED — no card or bad wiring\n");
            sd_initialized = false;
            return false;
        }
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

// ─── SD Test Init (detailed diagnostics) ────────────────────────
String sd_test_init()
{
    // Returns JSON string with step-by-step SD init results
    String result = "{";
    result += "\"pin_sd_cs\":" + String(cfg.pin_sd_cs) + ",";
    result += "\"pin_rs485_de\":" + String(cfg.pin_rs485_de) + ",";
    result += "\"modbus_paused_before\":" + String(modbus_is_paused() ? "true" : "false") + ",";

    // Step 1: Pause Modbus
    modbus_pause();
    result += "\"modbus_paused_after\":" + String(modbus_is_paused() ? "true" : "false") + ",";

    // Step 2: Drive CS HIGH
    int cs = cfg.pin_sd_cs;
    if (cs >= 0)
    {
        pinMode(cs, OUTPUT);
        digitalWrite(cs, HIGH);
        delay(10);
        int cs_read = digitalRead(cs);
        result += "\"cs_high_read\":" + String(cs_read) + ",";
    }
    else
    {
        result += "\"cs_high_read\":\"N/A\",";
    }

    // Step 3: Drive DE LOW explicitly
    if (cfg.pin_rs485_de >= 0)
    {
        pinMode(cfg.pin_rs485_de, OUTPUT);
        digitalWrite(cfg.pin_rs485_de, LOW);
        delay(5);
        int de_read = digitalRead(cfg.pin_rs485_de);
        result += "\"de_low_read\":" + String(de_read) + ",";
    }

    // Step 4: SPI bus init
    if (!sd_spi)
    {
        sd_spi = new SPIClass(HSPI);
        result += "\"spi_created\":true,";
    }
    else
    {
        result += "\"spi_created:false,spi_exists:true\",";
    }

    sd_spi->begin(7, 5, 6, cs);  // SCLK=7, MISO=5, MOSI=6
    result += "\"spi_begin_ok\":true,";

    // Step 5: CS HIGH again after SPI.begin (SPI.begin may change pin modes)
    pinMode(cs, OUTPUT);
    digitalWrite(cs, HIGH);
    delay(10);
    int cs_read2 = digitalRead(cs);
    result += "\"cs_high_read2\":" + String(cs_read2) + ",";

    // Step 6: SD.begin at 400kHz
    bool ok400 = SD.begin(cs, *sd_spi, 400000);
    result += "\"sd_begin_400khz\":" + String(ok400 ? "true" : "false") + ",";

    if (!ok400)
    {
        delay(100);
        bool ok1000 = SD.begin(cs, *sd_spi, 1000000);
        result += "\"sd_begin_1mhz\":" + String(ok1000 ? "true" : "false") + ",";

        if (!ok1000)
        {
            delay(100);
            bool ok20000 = SD.begin(cs, *sd_spi, 20000);
            result += "\"sd_begin_20khz\":" + String(ok20000 ? "true" : "false") + ",";

            if (!ok20000)
            {
                // All failed — cleanup and resume
                SD.end();
                modbus_resume();
                result += "\"result\":\"ALL_FAILED\"";
                result += "}";
                return result;
            }
            else
            {
                result += "\"card_type\":\"" + String(SD.cardType()) + "\",";
                result += "\"card_size_kb\":" + String(SD.totalBytes() / 1024) + ",";
                SD.end();
            }
        }
        else
        {
            result += "\"card_type\":" + String(SD.cardType()) + ",";
            result += "\"card_size_kb\":" + String(SD.totalBytes() / 1024) + ",";
            SD.end();
        }
    }
    else
    {
        result += "\"card_type\":" + String(SD.cardType()) + ",";
        result += "\"total_kb\":" + String(SD.totalBytes() / 1024) + ",";
        result += "\"used_kb\":" + String(SD.usedBytes() / 1024) + ",";
        SD.end();
    }

    modbus_resume();
    result += "\"result\":\"OK\"";
    result += "}";
    sd_initialized = false;  // Reset — we just tested, didn't keep it
    return result;
}
// ─── SD Exclusive Mode ───────────────────────────────────────────
// For boards where GPIO4 is shared between RS485 DE and SD CS (Waveshare ESP32-S3-ETH V1.0).
// Exclusive mode pauses Modbus, initializes SD, and allows browsing.
// When done, deinit SD and resume Modbus.

static bool sd_exclusive_active = false;

bool sd_begin_exclusive()
{
    if (sd_initialized)
    {
        // Already initialized (no conflict config or already in exclusive mode)
        sd_exclusive_active = false; // not our responsibility to end it
        return true;
    }

    // Pause Modbus — this drives RS485 DE LOW so GPIO4 is safe for SD CS
    modbus_pause();

    // Drive GPIO4 (SD CS) HIGH initially — SD spec requires CS high before init
    if (cfg.pin_sd_cs >= 0)
    {
        pinMode(cfg.pin_sd_cs, OUTPUT);
        digitalWrite(cfg.pin_sd_cs, HIGH);
        delay(10);
    }

    // Now safe to init SD because Modbus is paused and DE is LOW
    bool ok = sd_init(cfg.pin_sd_cs);
    if (ok)
    {
        sd_exclusive_active = true;
        LOG_I("[SD] Exclusive mode: SD initialized, Modbus paused\n");
    }
    else
    {
        // Init failed — resume Modbus immediately
        LOG_E("[SD] Exclusive mode: init failed, resuming Modbus\n");
        modbus_resume();
        sd_exclusive_active = false;
    }
    return ok;
}

void sd_end_exclusive()
{
    if (!sd_exclusive_active)
    {
        // Not in exclusive mode (SD was initialized normally or already ended)
        return;
    }

    sd_deinit();
    sd_exclusive_active = false;
    modbus_resume();
    LOG_I("[SD] Exclusive mode ended: SD deinitialized, Modbus resumed\n");
}

bool sd_is_exclusive()
{
    return sd_exclusive_active;
}

// ─── SD Status ──────────────────────────────────────────────────
bool sd_is_ok() { return sd_initialized; }
bool sd_has_pin_conflict() { return sd_pin_conflict; }
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

// ─── Browse Directory (list all files/folders in JSON) ──────────
// Returns JSON: {"path":"/","entries":[{"name":"file.json","size":1234,"is_dir":false},...]}
char *sd_browse_dir(const char *path, size_t *out_len)
{
    if (!sd_initialized)
    {
        if (out_len) *out_len = 0;
        return nullptr;
    }

    File dir = SD.open(path);
    if (!dir || !dir.isDirectory())
    {
        if (out_len) *out_len = 0;
        return nullptr;
    }

    JsonDocument doc;
    doc["path"] = path;
    JsonArray arr = doc.createNestedArray("entries");

    File entry = dir.openNextFile();
    uint16_t count = 0;
    while (entry && count < 64)
    {
        JsonObject obj = arr.createNestedObject();
        const char *ename = entry.name();
        // Strip leading path prefix — SD library returns "/path/file"
        // We want just the filename relative to the browsed directory
        String fullPath = String(ename);
        // Extract just the last segment after the final '/'
        int lastSlash = fullPath.lastIndexOf('/');
        String baseName = (lastSlash >= 0) ? fullPath.substring(lastSlash + 1) : fullPath;
        obj["name"] = baseName;
        obj["is_dir"] = entry.isDirectory();
        if (!entry.isDirectory())
            obj["size"] = (uint32_t)entry.size();
        else
            obj["size"] = 0;
        entry.close();
        entry = dir.openNextFile();
        count++;
    }
    dir.close();

    char *buf = (char *)psram_malloc(8192);
    if (!buf)
    {
        if (out_len) *out_len = 0;
        return nullptr;
    }

    size_t json_len = serializeJson(doc, buf, 8192);
    if (out_len) *out_len = json_len;
    return buf;
}

// ─── Make Directory ───────────────────────────────────────────────
bool sd_mkdir(const char *path)
{
    if (!sd_initialized) return false;
    if (SD.exists(path)) return true; // already exists
    bool ok = SD.mkdir(path);
    if (ok)
    {
        sd_refresh_stats();
        LOG_I("[SD] Created directory: %s\n", path);
    }
    else
    {
        LOG_E("[SD] Failed to create directory: %s\n", path);
    }
    return ok;
}

// ─── Format SD Card ───────────────────────────────────────────────
bool sd_format()
{
    if (!sd_initialized) return false;
    LOG_I("[SD] Formatting SD card...\n");

    // Walk and delete all files from root
    File root = SD.open("/");
    if (!root) return false;

    // Collect files to delete (can't delete while iterating same dir)
    String filesToDelete[128];
    String dirsToDelete[64];
    int fileCount = 0, dirCount = 0;

    File f = root.openNextFile();
    while (f && fileCount < 128)
    {
        String fpath = String(f.name());
        if (f.isDirectory())
        {
            if (dirCount < 64) dirsToDelete[dirCount++] = fpath;
        }
        else
        {
            filesToDelete[fileCount++] = fpath;
        }
        f.close();
        f = root.openNextFile();
    }
    root.close();

    // Delete files first
    for (int i = 0; i < fileCount; i++)
    {
        SD.remove(filesToDelete[i].c_str());
        LOG_I("[SD] Deleted file: %s\n", filesToDelete[i].c_str());
    }

    // Delete directories (non-recursive — only empty dirs)
    // Walk subdirs and delete their contents first
    for (int i = 0; i < dirCount; i++)
    {
        // Delete contents of subdirectory
        File sub = SD.open(dirsToDelete[i].c_str());
        if (sub)
        {
            File sf = sub.openNextFile();
            while (sf)
            {
                String spath = String(sf.name());
                sf.close();
                if (!SD.remove(spath.c_str()))
                {
                    // It may be a nested dir — try rmdir
                    SD.rmdir(spath.c_str());
                }
                sf = sub.openNextFile();
            }
            sub.close();
        }
        SD.rmdir(dirsToDelete[i].c_str());
        LOG_I("[SD] Removed dir: %s\n", dirsToDelete[i].c_str());
    }

    // Recreate /registers directory
    SD.mkdir("/registers");

    sd_refresh_stats();
    LOG_I("[SD] Format complete\n");
    return true;
}

// ─── Write Raw File ───────────────────────────────────────────────
bool sd_write_file(const char *path, const uint8_t *data, size_t len)
{
    if (!sd_initialized) return false;

    if (SD.exists(path))
        SD.remove(path);

    File f = SD.open(path, FILE_WRITE);
    if (!f)
    {
        LOG_E("[SD] Failed to open %s for write\n", path);
        return false;
    }

    size_t written = f.write(data, len);
    f.close();
    sd_refresh_stats();

    LOG_I("[SD] Wrote %s — %u bytes\n", path, written);
    return written == len;
}

// ─── Read Raw File Content ────────────────────────────────────────
char *sd_read_file(const char *path, size_t *out_len)
{
    if (!sd_initialized) return nullptr;

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

// ─── Delete File or Empty Directory ───────────────────────────────
bool sd_delete_path(const char *path)
{
    if (!sd_initialized) return false;
    if (!SD.exists(path)) return false;

    File f = SD.open(path);
    bool ok;
    if (f && f.isDirectory())
    {
        f.close();
        ok = SD.rmdir(path);
    }
    else
    {
        if (f) f.close();
        ok = SD.remove(path);
    }

    if (ok)
    {
        sd_refresh_stats();
        LOG_I("[SD] Deleted %s\n", path);
    }
    return ok;
}

// ─── Append Data to File (for chunked upload) ────────────────────
bool sd_append_file(const char *path, const uint8_t *data, size_t len)
{
    if (!sd_initialized) return false;

    File f = SD.open(path, FILE_APPEND);
    if (!f)
    {
        LOG_E("[SD] Failed to open %s for append\n", path);
        return false;
    }

    size_t written = f.write(data, len);
    f.close();

    return written == len;
}

// ─── Check if File/Dir Exists ─────────────────────────────────────
bool sd_file_exists(const char *path)
{
    if (!sd_initialized) return false;
    return SD.exists(path);
}

// ─── Remove a Single File (no dir check) ──────────────────────────
bool sd_remove_file(const char *path)
{
    if (!sd_initialized) return false;
    return SD.remove(path);
}

#else // !USE_SD — Stub implementations (zero code bloat)

bool sd_init(int8_t) { return false; }
void sd_deinit() {}
bool sd_is_ok() { return false; }
bool sd_has_pin_conflict() { return false; }
bool sd_begin_exclusive() { return false; }
void sd_end_exclusive() {}
bool sd_is_exclusive() { return false; }
uint64_t sd_total_kb() { return 0; }
uint64_t sd_used_kb() { return 0; }
const char *sd_type_str() { return "NONE"; }
void sd_refresh_stats() {}
bool sd_save_register_list(const char *, const char *, size_t) { return false; }
char *sd_read_register_list(const char *, size_t *len) { if (len) *len = 0; return nullptr; }
char *sd_list_register_files(size_t *len) { if (len) *len = 0; return nullptr; }
bool sd_delete_register_list(const char *) { return false; }
char *sd_browse_dir(const char *, size_t *len) { if (len) *len = 0; return nullptr; }
bool sd_mkdir(const char *) { return false; }
bool sd_format() { return false; }
bool sd_write_file(const char *, const uint8_t *, size_t) { return false; }
char *sd_read_file(const char *, size_t *len) { if (len) *len = 0; return nullptr; }
bool sd_delete_path(const char *) { return false; }
bool sd_append_file(const char *, const uint8_t *, size_t) { return false; }
bool sd_file_exists(const char *) { return false; }
bool sd_remove_file(const char *) { return false; }

#endif // USE_SD