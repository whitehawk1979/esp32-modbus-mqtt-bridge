/**
 * config.cpp — Web Configuration Portal v2.0
 * 
 * LAN is always primary. WiFi is fallback.
 * Waveshare ESP32-S3-ETH V1.0: fixed 6DI + 6R per module.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include "modbus_mqtt_ha_bridge.h"

static WebServer portal(8080);
static DNSServer dns;
static bool portal_active = false;

// ─── Global Config ─────────────────────────────────────────────
AppConfig cfg;

// ─── Load defaults ─────────────────────────────────────────────
static void cfg_defaults() {
    memset(&cfg, 0, sizeof(cfg));
    cfg.wifi_mode = 0;       // AP+STA default
    cfg.wifi_dhcp = true;
    cfg.lan_enabled = false;
    cfg.lan_dhcp = true;
    cfg.lan_type = 0;
    cfg.active_if = NET_IF_NONE;
    strlcpy(cfg.mqtt_host, "192.168.1.43", sizeof(cfg.mqtt_host));
    cfg.mqtt_port = 1883;
    strlcpy(cfg.mqtt_prefix, "modbusmqtt", sizeof(cfg.mqtt_prefix));
    cfg.ha_discovery = true;
    cfg.mb_baud = 9600;
    cfg.mb_scan_start = 1;
    cfg.mb_scan_end = 247;
    cfg.mb_parity = 0;
    cfg.mb_profile = MB_PROFILE_KC868_HA;
    cfg.mb_reg_coil_start = 0;
    cfg.mb_reg_di_start = 1;
    cfg.mb_poll_ms = 500;
    cfg.tcp_enabled = true;
    cfg.tcp_port = 502;
    // GPIO Pin defaults
    cfg.pin_rs485_rx   = 44;
    cfg.pin_rs485_tx   = 43;
    cfg.pin_rs485_de   = 4;
    cfg.pin_status_led = 2;
    cfg.pin_config_btn = 0;
    cfg.pin_eth_mosi   = 11;
    cfg.pin_eth_miso   = 13;
    cfg.pin_eth_sclk   = 12;
    cfg.pin_eth_cs     = 10;
    cfg.pin_eth_int    = 14;
    cfg.pin_eth_rst    = 9;
    strlcpy(cfg.hostname, "modbusmqtt", sizeof(cfg.hostname));
}

void config_init() { cfg_defaults(); }

// ── Factory defaults: auto-provision NVRAM on first boot after erase ──
// After erase_flash every NVRAM key is empty. We populate factory values
// so the device boots fully configured (WiFi+LAN+MQTT+modules+auth+pins).
static void factory_provision(Preferences &nv) {
    bool dirty = false;

    // ── WiFi ──
    char buf[64];
    nv.getString(NV_KEY_WIFI_SSID, buf, sizeof(buf));
    if (strlen(buf) == 0) { nv.putString(NV_KEY_WIFI_SSID, "Air 2"); dirty = true; }
    nv.getString(NV_KEY_WIFI_PASS, buf, sizeof(buf));
    if (strlen(buf) == 0) { nv.putString(NV_KEY_WIFI_PASS, "decembertizenhat"); dirty = true; }
    if (!nv.isKey(NV_KEY_WIFI_MODE)) { nv.putUChar(NV_KEY_WIFI_MODE, 0); dirty = true; }
    if (!nv.isKey(NV_KEY_WIFI_DHCP)) { nv.putBool(NV_KEY_WIFI_DHCP, true); dirty = true; }

    // ── LAN (W5500 enabled by default) ──
    if (!nv.isKey(NV_KEY_ETH_EN)) { nv.putBool(NV_KEY_ETH_EN, true); dirty = true; }
    if (!nv.isKey(NV_KEY_ETH_DHCP)) { nv.putBool(NV_KEY_ETH_DHCP, true); dirty = true; }
    if (!nv.isKey(NV_KEY_ETH_TYPE)) { nv.putUChar(NV_KEY_ETH_TYPE, 1); dirty = true; }  // W5500

    // ── MQTT ──
    nv.getString(NV_KEY_MQTT_HOST, buf, sizeof(buf));
    if (strlen(buf) == 0) { nv.putString(NV_KEY_MQTT_HOST, "192.168.1.43"); dirty = true; }
    if (!nv.isKey(NV_KEY_MQTT_PORT)) { nv.putUShort(NV_KEY_MQTT_PORT, 1883); dirty = true; }
    nv.getString(NV_KEY_MQTT_USER, buf, sizeof(buf));
    if (strlen(buf) == 0) { nv.putString(NV_KEY_MQTT_USER, "mqtt"); dirty = true; }
    nv.getString(NV_KEY_MQTT_PASS, buf, sizeof(buf));
    if (strlen(buf) == 0) { nv.putString(NV_KEY_MQTT_PASS, "2009December16"); dirty = true; }
    nv.getString(NV_KEY_MQTT_PREFIX, buf, sizeof(buf));
    if (strlen(buf) == 0) { nv.putString(NV_KEY_MQTT_PREFIX, "modbusmqtt"); dirty = true; }
    if (!nv.isKey(NV_KEY_MQTT_TLS)) { nv.putBool(NV_KEY_MQTT_TLS, false); dirty = true; }

    // ── HA Discovery ──
    if (!nv.isKey(NV_KEY_HA_DISC)) { nv.putBool(NV_KEY_HA_DISC, true); dirty = true; }

    // ── Modbus ──
    if (!nv.isKey(NV_KEY_MB_BAUD))    { nv.putUInt(NV_KEY_MB_BAUD, 9600); dirty = true; }
    if (!nv.isKey(NV_KEY_MB_START))   { nv.putUChar(NV_KEY_MB_START, 1); dirty = true; }
    if (!nv.isKey(NV_KEY_MB_END))     { nv.putUChar(NV_KEY_MB_END, 247); dirty = true; }
    if (!nv.isKey(NV_KEY_MB_PARITY))  { nv.putUChar(NV_KEY_MB_PARITY, 0); dirty = true; }
    if (!nv.isKey(NV_KEY_MB_PROFILE)) { nv.putUChar(NV_KEY_MB_PROFILE, MB_PROFILE_KC868_HA); dirty = true; }
    if (!nv.isKey(NV_KEY_MB_REG_COIL)) { nv.putUShort(NV_KEY_MB_REG_COIL, 0); dirty = true; }
    if (!nv.isKey(NV_KEY_MB_REG_DI))  { nv.putUShort(NV_KEY_MB_REG_DI, 1); dirty = true; }
    if (!nv.isKey(NV_KEY_MB_POLL_MS)) { nv.putUShort(NV_KEY_MB_POLL_MS, 500); dirty = true; }
    if (!nv.isKey(NV_KEY_VIRTUAL_MOD)) { nv.putBool(NV_KEY_VIRTUAL_MOD, false); dirty = true; }

    // ── TCP Modbus bridge ──
    if (!nv.isKey(NV_KEY_TCP_EN))   { nv.putBool(NV_KEY_TCP_EN, true); dirty = true; }
    if (!nv.isKey(NV_KEY_TCP_PORT)) { nv.putUShort(NV_KEY_TCP_PORT, 502); dirty = true; }

    // ── Hostname ──
    nv.getString(NV_KEY_HOSTNAME, buf, sizeof(buf));
    if (strlen(buf) == 0) { nv.putString(NV_KEY_HOSTNAME, "modbusmqtt"); dirty = true; }

    // ── Web Auth ──
    if (!nv.isKey(NV_KEY_WEB_AUTH)) { nv.putBool(NV_KEY_WEB_AUTH, true); dirty = true; }
    nv.getString(NV_KEY_WEB_PASS, buf, sizeof(buf));
    if (strlen(buf) == 0) { nv.putString(NV_KEY_WEB_PASS, "admin"); dirty = true; }

    // ── KinCony F16 module (addr=200) ──
    if (!nv.isKey("mlist_n"))  { nv.putString("mlist_n", "1"); dirty = true; }
    nv.getString("mn200", buf, sizeof(buf));
    if (strlen(buf) == 0) { nv.putString("mn200", "KinCony F16"); dirty = true; }
    nv.getString("hn200", buf, sizeof(buf));
    if (strlen(buf) == 0) { nv.putString("hn200", "KinCony F16"); dirty = true; }
    nv.getString("ar200", buf, sizeof(buf));
    if (strlen(buf) == 0) { nv.putString("ar200", "Gepeszet"); dirty = true; }
    // Relay names
    const char* relay_names[] = {"Alagsor","Utofutes","Foldszint","Ventilokonvektorok","Manzard","MV recirk"};
    for (int i = 0; i < 6; i++) {
        char key[16]; snprintf(key, sizeof(key), "rn200_%d", i);
        nv.getString(key, buf, sizeof(buf));
        if (strlen(buf) == 0) { nv.putString(key, relay_names[i]); dirty = true; }
    }
    // DI names
    const char* di_names[] = {"Kapcsolo 1","Kapcsolo 2","Kapcsolo 3","Kapcsolo 4","Kapcsolo 5","Kapcsolo 6"};
    for (int i = 0; i < 6; i++) {
        char key[16]; snprintf(key, sizeof(key), "dn200_%d", i);
        nv.getString(key, buf, sizeof(buf));
        if (strlen(buf) == 0) { nv.putString(key, di_names[i]); dirty = true; }
    }
    // Rooms list
    nv.getString("rooms", buf, sizeof(buf));
    if (strlen(buf) == 0) { nv.putString("rooms", "Gepeszet"); dirty = true; }

    // ── GPIO Pins (Waveshare ESP32-S3-ETH V1.0 = W5500) ──
    if (!nv.isKey(NV_KEY_PIN_RS485_RX))   { nv.putInt(NV_KEY_PIN_RS485_RX, 44); dirty = true; }
    if (!nv.isKey(NV_KEY_PIN_RS485_TX))   { nv.putInt(NV_KEY_PIN_RS485_TX, 43); dirty = true; }
    if (!nv.isKey(NV_KEY_PIN_RS485_DE))   { nv.putInt(NV_KEY_PIN_RS485_DE, 4); dirty = true; }
    if (!nv.isKey(NV_KEY_PIN_LED))        { nv.putInt(NV_KEY_PIN_LED, 2); dirty = true; }
    if (!nv.isKey(NV_KEY_PIN_BTN))        { nv.putInt(NV_KEY_PIN_BTN, 0); dirty = true; }
    if (!nv.isKey(NV_KEY_PIN_ETH_MOSI))   { nv.putInt(NV_KEY_PIN_ETH_MOSI, 11); dirty = true; }
    if (!nv.isKey(NV_KEY_PIN_ETH_MISO))   { nv.putInt(NV_KEY_PIN_ETH_MISO, 13); dirty = true; }
    if (!nv.isKey(NV_KEY_PIN_ETH_SCLK))   { nv.putInt(NV_KEY_PIN_ETH_SCLK, 12); dirty = true; }
    if (!nv.isKey(NV_KEY_PIN_ETH_CS))     { nv.putInt(NV_KEY_PIN_ETH_CS, 10); dirty = true; }
    if (!nv.isKey(NV_KEY_PIN_ETH_INT))    { nv.putInt(NV_KEY_PIN_ETH_INT, 14); dirty = true; }
    if (!nv.isKey(NV_KEY_PIN_ETH_RST))    { nv.putInt(NV_KEY_PIN_ETH_RST, 9); dirty = true; }

    if (dirty) Serial.println("[NV] Factory defaults provisioned");
}

void config_load() {
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);  // RW mode for factory provisioning
    
    // Auto-provision factory defaults if NVRAM empty (e.g. after erase_flash)
    factory_provision(nv);
    
    // ── Read all values from NVRAM ──
    nv.getString(NV_KEY_WIFI_SSID, cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    nv.getString(NV_KEY_WIFI_PASS, cfg.wifi_pass, sizeof(cfg.wifi_pass));
    nv.getString(NV_KEY_AP_NAME, cfg.ap_name, sizeof(cfg.ap_name));
    cfg.wifi_mode = nv.getUChar(NV_KEY_WIFI_MODE, 0);
    cfg.wifi_dhcp = nv.getBool(NV_KEY_WIFI_DHCP, true);
    nv.getString(NV_KEY_WIFI_IP, cfg.wifi_ip, sizeof(cfg.wifi_ip));
    nv.getString(NV_KEY_WIFI_GW, cfg.wifi_gw, sizeof(cfg.wifi_gw));
    nv.getString(NV_KEY_WIFI_MASK, cfg.wifi_mask, sizeof(cfg.wifi_mask));
    nv.getString(NV_KEY_WIFI_DNS, cfg.wifi_dns, sizeof(cfg.wifi_dns));
    
    cfg.lan_enabled = nv.getBool(NV_KEY_ETH_EN, true);   // default ON
    cfg.lan_dhcp = nv.getBool(NV_KEY_ETH_DHCP, true);
    nv.getString(NV_KEY_ETH_IP, cfg.lan_ip, sizeof(cfg.lan_ip));
    nv.getString(NV_KEY_ETH_GW, cfg.lan_gw, sizeof(cfg.lan_gw));
    nv.getString(NV_KEY_ETH_MASK, cfg.lan_mask, sizeof(cfg.lan_mask));
    nv.getString(NV_KEY_ETH_DNS, cfg.lan_dns, sizeof(cfg.lan_dns));
    cfg.lan_type = nv.getUChar(NV_KEY_ETH_TYPE, 1);     // default W5500
    
    nv.getString(NV_KEY_MQTT_HOST, cfg.mqtt_host, sizeof(cfg.mqtt_host));
    cfg.mqtt_port = nv.getUShort(NV_KEY_MQTT_PORT, 1883);
    nv.getString(NV_KEY_MQTT_USER, cfg.mqtt_user, sizeof(cfg.mqtt_user));
    nv.getString(NV_KEY_MQTT_PASS, cfg.mqtt_pass, sizeof(cfg.mqtt_pass));
    nv.getString(NV_KEY_MQTT_PREFIX, cfg.mqtt_prefix, sizeof(cfg.mqtt_prefix));
    cfg.mqtt_tls = nv.getBool(NV_KEY_MQTT_TLS, false);
    
    cfg.ha_discovery = nv.getBool(NV_KEY_HA_DISC, true);
    
    cfg.mb_baud = nv.getUInt(NV_KEY_MB_BAUD, 9600);
    cfg.mb_scan_start = nv.getUChar(NV_KEY_MB_START, 1);
    cfg.mb_scan_end = nv.getUChar(NV_KEY_MB_END, 247);
    cfg.mb_parity = nv.getUChar(NV_KEY_MB_PARITY, 0);
    cfg.mb_profile = nv.getUChar(NV_KEY_MB_PROFILE, MB_PROFILE_KC868_HA);
    cfg.mb_reg_coil_start = nv.getUShort(NV_KEY_MB_REG_COIL, 0);
    cfg.mb_reg_di_start = nv.getUShort(NV_KEY_MB_REG_DI, 1);
    cfg.mb_poll_ms = nv.getUShort(NV_KEY_MB_POLL_MS, 500);
    cfg.virtual_module = nv.getBool(NV_KEY_VIRTUAL_MOD, false);
    
    cfg.tcp_enabled = nv.getBool(NV_KEY_TCP_EN, true);
    cfg.tcp_port = nv.getUShort(NV_KEY_TCP_PORT, 502);
    
    cfg.pin_rs485_rx   = (int8_t)nv.getInt(NV_KEY_PIN_RS485_RX, 44);
    cfg.pin_rs485_tx   = (int8_t)nv.getInt(NV_KEY_PIN_RS485_TX, 43);
    cfg.pin_rs485_de   = (int8_t)nv.getInt(NV_KEY_PIN_RS485_DE, 4);
    cfg.pin_status_led = (int8_t)nv.getInt(NV_KEY_PIN_LED, 2);
    cfg.pin_config_btn = (int8_t)nv.getInt(NV_KEY_PIN_BTN, 0);
    cfg.pin_eth_mosi   = (int8_t)nv.getInt(NV_KEY_PIN_ETH_MOSI, 11);
    cfg.pin_eth_miso   = (int8_t)nv.getInt(NV_KEY_PIN_ETH_MISO, 13);
    cfg.pin_eth_sclk   = (int8_t)nv.getInt(NV_KEY_PIN_ETH_SCLK, 12);
    cfg.pin_eth_cs     = (int8_t)nv.getInt(NV_KEY_PIN_ETH_CS, 10);
    cfg.pin_eth_int    = (int8_t)nv.getInt(NV_KEY_PIN_ETH_INT, 14);
    cfg.pin_eth_rst    = (int8_t)nv.getInt(NV_KEY_PIN_ETH_RST, 9);
    nv.getString(NV_KEY_HOSTNAME, cfg.hostname, sizeof(cfg.hostname));
    
    cfg.web_auth = nv.getBool(NV_KEY_WEB_AUTH, true);
    nv.getString(NV_KEY_WEB_PASS, cfg.web_pass, sizeof(cfg.web_pass));
    
    nv.end();
    config_print();
}

void config_save() {
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    
    nv.putString(NV_KEY_WIFI_SSID, portal.arg("ssid"));
    nv.putString(NV_KEY_WIFI_PASS, portal.arg("wpass"));
    nv.putUChar(NV_KEY_WIFI_MODE, portal.arg("wifimode").toInt());
    nv.putBool(NV_KEY_WIFI_DHCP, portal.arg("wdhcp") == "1");
    nv.putString(NV_KEY_WIFI_IP, portal.arg("wip"));
    nv.putString(NV_KEY_WIFI_GW, portal.arg("wgw"));
    nv.putString(NV_KEY_WIFI_MASK, portal.arg("wmask"));
    nv.putString(NV_KEY_WIFI_DNS, portal.arg("wdns"));
    
    nv.putBool(NV_KEY_ETH_EN, portal.arg("lanen") == "1");
    nv.putBool(NV_KEY_ETH_DHCP, portal.arg("landhcp") == "1");
    nv.putString(NV_KEY_ETH_IP, portal.arg("lanip"));
    nv.putString(NV_KEY_ETH_GW, portal.arg("langw"));
    nv.putString(NV_KEY_ETH_MASK, portal.arg("lanmask"));
    nv.putString(NV_KEY_ETH_DNS, portal.arg("landns"));
    nv.putUChar(NV_KEY_ETH_TYPE, portal.arg("lantype").toInt());
    
    nv.putString(NV_KEY_MQTT_HOST, portal.arg("mhost"));
    nv.putUShort(NV_KEY_MQTT_PORT, portal.arg("mport").toInt());
    nv.putString(NV_KEY_MQTT_USER, portal.arg("muser"));
    nv.putString(NV_KEY_MQTT_PASS, portal.arg("mpass"));
    nv.putString(NV_KEY_MQTT_PREFIX, portal.arg("mpfx"));
    
    nv.putBool(NV_KEY_HA_DISC, portal.arg("hadisc") == "1");
    
    nv.putUInt(NV_KEY_MB_BAUD, portal.arg("mbaud").toInt());
    nv.putUChar(NV_KEY_MB_START, portal.arg("mstart").toInt());
    nv.putUChar(NV_KEY_MB_END, portal.arg("mend").toInt());
    nv.putUChar(NV_KEY_MB_PARITY, portal.arg("mpar").toInt());
    nv.putUChar(NV_KEY_MB_PROFILE, portal.arg("mbprof").toInt());
    nv.putUShort(NV_KEY_MB_REG_COIL, portal.arg("mbcoil").toInt());
    nv.putUShort(NV_KEY_MB_REG_DI, portal.arg("mbdi").toInt());
    nv.putUShort(NV_KEY_MB_POLL_MS, portal.arg("mbpoll").toInt());
    nv.putBool(NV_KEY_VIRTUAL_MOD, portal.arg("vmod") == "1");
    
    nv.putBool(NV_KEY_TCP_EN, portal.arg("tcpen") == "1");
    nv.putUShort(NV_KEY_TCP_PORT, portal.arg("tcpp").toInt());
    
    // GPIO Pins
    nv.putInt(NV_KEY_PIN_RS485_RX, portal.arg("prx").toInt());
    nv.putInt(NV_KEY_PIN_RS485_TX, portal.arg("ptx").toInt());
    nv.putInt(NV_KEY_PIN_RS485_DE, portal.arg("pde").toInt());
    nv.putInt(NV_KEY_PIN_LED, portal.arg("pled").toInt());
    nv.putInt(NV_KEY_PIN_BTN, portal.arg("pbtn").toInt());
    nv.putInt(NV_KEY_PIN_ETH_MOSI, portal.arg("pemosi").toInt());
    nv.putInt(NV_KEY_PIN_ETH_MISO, portal.arg("pemiso").toInt());
    nv.putInt(NV_KEY_PIN_ETH_SCLK, portal.arg("pesclk").toInt());
    nv.putInt(NV_KEY_PIN_ETH_CS, portal.arg("pecs").toInt());
    nv.putInt(NV_KEY_PIN_ETH_INT, portal.arg("peint").toInt());
    nv.putInt(NV_KEY_PIN_ETH_RST, portal.arg("perst").toInt());
    nv.putString(NV_KEY_HOSTNAME, portal.arg("hostname"));
    
    nv.end();
    LOG_ILN("[CONFIG] Saved all settings");
}

void config_print() {
    LOG_ILN("\n╔══════════════════════════════════════╗");
    LOG_ILN("║  Modbus-MQTT Bridge v2.0 Config       ║");
    LOG_ILN("╚══════════════════════════════════════╝");
    LOG_I("  LAN:   Enabled=%s Type=%s DHCP=%s IP=%s\n", 
        cfg.lan_enabled?"Yes":"No", cfg.lan_type==0?"W5500":cfg.lan_type==1?"LAN8720":"IP101", cfg.lan_dhcp?"Yes":"No", cfg.lan_ip);
    LOG_I("  WiFi:  SSID=%s AP=%s DHCP=%s IP=%s (fallback)\n", cfg.wifi_ssid, cfg.ap_name[0]?cfg.ap_name:"auto", cfg.wifi_dhcp?"Yes":"No", cfg.wifi_ip);
    LOG_I("  MQTT:  %s:%d prefix=%s\n", cfg.mqtt_host, cfg.mqtt_port, cfg.mqtt_prefix);
    LOG_I("  HA:    Discovery=%s\n", cfg.ha_discovery?"ON":"OFF");
    LOG_I("  MB:    Baud=%lu Start=%d End=%d Parity=%d Profile=%d Poll=%dms\n", 
        cfg.mb_baud, cfg.mb_scan_start, cfg.mb_scan_end, cfg.mb_parity, cfg.mb_profile, cfg.mb_poll_ms);
    LOG_I("  MB:    Virtual module=%s\n", cfg.virtual_module?"ON (addr 200)":"OFF");
    if (cfg.mb_profile == MB_PROFILE_GENERIC) {
        LOG_I("  MB:    Coil start=%d DI start=%d\n", cfg.mb_reg_coil_start, cfg.mb_reg_di_start);
    }
    LOG_I("  TCP:   Enabled=%s Port=%d\n", cfg.tcp_enabled?"Yes":"No", cfg.tcp_port);
    LOG_I("  HA V2: 6DI + 6R per module (fixed)\n");
    LOG_ILN("");
}

bool config_has_wifi() { return strlen(cfg.wifi_ssid) > 0; }
bool config_has_mqtt() { return strlen(cfg.mqtt_host) > 0; }

// ─── Module Name Persistence (per slave address) ────────────────
void config_save_module_name(uint8_t slave_addr, const char *mqtt_name, const char *ha_name) {
    if (slave_addr < 1 || slave_addr > 247) return;
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    char key[8];
    snprintf(key, sizeof(key), "mn%u", slave_addr);
    nv.putString(key, String(mqtt_name));
    snprintf(key, sizeof(key), "hn%u", slave_addr);
    nv.putString(key, String(ha_name));
    nv.end();
    LOG_I("[CONFIG] Saved module name: addr=%u mqtt=%s ha=%s\n", slave_addr, mqtt_name, ha_name);
}

String config_get_mqtt_name(uint8_t slave_addr) {
    if (slave_addr < 1 || slave_addr > 247) return String();
    Preferences nv;
    nv.begin(NV_NAMESPACE, true);
    char key[8];
    snprintf(key, sizeof(key), "mn%u", slave_addr);
    String val = nv.getString(key, "");
    nv.end();
    return val;
}

String config_get_ha_name(uint8_t slave_addr) {
    if (slave_addr < 1 || slave_addr > 247) return String();
    Preferences nv;
    nv.begin(NV_NAMESPACE, true);
    char key[8];
    snprintf(key, sizeof(key), "hn%u", slave_addr);
    String val = nv.getString(key, "");
    nv.end();
    return val;
}

void config_load_module_names() {
    // This is called lazily; module names are read individually via config_get_mqtt_name/ha_name
    LOG_ILN("[CONFIG] Module name lazy-load enabled (per-request NVRAM reads)");
}

// ─── Module Area / Room Persistence ───────────────────────────
void config_save_module_area(uint8_t slave_addr, const char *area) {
    if (slave_addr < 1 || slave_addr > 247) return;
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    char key[10];
    snprintf(key, sizeof(key), "ar%u", slave_addr);
    nv.putString(key, String(area));
    nv.end();
    LOG_I("[CONFIG] Saved module area: addr=%u area=%s\n", slave_addr, area);
}

String config_get_module_area(uint8_t slave_addr) {
    if (slave_addr < 1 || slave_addr > 247) return String();
    Preferences nv;
    nv.begin(NV_NAMESPACE, true);
    char key[10];
    snprintf(key, sizeof(key), "ar%u", slave_addr);
    String val = nv.getString(key, "");
    nv.end();
    return val;
}

// ─── Module List Persistence (fast boot) ─────────────────────
// Saves found module slave addresses to NVRAM so next boot skips scan.
extern Slave_Module *modules;
extern uint16_t module_count;

void config_save_module_list() {
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    uint8_t count = (module_count > MAX_SAVED_MODULES) ? MAX_SAVED_MODULES : module_count;
    nv.putUChar(NV_KEY_MOD_LIST_N, count);
    for (uint8_t i = 0; i < count; i++) {
        char akey[12], mkey[12];
        snprintf(akey, sizeof(akey), "%s%u", NV_KEY_MOD_ADDR, i);
        snprintf(mkey, sizeof(mkey), "%s%u", NV_KEY_MOD_MODEL, i);
        nv.putUChar(akey, modules[i].slave_addr);
        nv.putUChar(mkey, modules[i].model.model_id);
    }
    nv.end();
    LOG_I("[CONFIG] Module list saved: %u modules\n", count);
}

uint8_t config_load_module_list() {
    Preferences nv;
    nv.begin(NV_NAMESPACE, true);
    uint8_t count = nv.getUChar(NV_KEY_MOD_LIST_N, 0);
    if (count == 0 || count > MAX_SAVED_MODULES) {
        nv.end();
        return 0;
    }
    // Validate: check all addresses are in range
    bool valid = true;
    for (uint8_t i = 0; i < count; i++) {
        char akey[12];
        snprintf(akey, sizeof(akey), "%s%u", NV_KEY_MOD_ADDR, i);
        uint8_t addr = nv.getUChar(akey, 0);
        if (addr < 1 || addr > 247) { valid = false; break; }
    }
    nv.end();
    if (!valid) {
        LOG_ELN("[CONFIG] Saved module list invalid, clearing");
        config_clear_module_list();
        return 0;
    }
    LOG_I("[CONFIG] Found saved module list: %u modules\n", count);
    return count;
}

void config_clear_module_list() {
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    uint8_t count = nv.getUChar(NV_KEY_MOD_LIST_N, 0);
    for (uint8_t i = 0; i < count && i < MAX_SAVED_MODULES; i++) {
        char akey[12], mkey[12];
        snprintf(akey, sizeof(akey), "%s%u", NV_KEY_MOD_ADDR, i);
        snprintf(mkey, sizeof(mkey), "%s%u", NV_KEY_MOD_MODEL, i);
        nv.remove(akey);
        nv.remove(mkey);
    }
    nv.putUChar(NV_KEY_MOD_LIST_N, 0);
    nv.end();
    LOG_ILN("[CONFIG] Module list cleared from NVRAM");
}

// ─── Per-Entity Name Persistence (relays + DI) ─────────────────
// Key format: rn<addr>_<0-5> for relay, dn<addr>_<0-5> for DI
void config_save_relay_name(uint8_t slave_addr, uint8_t idx, const char *name) {
    if (slave_addr < 1 || slave_addr > 247 || idx > 5) return;
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    char key[12];
    snprintf(key, sizeof(key), "rn%u_%u", slave_addr, idx);
    nv.putString(key, String(name));
    nv.end();
}

String config_get_relay_name(uint8_t slave_addr, uint8_t idx) {
    if (slave_addr < 1 || slave_addr > 247 || idx > 5) return String();
    Preferences nv;
    nv.begin(NV_NAMESPACE, true);
    char key[12];
    snprintf(key, sizeof(key), "rn%u_%u", slave_addr, idx);
    String val = nv.getString(key, "");
    nv.end();
    return val;
}

void config_save_di_name(uint8_t slave_addr, uint8_t idx, const char *name) {
    if (slave_addr < 1 || slave_addr > 247 || idx > 5) return;
    Preferences nv;
    nv.begin(NV_NAMESPACE, false);
    char key[12];
    snprintf(key, sizeof(key), "dn%u_%u", slave_addr, idx);
    nv.putString(key, String(name));
    nv.end();
}

String config_get_di_name(uint8_t slave_addr, uint8_t idx) {
    if (slave_addr < 1 || slave_addr > 247 || idx > 5) return String();
    Preferences nv;
    nv.begin(NV_NAMESPACE, true);
    char key[12];
    snprintf(key, sizeof(key), "dn%u_%u", slave_addr, idx);
    String val = nv.getString(key, "");
    nv.end();
    return val;
}

// ─── HTML Portal Page ──────────────────────────────────────────
static const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="hu">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Modbus-MQTT Bridge v2.0</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#0d1117;color:#c9d1d9;padding:12px;font-size:15px}
h1{color:#58a6ff;font-size:1.4em;margin:8px 0}
h2{color:#f0883e;background:#161b22;padding:8px 12px;border-radius:6px;margin:16px 0 8px;font-size:1.1em}
.fm{margin-bottom:8px}
label{display:block;font-weight:600;color:#7ee787;margin-bottom:2px;font-size:14px}
input,select{width:100%;padding:8px;border:1px solid #30363d;border-radius:4px;background:#0d1117;color:#c9d1d9;font-size:15px}
input:focus,select:focus{outline:none;border-color:#58a6ff}
.row{display:flex;gap:8px}
.row .fm{flex:1}
.chk{display:flex;align-items:center;gap:6px;margin:6px 0}
.chk label{margin:0;font-weight:normal;color:#c9d1d9}
.chk input{width:auto}
button{background:#238636;color:white;border:none;padding:10px 20px;border-radius:6px;font-size:16px;cursor:pointer;margin:10px 0}
button:hover{background:#2ea043}
.note{color:#8b949e;font-size:12px;margin:2px 0 8px}
.pri{background:#161b22;border:1px solid #f0883e;border-radius:6px;padding:12px;margin:8px 0}
.pri b{color:#f0883e}
.foot{text-align:center;color:#484f58;font-size:11px;margin-top:16px;border-top:1px solid #21262d;padding-top:8px}
</style>
</head>
<body>
<h1>&#9889; Modbus-MQTT Bridge v2.0</h1>
<p style="color:#8b949e">Waveshare ESP32-S3-ETH V1.0 (6DI+6R) — ESP32-S3 | ESPHome független</p>

<div class="pri">
<b>&#128279; Hálózati stratégia: LAN elsődleges</b><br>
<span class="note">LAN mindig elsődleges ha elérhető. WiFi automatikus fallback ha LAN leáll. Ha LAN visszajön, automatikusan visszaáll LAN-ra.</span>
</div>

<form action="/save" method="POST">

<h2>&#128279; LAN / Ethernet (Elsődleges)</h2>
<div class="chk"><input type="checkbox" id="lanen" name="lanen" value="1" onchange="toggleLan()"><label for="lanen">LAN engedélyezése</label></div>
<div id="lanSection">
<div class="fm"><label>Chip típus</label><select name="lantype"><option value="0">W5500 (SPI)</option><option value="1">LAN8720 (RMII)</option><option value="2">IP101 (Waveshare beépített)</option></select></div>
<div class="chk"><input type="checkbox" id="landhcp" name="landhcp" value="1" checked onchange="toggleLanStatic()"><label for="landhcp">DHCP</label></div>
<div id="lanStatic">
<div class="row">
<div class="fm"><label>IP cím</label><input name="lanip" placeholder="192.168.1.100"></div>
<div class="fm"><label>Alhálózati maszk</label><input name="lanmask" placeholder="255.255.255.0"></div>
</div>
<div class="row">
<div class="fm"><label>Átjáró</label><input name="langw" placeholder="192.168.1.1"></div>
<div class="fm"><label>DNS</label><input name="landns" placeholder="192.168.1.1"></div>
</div>
</div>
</div>

<h2>&#128225; WiFi (Fallback)</h2>
<div class="note">WiFi csak akkor lép be, ha a LAN nem elérhető. Automatikusan visszaáll LAN-ra, ha az újra elérhető.</div>
<div class="fm"><label>SSID</label><input name="ssid" placeholder="WiFi neve"></div>
<div class="fm"><label>Jelszó</label><input type="password" name="wpass" placeholder="WiFi jelszó"></div>
<div class="chk"><input type="checkbox" id="wdhcp" name="wdhcp" value="1" checked onchange="toggleWifiStatic()"><label for="wdhcp">DHCP</label></div>
<div id="wifiStatic">
<div class="row">
<div class="fm"><label>IP cím</label><input name="wip" placeholder="192.168.1.101"></div>
<div class="fm"><label>Alhálózati maszk</label><input name="wmask" placeholder="255.255.255.0"></div>
</div>
<div class="row">
<div class="fm"><label>Átjáró</label><input name="wgw" placeholder="192.168.1.1"></div>
<div class="fm"><label>DNS</label><input name="wdns" placeholder="192.168.1.1"></div>
</div>
</div>

<h2>&#128172; MQTT Broker</h2>
<div class="row">
<div class="fm"><label>Host</label><input name="mhost" placeholder="homeassistant.local" value="homeassistant.local"></div>
<div class="fm"><label>Port</label><input type="number" name="mport" value="1883"></div>
</div>
<div class="row">
<div class="fm"><label>Felhasználó (opcionális)</label><input name="muser"></div>
<div class="fm"><label>Jelszó (opcionális)</label><input type="password" name="mpass"></div>
</div>
<div class="fm"><label>MQTT Prefix</label><input name="mpfx" value="modbusmqtt"></div>

<h2>&#127968; Home Assistant</h2>
<div class="chk"><input type="checkbox" id="hadisc" name="hadisc" value="1" checked><label for="hadisc">Auto-discovery (MQTT automatikus eszközfelvétel)</label></div>
<p class="note">Ha be van kapcsolva, a 6 relé és 6 DI automatikusan megjelenik a HA-ban. Kikapcsolva csak nyers MQTT üzenetek.</p>

<h2>&#128268; Modbus RS485</h2>
<div class="row">
<div class="fm"><label>Baud rate</label>
<select name="mbaud">
<option value="9600" selected>9600</option>
<option value="19200">19200</option>
<option value="38400">38400</option>
<option value="115200">115200</option>
</select></div>
<div class="fm"><label>Paritás</label>
<select name="mpar">
<option value="0" selected>8N1 (nincs)</option>
<option value="1">8E1 (páros)</option>
<option value="2">8O1 (páratlan)</option>
</select></div>
</div>
<div class="row">
<div class="fm"><label>Cím tartomány kezdete</label><input type="number" name="mstart" value="1" min="1" max="247"></div>
<div class="fm"><label>Cím tartomány vége</label><input type="number" name="mend" value="247" min="1" max="247"></div>
</div>
<p class="note">Korlátlan számú Modbus modul (6DI + 6R fix). Max 247 Modbus cím.</p>

<h2>&#128272; Modbus TCP Bridge</h2>
<div class="chk"><input type="checkbox" id="tcpen" name="tcpen" value="1" checked onchange="toggleTcp()"><label for="tcpen">TCP bridge engedélyezése</label></div>
<div id="tcpSection">
<div class="fm"><label>TCP port</label><input type="number" name="tcpp" value="502"></div>
</div>

<button type="submit">&#128190; Mentés & Újraindítás</button>
</form>

<div class="foot">Modbus-MQTT Bridge v2.0 — ESP32-S3-ETH (6DI+6R) — ESP32-S3</div>

<script>
function toggleLan(){document.getElementById('lanSection').style.display=document.getElementById('lanen').checked?'block':'none'}
function toggleLanStatic(){document.getElementById('lanStatic').style.display=document.getElementById('landhcp').checked?'none':'block'}
function toggleWifiStatic(){document.getElementById('wifiStatic').style.display=document.getElementById('wdhcp').checked?'none':'block'}
function toggleTcp(){document.getElementById('tcpSection').style.display=document.getElementById('tcpen').checked?'block':'none'}
toggleLan();toggleLanStatic();toggleWifiStatic();toggleTcp();
</script>
</body></html>
)rawliteral";

static const char SAVED_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><title>Mentve</title>
<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;text-align:center;padding:40px}
.ok{color:#3fb950;font-size:28px}p{margin:12px 0;color:#8b949e}</style></head>
<body>
<h1 class="ok">&#10004; Beállítások elmentve!</h1>
<p>Az eszköz újraindul és csatlakozik a hálózatra.</p>
<p>LAN elsődleges, WiFi automatikus fallback.</p>
<p style="margin-top:20px">&#9889; Modbus-MQTT Bridge v2.0</p>
</body></html>
)rawliteral";

// ─── Start Config Portal ──────────────────────────────────────
void config_start_portal() {
    LOG_ILN("[PORTAL] Starting config portal...");
    portal_active = true;
    
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
    
    LOG_I("[PORTAL] AP: %s, IP: %s\n", WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
    dns.start(53, "*", WiFi.softAPIP());
    
    portal.on("/", HTTP_GET, []() {
        portal.send_P(200, "text/html", PORTAL_HTML);
    });
    
    portal.on("/save", HTTP_POST, []() {
        config_save();
        portal.send_P(200, "text/html", SAVED_HTML);
        delay(2000);
        ESP.restart();
    });
    
    portal.begin();
    
    uint32_t start = millis();
    while (millis() - start < (WIFI_TIMEOUT_S * 1000)) {
        dns.processNextRequest();
        portal.handleClient();
        if (!portal_active) break;
        delay(10);
    }
    
    portal_active = false;
    LOG_ILN("[PORTAL] Timeout, restarting...");
    ESP.restart();
}