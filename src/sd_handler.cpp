// ─── SD Card Handler — SDIO 1-bit + SPI fallback ──────────────────
// Waveshare ESP32-S3-ETH V1.0 SD card slot
//
// SDIO 1-bit (NO CS needed!):
//   CLK=GPIO7, CMD=GPIO6, D0=GPIO5
//   Works even without R7/R19 (GPIO4→CS connection)
//
// SPI fallback (CS required — only works if R7/R19 populated):
//   MOSI=6, MISO=5, SCLK=7, CS=4  (HSPI)
//
// RS485 DE = GPIO42 (moved from GPIO4)
// W5500 = FSPI (MOSI=11, MISO=12, SCLK=13, CS=14) — SEPARATE bus
//
// SD is OPTIONAL: firmware works perfectly without SD card.
// Compile with -DUSE_SD to include SD card support.

#include "modbus_mqtt_ha_bridge.h"

#ifdef USE_SD

#include <SD_MMC.h>
#include <SD.h>
#include <SPI.h>

// ─── State ────────────────────────────────────────────────────
static bool sd_initialized = false;
static bool sd_pin_conflict = false;
static bool sd_using_sdio = false;   // true = SDIO 1-bit, false = SPI
static uint64_t sd_total_bytes = 0;
static uint64_t sd_used_bytes = 0;
static char sd_card_type[16] = "NONE";

// SPI bus (fallback)
static SPIClass *sd_spi = nullptr;

// ─── Helper ───────────────────────────────────────────────────
static char *psram_strdup(const char *s)
{
    size_t len = strlen(s);
    char *buf = (char *)psram_malloc(len + 1);
    if (buf) memcpy(buf, s, len + 1);
    return buf;
}

// ─── W5500 CS management ────────────────────────────────────────
// Ensure W5500 CS is HIGH during SD SPI transactions
static void w5500_cs_high()
{
    if (cfg.pin_eth_cs >= 0)
    {
        pinMode(cfg.pin_eth_cs, OUTPUT);
        digitalWrite(cfg.pin_eth_cs, HIGH);
    }
}

// ─── SDIO 1-bit Init ──────────────────────────────────────────
// Uses Arduino SD_MMC library (wraps ESP-IDF sdmmc driver).
// Requires only CLK, CMD, D0 — NO CS line needed!
// ESP32-S3 supports GPIO matrix remap → any GPIO for SDMMC.
static bool sd_init_sdio()
{
    LOG_I("[SD] Trying SDIO 1-bit mode (CLK=7, CMD=6, D0=5)...\n");

    // Set custom GPIO pins for SDMMC (GPIO matrix remap!)
    // Waveshare board: CLK=7, CMD=6, D0=5 — NOT the ESP32-S3 default IOMUX
    if (!SD_MMC.setPins(PIN_SD_SCLK, PIN_SD_MOSI, PIN_SD_MISO))
    {
        LOG_E("[SD] SD_MMC.setPins() failed!\n");
        return false;
    }
    LOG_I("[SD] SD_MMC.setPins: CLK=%d, CMD=%d, D0=%d\n",
          PIN_SD_SCLK, PIN_SD_MOSI, PIN_SD_MISO);

    // Begin in 1-bit mode, 20 MHz, format_if_mount_failed=false
    // mountpoint = "/sdcard"
    if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT))
    {
        LOG_E("[SD] SD_MMC.begin() 1-bit FAILED\n");
        SD_MMC.end();
        return false;
    }

    sd_using_sdio = true;
    sd_initialized = true;

    // Card info
    sdcard_type_t type = SD_MMC.cardType();
    if (type == CARD_MMC)
        strlcpy(sd_card_type, "MMC", sizeof(sd_card_type));
    else if (type == CARD_SD)
        strlcpy(sd_card_type, "SDSC", sizeof(sd_card_type));
    else if (type == CARD_SDHC)
        strlcpy(sd_card_type, "SDHC", sizeof(sd_card_type));
    else
        strlcpy(sd_card_type, "UNKNOWN", sizeof(sd_card_type));

    sd_total_bytes = SD_MMC.cardSize();
    sd_used_bytes = SD_MMC.usedBytes();

    LOG_I("[SD] SDIO 1-bit OK — Type: %s, Size: %llu KB, Used: %llu KB\n",
          sd_card_type, sd_total_bytes / 1024, sd_used_bytes / 1024);

    // Create /registers directory if not exists
    if (!SD_MMC.exists("/registers"))
    {
        SD_MMC.mkdir("/registers");
        LOG_I("[SD] Created /registers directory\n");
    }

    return true;
}

// ─── SPI Init (fallback) ───────────────────────────────────────
static bool sd_init_spi(int8_t cs_pin)
{
    LOG_I("[SD] Trying SPI mode (CS=%d, MOSI=6, MISO=5, SCLK=7)...\n", cs_pin);

    if (cs_pin < 0)
    {
        LOG_E("[SD] SPI mode requires CS pin\n");
        return false;
    }

    // Check pin conflict with RS485 DE
    if (cs_pin == cfg.pin_rs485_de && cfg.pin_rs485_de >= 0)
    {
        sd_pin_conflict = true;
        if (!modbus_is_paused())
        {
            LOG_E("[SD] BLOCKED — GPIO%d is RS485 DE AND SD CS!\n", cs_pin);
            return false;
        }
        LOG_I("[SD] Pin conflict GPIO%d but Modbus paused — exclusive mode\n", cs_pin);
    }
    else
    {
        sd_pin_conflict = false;
    }

    w5500_cs_high();

    if (!sd_spi)
    {
        sd_spi = new SPIClass(HSPI);
        if (!sd_spi)
        {
            LOG_E("[SD] Failed to allocate SPIClass\n");
            return false;
        }
    }

    pinMode(PIN_SD_MISO, INPUT_PULLUP);
    sd_spi->begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI, cs_pin);

    pinMode(cs_pin, OUTPUT);
    digitalWrite(cs_pin, HIGH);
    delay(10);

    w5500_cs_high();

    // Try SD.begin at 400 kHz → 1 MHz → 20 kHz
    if (!SD.begin(cs_pin, *sd_spi, 400000))
    {
        LOG_E("[SD] SPI 400kHz failed — trying 1MHz...\n");
        delay(100);
        w5500_cs_high();
        if (!SD.begin(cs_pin, *sd_spi, 1000000))
        {
            LOG_E("[SD] SPI 1MHz failed — trying 20kHz...\n");
            delay(100);
            w5500_cs_high();
            if (!SD.begin(cs_pin, *sd_spi, 20000))
            {
                LOG_E("[SD] SPI failed at all speeds\n");
                return false;
            }
        }
    }

    sd_using_sdio = false;
    sd_initialized = true;

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

    LOG_I("[SD] SPI OK — Type: %s, Total: %llu KB, Used: %llu KB\n",
          sd_card_type, sd_total_bytes / 1024, sd_used_bytes / 1024);

    if (!SD.exists("/registers"))
    {
        SD.mkdir("/registers");
        LOG_I("[SD] Created /registers directory\n");
    }

    return true;
}

// ─── Unified SD Init ──────────────────────────────────────────
// mode: "sdio" = SDIO 1-bit only, "spi" = SPI only, "auto" = SDIO then SPI
// Default from sd_init(cs_pin) calls sd_init(cs_pin, "spi") — SAFE, no crash risk
bool sd_init(int8_t cs_pin, const char *mode)
{
    if (sd_initialized)
    {
        LOG_I("[SD] Already initialized\n");
        return true;
    }

    // Pause Modbus during SD operations
    modbus_pause();

    bool try_sdio = (strcmp(mode, "sdio") == 0 || strcmp(mode, "auto") == 0);
    bool try_spi  = (strcmp(mode, "spi") == 0  || strcmp(mode, "auto") == 0);

    // ── Attempt 1: SDIO 1-bit (no CS needed, works without R7/R19) ──
    if (try_sdio)
    {
        if (sd_init_sdio())
        {
            modbus_resume();
            return true;
        }
        LOG_I("[SD] SDIO failed\n");
    }

    // ── Attempt 2: SPI fallback (needs CS, R7/R19 must be present) ──
    if (try_spi && cs_pin >= 0)
    {
        if (sd_init_spi(cs_pin))
        {
            modbus_resume();
            return true;
        }
    }

    // Both failed
    modbus_resume();
    LOG_E("[SD] All init methods failed — no SD card or hardware issue\n");
    return false;
}

// Legacy wrapper — SPI only (SAFE, no SDIO crash risk)
bool sd_init(int8_t cs_pin)
{
    return sd_init(cs_pin, "spi");
}

// ─── SD Deinit ────────────────────────────────────────────────
void sd_deinit()
{
    if (!sd_initialized) return;

    if (sd_using_sdio)
    {
        SD_MMC.end();
        LOG_I("[SD] SDIO unmounted\n");
    }
    else
    {
        w5500_cs_high();
        SD.end();
        LOG_I("[SD] SPI unmounted\n");
    }

    sd_initialized = false;
    sd_using_sdio = false;
}

// ─── SD Test Init (diagnostics) ───────────────────────────────
String sd_test_init()
{
    String result = "{";
    result += "\"pin_sd_cs\":" + String(cfg.pin_sd_cs) + ",";
    result += "\"pin_rs485_de\":" + String(cfg.pin_rs485_de) + ",";
    result += "\"pin_eth_cs\":" + String(cfg.pin_eth_cs) + ",";
    result += "\"modbus_paused_before\":" + String(modbus_is_paused() ? "true" : "false") + ",";

    modbus_pause();
    result += "\"modbus_paused_after\":" + String(modbus_is_paused() ? "true" : "false") + ",";

    int cs = cfg.pin_sd_cs;

    // ── Test 1: SDIO 1-bit mode ──
    {
        bool pins_ok = SD_MMC.setPins(PIN_SD_SCLK, PIN_SD_MOSI, PIN_SD_MISO);
        result += "\"sdio_set_pins\":" + String(pins_ok ? "true" : "false") + ",";

        bool sdio_ok = pins_ok && SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT);
        result += "\"sdio_begin\":" + String(sdio_ok ? "true" : "false") + ",";

        if (sdio_ok)
        {
            sdcard_type_t t = SD_MMC.cardType();
            const char *tname = (t == CARD_MMC) ? "MMC" : (t == CARD_SD) ? "SDSC" :
                               (t == CARD_SDHC) ? "SDHC" : "UNKNOWN";
            result += "\"sdio_card_type\":\"" + String(tname) + "\",";
            result += "\"sdio_card_size_kb\":" + String(SD_MMC.cardSize() / 1024) + ",";
            result += "\"sdio_total_kb\":" + String(SD_MMC.totalBytes() / 1024) + ",";
            result += "\"sdio_used_kb\":" + String(SD_MMC.usedBytes() / 1024) + ",";
            SD_MMC.end();
            result += "\"sdio_ok\":true,";
        }
        else
        {
            SD_MMC.end();
            result += "\"sdio_ok\":false,";
        }
    }

    // ── Test 2: Raw SPI diagnostics ──
    {
        if (cs >= 0)
        {
            pinMode(cs, OUTPUT);
            digitalWrite(cs, HIGH);
            delay(10);
            result += "\"cs_high_read\":" + String(digitalRead(cs)) + ",";
        }

        if (cfg.pin_rs485_de >= 0)
        {
            pinMode(cfg.pin_rs485_de, OUTPUT);
            digitalWrite(cfg.pin_rs485_de, LOW);
            delay(5);
            result += "\"de_low_read\":" + String(digitalRead(cfg.pin_rs485_de)) + ",";
        }

        w5500_cs_high();

        if (!sd_spi)
            sd_spi = new SPIClass(HSPI);
        pinMode(PIN_SD_MISO, INPUT_PULLUP);
        sd_spi->begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI, cs);

        if (cs >= 0)
        {
            pinMode(cs, OUTPUT);
            digitalWrite(cs, HIGH);
            delay(10);
            result += "\"cs_high_read2\":" + String(digitalRead(cs)) + ",";
        }
        w5500_cs_high();

        // Raw CMD0 test
        if (cs >= 0)
        {
            sd_spi->beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
            for (int i = 0; i < 10; i++)
                sd_spi->transfer(0xFF);
            int miso_high = digitalRead(PIN_SD_MISO);

            sd_spi->beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
            digitalWrite(cs, LOW);
            delayMicroseconds(1);
            uint8_t cmd0[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
            for (int i = 0; i < 6; i++)
                sd_spi->transfer(cmd0[i]);
            uint8_t resp = 0xFF;
            int attempts = 0;
            for (attempts = 0; attempts < 16; attempts++)
            {
                resp = sd_spi->transfer(0xFF);
                if (resp != 0xFF) break;
            }
            int miso_low = digitalRead(PIN_SD_MISO);
            sd_spi->endTransaction();
            digitalWrite(cs, HIGH);

            result += "\"miso_high\":" + String(miso_high) + ",";
            result += "\"miso_low\":" + String(miso_low) + ",";
            result += "\"cmd0_response\":\"0x" + String(resp, HEX) + "\",";
            result += "\"cmd0_attempts\":" + String(attempts) + ",";
            result += "\"cmd0_ok\":" + String((resp == 0x01) ? "true" : "false") + ",";
        }
    }

    // ── Test 3: SPI mode with Waveshare method ──
    if (cs >= 0)
    {
        SPI.begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI, cs);
        pinMode(PIN_SD_MISO, INPUT_PULLUP);
        bool ok_ws = SD.begin(cs);
        result += "\"ws_sd_begin\":" + String(ok_ws ? "true" : "false") + ",";
        if (ok_ws)
        {
            result += "\"ws_card_type\":\"" + String(SD.cardType()) + "\",";
            result += "\"ws_card_size_kb\":" + String(SD.totalBytes() / 1024) + ",";
            SD.end();
        }
    }

    // SPI with dedicated SPIClass at various speeds
    if (cs >= 0)
    {
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
            }
        }
        else
        {
            result += "\"spi_card_type\":\"" + String(SD.cardType()) + "\",";
            result += "\"spi_total_kb\":" + String(SD.totalBytes() / 1024) + ",";
            SD.end();
        }
    }

    modbus_resume();

    result += "\"ok\":" + String(sd_initialized ? "true" : "false") + ",";
    result += "\"mode\":" + String(sd_using_sdio ? "\"sdio_1bit\"" : (sd_initialized ? "\"spi\"" : "\"none\"")) + ",";
    result += "\"pin_conflict\":" + String(sd_pin_conflict ? "true" : "false");
    result += "}";

    sd_initialized = false;
    return result;
}

// ─── SD Exclusive Mode ────────────────────────────────────────
static bool sd_exclusive_active = false;

bool sd_begin_exclusive()
{
    if (sd_initialized)
    {
        sd_exclusive_active = false;
        return true;
    }

    modbus_pause();

    if (cfg.pin_sd_cs >= 0)
    {
        pinMode(cfg.pin_sd_cs, OUTPUT);
        digitalWrite(cfg.pin_sd_cs, HIGH);
        delay(10);
    }
    w5500_cs_high();

    bool ok = sd_init(cfg.pin_sd_cs);
    if (ok)
    {
        sd_exclusive_active = true;
        LOG_I("[SD] Exclusive mode active (mode=%s)\n", sd_using_sdio ? "SDIO" : "SPI");
    }
    else
    {
        LOG_E("[SD] Exclusive mode: init failed\n");
        modbus_resume();
        sd_exclusive_active = false;
    }
    return ok;
}

void sd_end_exclusive()
{
    if (!sd_exclusive_active) return;
    sd_deinit();
    sd_exclusive_active = false;
    modbus_resume();
    LOG_I("[SD] Exclusive mode ended\n");
}

bool sd_is_exclusive() { return sd_exclusive_active; }

// ─── SD Status ────────────────────────────────────────────────
bool sd_is_ok() { return sd_initialized; }
bool sd_has_pin_conflict() { return sd_pin_conflict; }
bool sd_is_sdio_mode() { return sd_using_sdio; }
uint64_t sd_total_kb()
{
    if (!sd_initialized) return 0;
    return sd_using_sdio ? SD_MMC.totalBytes() / 1024 : SD.totalBytes() / 1024;
}
uint64_t sd_used_kb()
{
    if (!sd_initialized) return 0;
    return sd_using_sdio ? SD_MMC.usedBytes() / 1024 : SD.usedBytes() / 1024;
}
const char *sd_type_str() { return sd_card_type; }

void sd_refresh_stats()
{
    if (!sd_initialized) return;
    if (sd_using_sdio)
    {
        sd_total_bytes = SD_MMC.cardSize();
        sd_used_bytes = SD_MMC.usedBytes();
    }
    else
    {
        sd_total_bytes = SD.totalBytes();
        sd_used_bytes = SD.usedBytes();
    }
}

// ─── File Operations ─────────────────────────────────────────
// SDIO mounts at /sdcard, SPI uses SD library's FS object
// We use a helper to get the right FS object

static fs::FS &sd_fs()
{
    return sd_using_sdio ? (fs::FS &)SD_MMC : (fs::FS &)SD;
}

bool sd_save_register_list(const char *device_name, const char *json_content, size_t json_len)
{
    if (!sd_initialized) return false;

    fs::FS &fs = sd_fs();
    char path[64];
    snprintf(path, sizeof(path), "/registers/%s.json", device_name);

    if (!fs.exists("/registers"))
        fs.mkdir("/registers");

    File f = fs.open(path, FILE_WRITE);
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

char *sd_read_register_list(const char *device_name, size_t *out_len)
{
    if (!sd_initialized) return nullptr;

    fs::FS &fs = sd_fs();
    char path[64];
    snprintf(path, sizeof(path), "/registers/%s.json", device_name);

    File f = fs.open(path, FILE_READ);
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
    return buf;
}

// ─── Directory Listing ───────────────────────────────────────
String sd_list_dir(const char *path, int depth)
{
    if (!sd_initialized) return "[]";

    fs::FS &fs = sd_fs();
    String result = "[";

    File root = fs.open(path);
    if (!root || !root.isDirectory())
    {
        LOG_E("[SD] Cannot open dir %s\n", path);
        return "[]";
    }

    bool first = true;
    File entry = root.openNextFile();
    while (entry)
    {
        if (!first) result += ",";
        first = false;

        result += "{";
        result += "\"name\":\"" + String(entry.name()) + "\",";
        result += "\"size\":" + String(entry.size()) + ",";
        result += "\"is_dir\":" + String(entry.isDirectory() ? "true" : "false");

        if (entry.isDirectory() && depth > 0 && entry.name()[0] != '.')
        {
            String subpath = String(path) + "/" + String(entry.name());
            result += ",\"children\":" + sd_list_dir(subpath.c_str(), depth - 1);
        }

        result += "}";
        entry = root.openNextFile();
    }

    result += "]";
    return result;
}

// ─── Register File List ───────────────────────────────────────
char *sd_list_register_files(size_t *out_len)
{
    if (!sd_initialized) return nullptr;

    fs::FS &fs = sd_fs();
    String json = "[";

    File dir = fs.open("/registers");
    if (!dir || !dir.isDirectory())
    {
        if (out_len) *out_len = 2;
        return psram_strdup("[]");
    }

    bool first = true;
    File f = dir.openNextFile();
    while (f)
    {
        if (!f.isDirectory())
        {
            if (!first) json += ",";
            first = false;
            json += "{\"name\":\"" + String(f.name()) + "\",\"size\":" + String(f.size()) + "}";
        }
        f = dir.openNextFile();
    }

    json += "]";
    size_t len = json.length();
    char *buf = (char *)psram_malloc(len + 1);
    if (buf)
    {
        memcpy(buf, json.c_str(), len);
        buf[len] = '\0';
    }
    if (out_len) *out_len = len;
    return buf;
}

bool sd_delete_register_list(const char *device_name)
{
    if (!sd_initialized) return false;
    fs::FS &fs = sd_fs();
    char path[64];
    snprintf(path, sizeof(path), "/registers/%s.json", device_name);
    return fs.remove(path);
}

// ─── Browse Directory ─────────────────────────────────────────
char *sd_browse_dir(const char *path, size_t *out_len)
{
    if (!sd_initialized) return nullptr;

    String json = sd_list_dir(path, 0);
    size_t len = json.length();
    char *buf = (char *)psram_malloc(len + 1);
    if (buf)
    {
        memcpy(buf, json.c_str(), len);
        buf[len] = '\0';
    }
    if (out_len) *out_len = len;
    return buf;
}

bool sd_mkdir(const char *path)
{
    if (!sd_initialized) return false;
    return sd_fs().mkdir(path);
}

bool sd_format()
{
    // Formatting not supported via Arduino API
    LOG_E("[SD] Format not supported — use PC to format FAT32\n");
    return false;
}

// ─── File CRUD ────────────────────────────────────────────────
char *sd_read_file(const char *path, size_t *out_len)
{
    if (!sd_initialized) return nullptr;

    fs::FS &fs = sd_fs();
    File f = fs.open(path, FILE_READ);
    if (!f) return nullptr;

    size_t fsize = f.size();
    if (fsize == 0 || fsize > 65536)
    {
        f.close();
        return nullptr;
    }

    char *buf = (char *)psram_malloc(fsize + 1);
    if (!buf)
    {
        f.close();
        return nullptr;
    }

    size_t read_bytes = f.readBytes(buf, fsize);
    f.close();
    buf[read_bytes] = '\0';
    if (out_len) *out_len = read_bytes;
    return buf;
}

bool sd_delete_path(const char *path)
{
    if (!sd_initialized) return false;
    fs::FS &fs = sd_fs();
    File f = fs.open(path);
    if (!f) return false;
    if (f.isDirectory())
    {
        f.close();
        return fs.rmdir(path);
    }
    f.close();
    return fs.remove(path);
}

bool sd_append_file(const char *path, const uint8_t *data, size_t len)
{
    if (!sd_initialized) return false;
    File f = sd_fs().open(path, FILE_APPEND);
    if (!f) return false;
    size_t written = f.write(data, len);
    f.close();
    return written == len;
}

bool sd_file_exists(const char *path)
{
    if (!sd_initialized) return false;
    return sd_fs().exists(path);
}

bool sd_remove_file(const char *path)
{
    if (!sd_initialized) return false;
    return sd_fs().remove(path);
}

#endif // USE_SD