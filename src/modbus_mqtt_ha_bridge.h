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
#define PIN_RS485_DE 4   // RS485 Driver Enable (DE/RE)
#define PIN_STATUS_LED 2 // Built-in LED
#define PIN_CONFIG_BTN 0 // BOOT button — hold = WiFi config portal

// W5500 SPI Ethernet pins (adjust for your board)
#define PIN_ETH_MOSI 11
#define PIN_ETH_MISO 13
#define PIN_ETH_SCLK 12
#define PIN_ETH_CS 10
#define PIN_ETH_INT 14
#define PIN_ETH_RST 9

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

// ─── MQTT Configuration ────────────────────────────────────────
#define MQTT_KEEPALIVE_S 60
#define MQTT_SOCKET_TIMEOUT 5000
#define MQTT_MAX_PACKET 2048
#define OTA_MAX_SIZE 1310720 // 1.25MB max firmware (app0 partition 1280KB - margin)
#define MQTT_QOS 1
#define MQTT_RETAIN_STATE true
#define MQTT_RECONNECT_MS 5000

// ─── Firmware Version ────────────────────────────────────────
#define FIRMWARE_VERSION "2.7.1" // 16MB partition table, 3MB app slots

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
#define NV_KEY_MB_PROFILE "mbprof"
#define NV_KEY_MB_REG_COIL "mbcoil"
#define NV_KEY_MB_REG_DI "mbdi"
#define NV_KEY_MB_POLL_MS "mbpoll"
#define NV_KEY_VIRTUAL_MOD "vmod"   // Virtual module for testing
#define NV_KEY_MOD_LIST_N "mlist_n" // Number of saved modules (0=none)
#define NV_KEY_MOD_ADDR "mlist_a"   // Saved slave address prefix: mlist_a0..15
#define NV_KEY_MOD_MODEL "mlist_m"  // Saved model_id prefix: mlist_m0..15
#define NV_KEY_MOD_DIRM "mlist_dr"  // DI→Relay map prefix: mlist_dr0..15 (blob[6])
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
#define CONFIG_VERSION     1       // Current schema version

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
    uint8_t di_relay_map[HA_V2_DI_COUNT];  // DI→Relay mapping (255 = none)
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
    int8_t pin_rs485_de;   // RS485 DE/RE (default: 4)
    int8_t pin_status_led; // Status LED (default: 2)
    int8_t pin_config_btn; // Config button (default: 0)
    int8_t pin_eth_mosi;   // W5500 SPI MOSI (default: 11)
    int8_t pin_eth_miso;   // W5500 SPI MISO (default: 13)
    int8_t pin_eth_sclk;   // W5500 SPI CLK (default: 12)
    int8_t pin_eth_cs;     // W5500 SPI CS (default: 10)
    int8_t pin_eth_int;    // W5500 INT (default: 14)
    int8_t pin_eth_rst;    // W5500 RST (default: 9)
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
extern bool net_connected; // Any network interface connected
extern String active_ip;   // Current active IP address

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
// modbus_handler.cpp
void modbus_init();
void modbus_set_timeout(uint16_t ms);
void modbus_scan_bus();
void scan_modbus_start();
bool modbus_read_identification(uint8_t slave, HA_Model &model);
bool modbus_read_discrete_inputs(uint8_t slave, uint8_t count, bool *states);
bool modbus_read_coils(uint8_t slave, uint8_t count, bool *states);
bool modbus_write_coil(uint8_t slave, uint8_t coil, bool state);
bool modbus_write_coils(uint8_t slave, uint8_t count, const bool *states);
bool modbus_read_di_do_combined(uint8_t slave, uint8_t di_count, uint8_t do_count, bool *di_states, bool *do_states);
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

// tcp_bridge.cpp
void tcp_init();
void tcp_loop();
extern volatile bool bus_busy; // TCP bridge bus mutex flag
uint32_t tcp_get_req_count();
uint32_t tcp_get_err_count();

// eth_handler.cpp
void eth_init();
void eth_loop();
bool eth_is_connected();
bool eth_is_started();
void eth_set_connected(bool v);
void eth_set_started(bool v);
String eth_get_ip();
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