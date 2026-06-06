/**
 * modbus_mqtt_ha_bridge.h — Configuration & Data Structures
 *
 * Universal Modbus-MQTT Bridge for Waveshare ESP32-S3-ETH V1.0 modules
 * ESP32-S3 | Arduino | PlatformIO | ESPHome-independent
 *
 * v2.0 — Ethernet+WiFi, static IP, unlimited modules, HA discovery toggle
 */

#ifndef MODBUS_MQTT_HA_BRIDGE_H
#define MODBUS_MQTT_HA_BRIDGE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// ─── Logging Macros ──────────────────────────────────────────
// LOG_E = always (errors, warnings)
// LOG_I = info (boot, connect, ready) — active when CORE_DEBUG_LEVEL >= 1
// LOG_D = debug (callback, poll, TCP detail) — active when CORE_DEBUG_LEVEL >= 3
#if CORE_DEBUG_LEVEL >= 3
#define LOG_D(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#define LOG_DLN(msg) Serial.println(msg)
#elif CORE_DEBUG_LEVEL >= 1
#define LOG_D(fmt, ...) ((void)0)
#define LOG_DLN(msg) ((void)0)
#else
#define LOG_D(fmt, ...) ((void)0)
#define LOG_DLN(msg) ((void)0)
#endif

#if CORE_DEBUG_LEVEL >= 1
#define LOG_I(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#define LOG_ILN(msg) Serial.println(msg)
#else
#define LOG_I(fmt, ...) ((void)0)
#define LOG_ILN(msg) ((void)0)
#endif

#define LOG_E(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#define LOG_ELN(msg) Serial.println(msg)

// ─── Board Pin Definitions (ESP32-S3) ───────────────────────────
#define PIN_RS485_RX 44  // UART2 RX (Modbus)
#define PIN_RS485_TX 43  // UART2 TX (Modbus)
#define PIN_RS485_DE 42  // RS485 Driver Enable (DE/RE) — moved from GPIO4 to free SD CS
#define PIN_STATUS_LED 2 // Built-in LED (GPIO2 — boot/standalone)
#define PIN_WS2812    21 // WS2812B RGB LED (Waveshare ESP32-S3-ETH V1.0)
#define PIN_CONFIG_BTN 0 // BOOT button — hold = WiFi config portal

// W5500 SPI Ethernet pins (Waveshare ESP32-S3-ETH V1.0)
#define PIN_ETH_MOSI 11
#define PIN_ETH_MISO 12
#define PIN_ETH_SCLK 13
#define PIN_ETH_CS 14
#define PIN_ETH_INT 10
#define PIN_ETH_RST 9

// SD Card (SEPARATE HSPI bus from W5500 FSPI)
// Waveshare ESP32-S3-ETH V1.0: SD on SPI2 (HSPI), W5500 on FSPI
// Waveshare official demo confirms: MISO=5, MOSI=6, SCLK=7, CS=4
// Separate bus from W5500 (which uses MOSI=11,MISO=12,SCLK=13)
#define PIN_SD_MOSI 6
#define PIN_SD_MISO 5
#define PIN_SD_SCLK 7
#define PIN_SD_CS 4
// SDIO 1-bit pin mapping (same physical slot, different signal names):
//   SD_CLK = GPIO7 (same as SPI SCLK)
//   SD_CMD = GPIO4 (same as SPI CS — NOT MOSI!)
//   SD_D0  = GPIO6 (same as SPI MOSI)
//   SD_D1  = GPIO5 (same as SPI MISO)
// ESP32-S3 requires ALL 6 pins via GPIO matrix (SOC_SDMMC_USE_GPIO_MATRIX=1)
// D2/D3 not wired to slot — use dummy available GPIOs
#define PIN_SDIO_CLK  7
#define PIN_SDIO_CMD  4
#define PIN_SDIO_D0   6
#define PIN_SDIO_D1   5
#define PIN_SDIO_D2   18   // NOT connected to slot (available GPIO)
#define PIN_SDIO_D3   17   // NOT connected to slot (available GPIO)

// ─── Modbus Configuration ──────────────────────────────────────
#define MODBUS_BAUD 9600
#define MODBUS_PARITY SERIAL_8N1
#define MODBUS_TIMEOUT_MS 1000
#define MODBUS_TIMEOUT_ONLINE 150  // Online module: fast response expected
#define MODBUS_TIMEOUT_OFFLINE 500 // Offline/rescan: more patient
#define MODBUS_POLL_MS 100
#define MODBUS_SCAN_START 1
#define MODBUS_SCAN_END 247   // Full Modbus address range
#define MODBUS_MAX_SLAVES 247 // Unlimited — up to full bus

// ─── Module Fixed Configuration ─────────────────────────────────
// Every module has exactly 6 DI and 6 Relay outputs
#define HA_V2_DI_COUNT 6
#define HA_V2_RELAY_COUNT 6

// ─── Modbus Device Profiles ──────────────────────────────────────
#define MB_PROFILE_CUSTOM 0
#define MB_PROFILE_KC868_HA 1 // KinCony KC868-HA V2 (6DI+6R)
#define MB_PROFILE_GENERIC 2  // Generic holding registers (configurable)

// ─── Modbus Register Map (KC868-HA V2) ────────────────────────
#define REG_MODEL_ID 0x0064
#define REG_FW_VERSION 0x0065
#define REG_SERIAL_LO 0x0100
#define REG_SERIAL_HI 0x0101
#define REG_DI_DO_STATUS 0x00C8 // DI bitmask (reg 0) + DO bitmask (reg 1)

// ─── Click Detection Timing ────────────────────────────────────
#define CLICK_SINGLE_MS 50
#define CLICK_DOUBLE_MS 350
#define CLICK_HOLD_MS 500
#define CLICK_MULTI_MS 200
#define CLICK_MAX_COUNT 5

// ─── Latch/Momentary Detection ────────────────────────────────
#define MOMENTARY_THRESHOLD_MS 1000  // Press <1s = momentary, >=1s = latch hold
#define LATCH_DETECT_MIN_SAMPLES 3   // Need 3 cycles before deciding
#define LATCH_DETECT_RATIO 70       // >70% momentary → momentary, else latch

// ─── MQTT Configuration ────────────────────────────────────────
#define MQTT_KEEPALIVE_S 60
#define MQTT_SOCKET_TIMEOUT 5000
#define MQTT_MAX_PACKET 2048
#define OTA_MAX_SIZE 3145728 // 3MB max firmware (app0+app1 partition 3MB each)
#define MQTT_QOS 1
#define MQTT_RETAIN_STATE true
#define MQTT_RECONNECT_MS 5000

// ─── Firmware Version ────────────────────────────────────────
#define FIRMWARE_VERSION "2.11.0" // Scan result API, register scan, FC05 coil write, storage nav fix, OTA_MAX_SIZE 3MB

// ─── TCP Modbus Bridge ─────────────────────────────────────────
#define TCP_PORT 502
#define TCP_MAX_CLIENTS 4

// ─── WiFi/AP Configuration ──────────────────────────────────────
#define WIFI_AP_SSID "ModbusMQTT-Setup"
#define WIFI_AP_PASS_DEFAULT "12345678" // default only — actual password stored in NVRAM (NV_KEY_AP_PASS)
#define WIFI_TIMEOUT_S 300

// ─── WiFi Mode ────────────────────────────────────────────────
// APSTA: AP always on + STA connects to router. Accessible both ways.
// STA only: No AP, STA only (save power, not recommended)
enum WiFiModeCfg : uint8_t
{
    WIFICFG_APSTA = 0,   // AP + STA — always reachable via AP
    WIFICFG_STA_ONLY = 1 // STA only — AP disabled
};

// ─── Network Priority ──────────────────────────────────────────
// LAN is always primary. WiFi is fallback only.
// Auto-fallback: LAN lost → WiFi takes over → LAN back → auto-switch back
enum NetInterface : uint8_t
{
    NET_IF_NONE = 0,
    NET_IF_LAN = 1,
    NET_IF_WIFI = 2
};

// ─── NVRAM Keys ────────────────────────────────────────────────
#define NV_NAMESPACE "mbmqtt2"
// WiFi
#define NV_KEY_WIFI_SSID "ssid"
#define NV_KEY_WIFI_PASS "wpass"
#define NV_KEY_AP_NAME "apn"
#define NV_KEY_AP_PASS "appass"
#define NV_KEY_WIFI_IP "wip"
#define NV_KEY_WIFI_GW "wgw"
#define NV_KEY_WIFI_MASK "wmask"
#define NV_KEY_WIFI_DNS "wdns"
#define NV_KEY_WIFI_DHCP "wdhcp"
// Ethernet
#define NV_KEY_ETH_EN "ethen"
#define NV_KEY_ETH_IP "eip"
#define NV_KEY_ETH_GW "egw"
#define NV_KEY_ETH_MASK "emask"
#define NV_KEY_ETH_DNS "edns"
#define NV_KEY_ETH_DHCP "edhcp"
#define NV_KEY_ETH_TYPE "etype" // 0=W5500, 1=LAN8720, 2=IP101
// Priority
#define NV_KEY_NET_PRIO "nprio"
// MQTT
#define NV_KEY_MQTT_HOST "mhost"
#define NV_KEY_MQTT_PORT "mport"
#define NV_KEY_MQTT_USER "muser"
#define NV_KEY_MQTT_PASS "mpass"
#define NV_KEY_MQTT_PREFIX "mpfx"
#define NV_KEY_MQTT_TLS "mtls"
// HA
#define NV_KEY_HA_DISC "hadisc" // HA auto-discovery be/ki
// Modbus
#define NV_KEY_MB_BAUD "mbaud"
#define NV_KEY_MB_START "mstart" // scan start addr
#define NV_KEY_MB_END "mend"     // scan end addr
#define NV_KEY_MB_PARITY "mpar"  // 0=8N1, 1=8E1, 2=8O1
// TCP
#define NV_KEY_TCP_EN "tcpen"
#define NV_KEY_TCP_PORT "tcpp"
#define NV_KEY_WIFI_MODE "wifimode"
#define NV_KEY_HOSTNAME "hostname"
// GPIO Pins
#define NV_KEY_PIN_RS485_RX "prx"
#define NV_KEY_PIN_RS485_TX "ptx"
#define NV_KEY_PIN_RS485_DE "pde"
#define NV_KEY_PIN_LED "pled"
#define NV_KEY_PIN_BTN "pbtn"
#define NV_KEY_PIN_ETH_MOSI "pemosi"
#define NV_KEY_PIN_ETH_MISO "pemiso"
#define NV_KEY_PIN_ETH_SCLK "pesclk"
#define NV_KEY_PIN_ETH_CS "pecs"
#define NV_KEY_PIN_ETH_INT "peint"
#define NV_KEY_PIN_ETH_RST "perst"
#define NV_KEY_PIN_SD_CS "psdcs"
#define NV_KEY_SD_EN "sdena"
#define NV_KEY_MB_PROFILE "mbprof"
#define NV_KEY_MB_REG_COIL "mbcoil"
#define NV_KEY_MB_REG_DI "mbdi"
#define NV_KEY_MB_POLL_MS "mbpoll"
#define NV_KEY_VIRTUAL_MOD "vmod"   // Virtual module for testing
#define NV_KEY_MOD_LIST_N "mlist_n" // Number of saved modules (0=none)
#define NV_KEY_MOD_ADDR "mlist_a"   // Saved slave address prefix: mlist_a0..15
#define NV_KEY_MOD_MODEL "mlist_m"  // Saved model_id prefix: mlist_m0..15
#define NV_KEY_MOD_DIRM "mlist_dr"  // DI→Relay map prefix: mlist_dr0..15 (blob[6])
#define NV_KEY_MOD_EDGE "mlist_ed" // DI→Edge map prefix: mlist_ed0..15 (blob[18]=6×3bytes)
#define MAX_SAVED_MODULES 16        // Max modules in saved list
#define NV_KEY_ROOMS "rooms"        // Custom room list (newline-separated)
#define MAX_ROOMS 30
// Web authentication
#define NV_KEY_WEB_AUTH "wauth"  // Enable web auth (bool)
#define NV_KEY_WEB_PASS "wauthp" // Auth password (NOT "wpass" — that's WiFi!)
// Module aliases (per slave_addr, keys "mn1".."mn247" and "hn1".."hn247")

// ─── Config Integrity (CRC + Version) ──────────────────────────
#define NV_KEY_CONFIG_CRC "ccrc"   // CRC32 of all config keys at save time
#define NV_KEY_CONFIG_VER "cver"   // Config schema version (increment on breaking change)
#define CONFIG_VERSION     2       // Current schema version

// ─── DI→Relay Edge Event Actions (v1.06 protocol) ───────────────
// Modeled after KinCony KC868-HA v1.06 EdgeEvent mode:
// Each DI can trigger a different action on rising and falling edge.
enum DIEdgeAction : uint8_t
{
    DI_EDGE_NONE = 0,  // NoDef — do nothing
    DI_EDGE_ON   = 1,  // Turn relay ON
    DI_EDGE_OFF  = 2,  // Turn relay OFF
    DI_EDGE_TOG  = 3   // Toggle relay
};

// ─── Per-DI edge action: target relay + rising/falling action
struct DI_EdgeAction
{
    uint8_t relay;              // Target relay index (0-5), or 0xFF = no mapping
    uint8_t rising_action;     // DI_EDGE_NONE/ON/OFF/TOG — action on press (rising edge)
    uint8_t falling_action;    // DI_EDGE_NONE/ON/OFF/TOG — action on release (falling edge)
};

// ─── Register Config (Modbus FC03/FC04 read + MQTT + HA discovery) ──
enum RegType : uint8_t
{
    REG_HOLDING = 3,   // FC03 Read Holding Registers
    REG_INPUT   = 4    // FC04 Read Input Registers
};

enum RegHAClass : uint8_t
{
    HAC_SENSOR,         // Generic sensor
    HAC_TEMPERATURE,    // Temperature °C
    HAC_HUMIDITY,       // Humidity %
    HAC_POWER,          // Power W
    HAC_ENERGY,         // Energy kWh
    HAC_PRESSURE,       // Pressure
    HAC_VOLTAGE,        // Voltage V
    HAC_CURRENT,        // Current A
    HAC_FREQUENCY,      // Frequency Hz
    HAC_COP,            // COP (dimensionless, 1-decimal)
};

struct RegisterConfig
{
    uint16_t addr;        // Modbus register address
    RegType  reg_type;    // FC03 or FC04
    RegHAClass ha_class;  // HA device_class mapping
    uint8_t  slave_addr;  // Which slave to read from
    uint16_t scale;       // Divisor for display (10 = divide by 10, 1 = raw)
    char     name[24];    // Friendly name (e.g. "Kültér hőmérséklet")
    char     unit[8];     // Unit string (e.g. "°C", "W", "kWh")
    bool     enabled;     // Enable/disable this register
    // Runtime (not saved to NVS)
    float    last_value;  // Last successfully read value
    bool     published;   // True after first MQTT publish
    uint32_t last_read_ms;// Last successful read timestamp
};

#define MAX_REGISTERS 32  // Max configurable registers

// NVS keys for register persistence
#define NV_KEY_REG_COUNT "regcnt"   // Number of saved registers
#define NV_KEY_REG_PREFIX "reg"    // reg0..reg31 binary blobs

// ─── Click event types ──────────────────────────────────────────
enum ClickType : uint8_t
{
    CLICK_NONE = 0,
    CLICK_SINGLE,
    CLICK_DOUBLE,
    CLICK_TRIPLE,
    CLICK_QUAD,
    CLICK_PENTA,
    CLICK_HOLD,
    CLICK_HOLD_RELEASE
};

// ─── Model identification ──────────────────────────────────────
struct HA_Model
{
    uint8_t model_id;
    uint16_t firmware_ver;
    uint32_t serial_number;
    char model_name[24];
    // Waveshare ESP32-S3-ETH V1.0: always 6 DI + 6 Relay (fixed, not read from registers)
    static constexpr uint8_t DI_COUNT = HA_V2_DI_COUNT;
    static constexpr uint8_t RELAY_COUNT = HA_V2_RELAY_COUNT;
};

// ─── DI Input Type (auto-detected) ──────────────────────────────
enum DIInputType : uint8_t
{
    DI_TYPE_UNKNOWN = 0,   // Not yet determined
    DI_TYPE_LATCH   = 1,   // Toggle/switch — stays ON or OFF (wall switch)
    DI_TYPE_MOMENTARY = 2  // Push-button — springs back (doorbell, momentary)
};

// ─── Digital Input state machine ───────────────────────────────
struct DI_State
{
    bool current;
    bool prev;
    uint32_t press_start_ms;
    uint32_t last_click_ms;
    uint8_t click_count;
    bool holding;
    ClickType pending;
    bool hold_fired;
    bool published; // State published to MQTT
    // ── Latch/Momentary auto-detection (v2.8+) ──
    DIInputType detected_type;    // Auto-detected: UNKNOWN→LATCH or MOMENTARY
    uint8_t sample_count;         // How many press-release cycles observed
    uint8_t momentary_votes;      // How many were short presses (<1s)
    uint32_t last_rising_ms;      // Timestamp of last rising edge
    uint32_t last_press_duration; // Duration of last completed press (ms)
};

// ─── Relay state ───────────────────────────────────────────────
struct Relay_State
{
    bool state;
    bool pending;
    uint32_t cmd_time_ms;
    bool published; // State published to MQTT
};

// ─── Complete module state ─────────────────────────────────────
struct Slave_Module
{
    uint8_t slave_addr;
    bool online;
    uint32_t last_seen_ms;
    uint8_t fail_count;
    HA_Model model;
    uint8_t profile;                       // Per-slave profile override (0 = use global)
    Relay_State relays[HA_V2_RELAY_COUNT]; // Always 6
    DI_State inputs[HA_V2_DI_COUNT];       // Always 6
    uint8_t di_relay_map[HA_V2_DI_COUNT];  // DI→Relay mapping (255 = none) [LEGACY — for NVRAM compat]
    DI_EdgeAction di_edge_map[HA_V2_DI_COUNT]; // DI→Relay Edge mapping (v2.8+ EdgeEvent)
    bool discovered;
    bool active;     // Slot in use
    bool is_virtual; // Virtual module (no physical hardware)
};

// ─── Global Config ─────────────────────────────────────────────────
struct AppConfig
{
    // WiFi (fallback when LAN is down)
    uint8_t wifi_mode; // 0=AP+STA (default), 1=STA only
    char wifi_ssid[64];
    char wifi_pass[64];
    char ap_name[33]; // Custom AP SSID (empty = hostname-MACHEX auto)
    char ap_pass[64]; // AP password (default: 12345678)
    bool wifi_dhcp;
    char wifi_ip[16];
    char wifi_gw[16];
    char wifi_mask[16];
    char wifi_dns[16];
    // Ethernet/LAN (always primary when available)
    bool lan_enabled;
    bool lan_dhcp;
    char lan_ip[16];
    char lan_gw[16];
    char lan_mask[16];
    char lan_dns[16];
    uint8_t lan_type; // 0=W5500 SPI, 1=LAN8720 RMII, 2=IP101 EMAC
    // Active interface tracking (runtime, not saved)
    NetInterface active_if; // NET_IF_LAN or NET_IF_WIFI
    // MQTT
    char mqtt_host[64];
    uint16_t mqtt_port;
    bool mqtt_tls; // Enable TLS (WiFiClientSecure)
    char mqtt_user[32];
    char mqtt_pass[64];
    char mqtt_prefix[32];
    // HA
    bool ha_discovery;
    // Modbus
    uint32_t mb_baud;
    uint8_t mb_scan_start;
    uint8_t mb_scan_end;
    uint8_t mb_parity;          // 0=8N1, 1=8E1, 2=8O1
    uint8_t mb_profile;         // Device profile (default: MB_PROFILE_KC868_HA)
    uint16_t mb_reg_coil_start; // Coil start register for GENERIC profile
    uint16_t mb_reg_di_start;   // DI start register for GENERIC profile
    uint16_t mb_poll_ms;        // Global poll interval in ms
    // Virtual module (for testing without physical hardware)
    bool virtual_module; // Enable virtual HA V2 module on address 200
    // TCP
    bool tcp_enabled;
    uint16_t tcp_port;
    // GPIO Pins (configurable)
    int8_t pin_rs485_rx;   // UART2 RX (default: 44)
    int8_t pin_rs485_tx;   // UART2 TX (default: 43)
    int8_t pin_rs485_de;   // RS485 DE/RE (default: 42, was 4 — conflicts with SD CS)
    int8_t pin_status_led; // Status LED (default: 2)
    int8_t pin_config_btn; // Config button (default: 0)
    int8_t pin_eth_mosi;   // W5500 SPI MOSI (default: 11)
    int8_t pin_eth_miso;   // W5500 SPI MISO (default: 12)
    int8_t pin_eth_sclk;   // W5500 SPI CLK (default: 13)
    int8_t pin_eth_cs;     // W5500 SPI CS  (default: 14)
    int8_t pin_eth_int;    // W5500 INT     (default: 10)
    int8_t pin_eth_rst;    // W5500 RST     (default: 9)
    // SD Card (separate HSPI bus from W5500!)
    bool sd_enabled;       // SD card feature enabled
    int8_t pin_sd_cs;      // SD Card CS (default: 4)
    // Device identity
    char hostname[32]; // mDNS + AP SSID suffix + MQTT client ID (default: "modbusmqtt")
    // Web authentication
    bool web_auth;     // Enable Digest Auth on write endpoints
    char web_pass[32]; // Auth password (empty = disabled)
};

extern AppConfig cfg;

// ─── Global State ──────────────────────────────────────────────
extern Slave_Module *modules; // Dynamic array, heap allocated
extern uint16_t module_count;
extern uint16_t modules_capacity;
extern bool scanning_done;
extern bool scan_active;
extern uint16_t scan_addr;

// ─── Scan Result Buffer ──────────────────────────────────────
#define MAX_SCAN_RESULTS 32
struct ScanResult {
    uint8_t addr;
    uint8_t model_id;
    uint16_t firmware_ver;
    char model_name[24];
    bool identified;    // true=identification read OK, false=just responded
};
extern ScanResult scan_results[MAX_SCAN_RESULTS];
extern uint8_t scan_result_count;

extern bool net_connected; // Any network interface connected
extern String active_ip;   // Current active IP address

// ─── Register Config State ──────────────────────────────────────
extern RegisterConfig registers[MAX_REGISTERS];
extern uint8_t register_count;

// ─── Modbus Statistics ─────────────────────────────────────────
struct ModbusStats
{
    uint32_t tx_count;
    uint32_t rx_count;
    uint32_t err_count;
    uint32_t last_err_time;  // millis()
    uint8_t last_err_code;   // ModbusMaster exception code
    uint8_t last_slave_addr; // last addressed slave
};
extern ModbusStats mb_stats;

// ─── Function Prototypes ───────────────────────────────────────
// DI→Relay edge action executor
void di_execute_edge_action(Slave_Module *mod, uint8_t di_idx, uint8_t action, bool is_virtual);
const char *di_edge_action_str(uint8_t action);
const char *di_input_type_str(uint8_t type);

// sd_handler.cpp
bool sd_init(int8_t cs_pin);                                  // SPI only (safe)
bool sd_init(int8_t cs_pin, const char *mode);        // "spi","sdio","auto"
void sd_deinit();
bool sd_is_ok();
bool sd_has_pin_conflict();
bool sd_is_sdio_mode();
bool sd_begin_exclusive();
void sd_end_exclusive();
bool sd_is_exclusive();
String sd_gpio_diag();
String sd_test_init();
uint64_t sd_total_kb();
uint64_t sd_used_kb();
const char *sd_type_str();
void sd_refresh_stats();
bool sd_save_register_list(const char *device_name, const char *json_content, size_t json_len);
char *sd_read_register_list(const char *device_name, size_t *out_len);
char *sd_list_register_files(size_t *out_len);
bool sd_delete_register_list(const char *device_name);
char *sd_browse_dir(const char *path, size_t *out_len);
bool sd_mkdir(const char *path);
bool sd_format();
bool sd_write_file(const char *path, const uint8_t *data, size_t len);
char *sd_read_file(const char *path, size_t *out_len);
bool sd_delete_path(const char *path);
bool sd_append_file(const char *path, const uint8_t *data, size_t len);
bool sd_file_exists(const char *path);
bool sd_remove_file(const char *path);

// led_handler.cpp
void led_init();
void led_set_color(uint8_t r, uint8_t g, uint8_t b);
void led_set_brightness(uint8_t percent);
void led_set_state(bool on);         // on/off toggle
bool led_is_on();
void led_get_color(uint8_t *r, uint8_t *g, uint8_t *b);
uint8_t led_get_brightness();
void led_publish_state();            // MQTT state publish
void led_setup_discovery();          // MQTT HA discovery
bool led_handle_command(const char *topic, const char *payload, size_t len);  // MQTT cmd

// modbus_handler.cpp
void modbus_init();
void modbus_set_timeout(uint16_t ms);
void modbus_scan_bus();
void scan_modbus_start();
void modbus_pause();
void modbus_resume();
bool modbus_is_paused();
bool modbus_read_identification(uint8_t slave, HA_Model &model);
bool modbus_read_discrete_inputs(uint8_t slave, uint8_t count, bool *states);
bool modbus_read_coils(uint8_t slave, uint8_t count, bool *states);
bool modbus_write_coil(uint8_t slave, uint8_t coil, bool state);
bool modbus_write_coils(uint8_t slave, uint8_t count, const bool *states);
bool modbus_read_di_do_combined(uint8_t slave, uint8_t di_count, uint8_t do_count, bool *di_states, bool *do_states);
bool modbus_read_register(uint8_t slave_addr, RegType reg_type, uint16_t reg_addr, uint16_t *value);
uint8_t modbus_fc16_write_registers(uint8_t slave_addr, uint16_t start_addr, uint16_t count, const uint16_t *values);
uint8_t effective_profile(Slave_Module *mod);
void modbus_set_timeout_for_module(bool online);

// TCP gateway raw Modbus access (for tcp_bridge.cpp)
struct ModbusRawResult
{
    uint8_t status;         // ModbusMaster result code
    uint16_t resp_len;      // Number of 16-bit words in response
    uint16_t resp_buf[125]; // Response data buffer
};
ModbusRawResult modbus_raw_request(uint8_t slave,
                                   uint8_t func,
                                   uint16_t start,
                                   uint16_t count_or_value,
                                   uint16_t count2 = 0,
                                   const uint16_t *values = nullptr);

// modbus_write.cpp
bool modbus_write_register(uint8_t slave_addr, uint16_t reg_addr, uint16_t value);
bool modbus_write_registers(uint8_t slave_addr, uint16_t start_addr, uint16_t count, const uint16_t *values);
bool modbus_write_slave_id(uint8_t slave_addr, uint16_t id_register_addr, uint8_t new_id);

// storage_handler.cpp  (-DUSE_STORAGE)
#ifdef USE_STORAGE
bool storage_init();
bool storage_list_dir(const char *path, String &json_output);
bool storage_read_file(const char *path, String &content);
bool storage_write_file(const char *path, const char *content, size_t len);
bool storage_restore_pins();
bool storage_delete_file(const char *path);
bool storage_exists(const char *path);
uint64_t storage_total_bytes();
uint64_t storage_used_bytes();
#endif

void module_add_from_scan(uint8_t addr, uint8_t model_id);
void module_add_virtual();
void modbus_poll_loop();
void modbus_stats_reset();

// click_detector.cpp
void click_init(DI_State *di, uint8_t count);
void click_update(DI_State *di, uint8_t idx, bool physical_state);
ClickType click_get_event(DI_State *di, uint8_t idx);
const char *click_type_str(ClickType ct);
typedef void (*ClickCallback)(uint8_t di_idx, uint8_t click_type);
void click_set_callback(ClickCallback cb);

// mqtt_handler.cpp
void mqtt_init();
void mqtt_loop();
bool mqtt_is_connected();
bool mqtt_publish_topic(const char *topic, const char *payload, bool retained = false);
bool mqtt_is_on_lan();
void mqtt_force_disconnect();
void mqtt_cleanup_discovery(Slave_Module *mod);
void mqtt_publish_discovery(Slave_Module *mod);
void mqtt_publish_relay_state(Slave_Module *mod, uint8_t relay_idx);
void mqtt_publish_di_state(Slave_Module *mod, uint8_t di_idx, bool state);
void mqtt_publish_click_event(Slave_Module *mod, uint8_t di_idx, ClickType ct);
void mqtt_publish_module_online(Slave_Module *mod, bool online);
void mqtt_subscribe_commands(Slave_Module *mod);
void mqtt_publish_stats();
void mqtt_publish_bridge_discovery();
void mqtt_publish_bridge_state();
void mqtt_set_click_module(Slave_Module *mod);
void mqtt_publish_register_value(RegisterConfig *reg);
void mqtt_publish_register_discovery(RegisterConfig *reg);

// tcp_bridge.cpp
void tcp_init();
void tcp_loop();
extern volatile bool bus_busy; // TCP bridge bus mutex flag
uint32_t tcp_get_req_count();
uint32_t tcp_get_err_count();

// eth_handler.cpp
void eth_init();
void eth_loop();
void w5500_reinit();
bool eth_is_connected();
bool eth_is_started();
void eth_set_connected(bool v);
void eth_set_started(bool v);
String eth_get_ip();
void eth_hard_reset_and_restart();
void eth_web_loop();

// web_server.cpp
void web_server_init();
void web_server_loop();
bool web_auth_ok(); // Check web auth, return true if OK or auth disabled

// config.cpp
void config_init();
void config_load();
void config_save();
bool config_has_wifi();
bool config_has_mqtt();
void config_start_portal();
void config_print();
uint32_t config_compute_crc();          // CRC32 of all core config key-value pairs
bool config_validate();                 // Return true if CRC matches, false = corrupt
void config_write_crc();                // Compute + save CRC after config_save()
void config_load_module_names();
void config_save_module_name(uint8_t slave_addr, const char *mqtt_name, const char *ha_name);
void config_save_module_list();    // Save current module addresses to NVRAM
uint8_t config_load_module_list(); // Load saved modules, return count (0=none)
void config_clear_module_list();   // Delete saved module list from NVRAM
String config_get_mqtt_name(uint8_t slave_addr);
String config_get_ha_name(uint8_t slave_addr);
void config_save_module_area(uint8_t slave_addr, const char *area);
String config_get_module_area(uint8_t slave_addr);
void config_save_relay_name(uint8_t slave_addr, uint8_t idx, const char *name);
String config_get_relay_name(uint8_t slave_addr, uint8_t idx);
void config_save_di_name(uint8_t slave_addr, uint8_t idx, const char *name);
String config_get_di_name(uint8_t slave_addr, uint8_t idx);
void config_save_registers();
void config_load_registers();

// ota_handler.cpp
void ota_init();
void ota_loop();

// watchdog.cpp
void wdt_init();
void wdt_check();
void wdt_loop_tick_reset();
void wdt_notify_publish();
void wdt_reboot(const char *reason);
uint32_t wdt_get_reboots();

// ws_handler.cpp
void ws_init();
void ws_loop();
void ws_notify_relay(Slave_Module *mod, uint8_t relay_idx);
void ws_notify_di(Slave_Module *mod, uint8_t di_idx, bool state);
void ws_notify_mqtt(bool connected);
uint8_t ws_client_count_get();

// main.cpp
void setup();
void loop();

// ─── PSRAM Helpers ──────────────────────────────────────────────
// ESP32-S3 has 8MB OPI PSRAM — use for large buffers (discovery JSON, WS payloads)
inline size_t psram_free() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
inline size_t psram_total() { return heap_caps_get_total_size(MALLOC_CAP_SPIRAM); }
inline bool psram_available() { return psram_total() > 0; }
// Allocate in PSRAM if available, fall back to regular heap
inline void *psram_malloc(size_t size)
{
    if (psram_available())
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return malloc(size);
}

// ─── PSRAM Allocator for ArduinoJson ────────────────────────────
// Implements ArduinoJson::Allocator interface — allocate from PSRAM first,
// fall back to regular heap. Use for large JsonDocument instances (discovery,
// heartbeat, backup, modules API).
class PsramAllocator : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t size) override {
    if (psram_available())
      return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return malloc(size);
  }

  void deallocate(void* ptr) override {
    heap_caps_free(ptr);
  }

  void* reallocate(void* ptr, size_t new_size) override {
    if (psram_available())
      return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
    return realloc(ptr, new_size);
  }

  static PsramAllocator* instance() {
    static PsramAllocator allocator;
    return &allocator;
  }

 private:
  PsramAllocator() = default;
  ~PsramAllocator() = default;
};

#endif