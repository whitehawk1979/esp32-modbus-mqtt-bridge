// ─── SD Card Handler ───────────────────────────────────────────
// SD card on SAME FSPI bus as W5500 (Waveshare ESP32-S3-ETH V1.0)
// W5500: MOSI=11, MISO=12, SCLK=13, CS=14 (FSPI)
// SD:    MOSI=11, MISO=12, SCLK=13, CS=4  (SAME FSPI bus!)
// ⚠️ HARDWARE WARNING: GPIO4 = RS485 DE = SD CS on Waveshare board!
//    SD and Modbus CANNOT be used simultaneously on this board.
//    If cfg.pin_sd_cs == cfg.pin_rs485_de, SD init is BLOCKED unless Modbus paused.
//
// v2.9.1 — Fixed: SD uses FSPI (same as W5500), NOT separate HSPI!
//           Schematic confirms: MOSI=GPIO11, MISO=GPIO12, SCLK=GPIO13, CS=GPIO4
//           W5500 CS must be HIGH during SD transactions.
//
// SD is OPTIONAL: firmware works perfectly without SD card.
// Compile with -DUSE_SD to include SD card support.
// Without it, all SD functions compile to safe stubs (no code bloat).

#include "modbus_mqtt_ha_bridge.h"

#ifdef USE_SD
#include <SD.h>
#include <SPI.h>

// ─── SD on shared FSPI bus ──────────────────────────────────
// The W5500 uses FSPI (MOSI=11, MISO=12, SCLK=13, CS=14).
// SD card shares the SAME FSPI bus (MOSI=11, MISO=12, SCLK=13, CS=4).
//
// CRITICAL: We use a DEDICATED SPIClass for SD so that SD.begin() initializes
// the bus with the correct pin mapping. The default SPI object may have been
// configured by the Ethernet library with different settings.
// SD.begin() internally calls SPI.begin() which would reset pin assignments!

static SPIClass *sd_spi = nullptr;

static bool sd_initialized = false;
static bool sd_pin_conflict = false;  // SD CS == RS485 DE
static uint64_t sd_total_bytes = 0;
static uint64_t sd_used_bytes = 0;
static char sd_card_type[16] = "NONE";

// ─── W5500 CS management ────────────────────────────────────────
// When SD uses the same FSPI bus as W5500, we must ensure W5500 CS is HIGH
// during SD transactions so W5500 doesn't see garbage SPI data.
// The W5500 CS pin is cfg.pin_eth_cs (default GPIO14).
static void w5500_cs_high()
{
    if (cfg.pin_eth_cs >= 0)
    {
        pinMode(cfg.pin_eth_cs, OUTPUT);
        digitalWrite(cfg.pin_eth_cs, HIGH);
    }
}

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

    LOG_I("[SD] Initializing with CS=%d on SHARED FSPI bus (MOSI=11,MISO=12,SCLK=13)...\n", cs_pin);

    // Step 1: Ensure W5500 CS is HIGH so it doesn't interfere with SD SPI
    w5500_cs_high();
    LOG_I("[SD] W5500 CS (GPIO%d) set HIGH\n", cfg.pin_eth_cs);

    // Step 2: Create dedicated SPIClass for FSPI with our pin mapping
    // This is critical — SD.begin() calls SPI.begin() internally which would
    // reset pin assignments to ESP32 defaults (23,19,18) if using the shared SPI object!
    if (!sd_spi)
    {
        sd_spi = new SPIClass(FSPI);
        if (!sd_spi)
        {
            LOG_E("[SD] Failed to allocate SPIClass\n");
            return false;
        }
    }
    sd_spi->begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI, cs_pin);

    // Step 3: Drive SD CS HIGH before SD.begin (SD spec requires CS high at init)
    pinMode(cs_pin, OUTPUT);
    digitalWrite(cs_pin, HIGH);
    delay(10);

    // Step 4: Re-assert W5500 CS HIGH after SPI.begin (may have changed pin modes)
    w5500_cs_high();

    // Step 5: Start with low speed for init (SD spec requires ≤400kHz for CMD0)
    if (!SD.begin(cs_pin, *sd_spi, 400000))  // 400 kHz initial — SD spec
    {
        LOG_E("[SD] FAILED at 400kHz — trying 1MHz...\n");
        delay(100);
        // Re-assert W5500 CS HIGH before retry
        w5500_cs_high();
        if (!SD.begin(cs_pin, *sd_spi, 1000000))  // try 1 MHz
        {
            LOG_E("[SD] FAILED — trying 20kHz...\n");
            delay(100);
            w5500_cs_high();
            if (!SD.begin(cs_pin, *sd_spi, 20000))  // try 20 kHz (extreme fallback)
            {
                LOG_E("[SD] FAILED at all speeds — no card or bad wiring\n");
                sd_initialized = false;
                return false;
            }
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

    // Re-assert W5500 CS HIGH during cleanup
    w5500_cs_high();
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
    result += "\"pin_eth_cs\":" + String(cfg.pin_eth_cs) + ",";
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

    // Step 4: Drive W5500 CS HIGH (shared bus!)
    if (cfg.pin_eth_cs >= 0)
    {
        w5500_cs_high();
        delay(5);
        int eth_cs_read = digitalRead(cfg.pin_eth_cs);
        result += "\"eth_cs_high_read\":" + String(eth_cs_read) + ",";
    }

    // Step 5: Initialize DEDICATED FSPI SPIClass with correct Waveshare pins
    if (!sd_spi)
        sd_spi = new SPIClass(FSPI);
    sd_spi->begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI, cs);
    result += "\"spi_begin_pins\":\"SCLK=" + String(PIN_SD_SCLK) + ",MISO=" + String(PIN_SD_MISO) + ",MOSI=" + String(PIN_SD_MOSI) + "\",";
    result += "\"spi_begin_ok\":true,";

    // Step 6: CS HIGH again after SPI.begin (SPI.begin may change pin modes)
    pinMode(cs, OUTPUT);
    digitalWrite(cs, HIGH);
    delay(10);
    int cs_read2 = digitalRead(cs);
    result += "\"cs_high_read2\":" + String(cs_read2) + ",";

    // Step 7: W5500 CS HIGH again after SPI.begin
    w5500_cs_high();

    // Step 7b: RAW SPI DEBUG — send CMD0 (GO_IDLE_STATE) directly
    // This tells us if the SD card physically responds on the SPI bus
    {
        // First: dummy clock cycles with CS HIGH (SD spec: 74+ clocks before CMD0)
        sd_spi->beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
        for (int i = 0; i < 10; i++)  // 10 × 8 = 80 clock cycles
            sd_spi->transfer(0xFF);
        sd_spi->endTransaction();

        // Check MISO level with CS HIGH (should be pulled up by card or resistor)
        int miso_high = digitalRead(PIN_SD_MISO);

        // Now assert CS and send CMD0
        sd_spi->beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
        digitalWrite(cs, LOW);  // Assert SD CS
        delayMicroseconds(1);

        // CMD0: 0x40 0x00 0x00 0x00 0x00 0x95 (CMD0 with valid CRC)
        uint8_t cmd0[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
        for (int i = 0; i < 6; i++)
            sd_spi->transfer(cmd0[i]);

        // Read response (up to 16 bytes, looking for R1 response 0x01 = idle)
        uint8_t resp = 0xFF;
        int attempts = 0;
        for (attempts = 0; attempts < 16; attempts++)
        {
            resp = sd_spi->transfer(0xFF);
            if (resp != 0xFF) break;  // Got a response
        }

        // Check MISO while CS is LOW
        int miso_low = digitalRead(PIN_SD_MISO);

        sd_spi->endTransaction();
        digitalWrite(cs, HIGH);

        result += "\"miso_high\":" + String(miso_high) + ",";
        result += "\"miso_low\":" + String(miso_low) + ",";
        result += "\"cmd0_response\":\"0x" + String(resp, HEX) + "\",";
        result += "\"cmd0_attempts\":" + String(attempts) + ",";
        result += "\"cmd0_ok\":" + String((resp == 0x01) ? "true" : "false") + ",";
    }

    // Step 8: SD.begin at 400kHz
    bool ok400 = SD.begin(cs, *sd_spi, 400000);
    result += "\"sd_begin_400khz\":" + String(ok400 ? "true" : "false") + ",";

    if (!ok400)
    {
        delay(100);
        w5500_cs_high();
        bool ok1000 = SD.begin(cs, *sd_spi, 1000000);
        result += "\"sd_begin_1mhz\":" + String(ok1000 ? "true" : "false") + ",";

        if (!ok1000)
        {
            delay(100);
            w5500_cs_high();
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

    // Drive W5500 CS HIGH — we share FSPI bus!
    w5500_cs_high();

    // Now safe to init SD because Modbus is paused, DE is LOW, W5500 CS is HIGH
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
        String fullPath = String(ename);
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

    File root = SD.open("/");
    if (!root) return false;

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

    for (int i = 0; i < fileCount; i++)
    {
        SD.remove(filesToDelete[i].c_str());
        LOG_I("[SD] Deleted file: %s\n", filesToDelete[i].c_str());
    }

    for (int i = 0; i < dirCount; i++)
    {
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
                    SD.rmdir(spath.c_str());
                }
                sf = sub.openNextFile();
            }
            sub.close();
        }
        SD.rmdir(dirsToDelete[i].c_str());
        LOG_I("[SD] Removed dir: %s\n", dirsToDelete[i].c_str());
    }

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