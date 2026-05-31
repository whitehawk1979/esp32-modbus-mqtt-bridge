# ⚡ ESP32-S3 Modbus-MQTT Bridge

**v2.5.0** — Firmware for ESP32-S3 that bridges KinCony Modbus relay controllers (RS485) to Home Assistant via MQTT. Dual-network (LAN+WiFi), MQTT auto-discovery, 8MB PSRAM optimization, web UI with 6 tabs, Digest Auth, auto-provisioning, OTA updates.

## Features

- **Dual Network**: W5500 Ethernet (primary) + WiFi (fallback), automatic failover
- **MQTT Dual-Transport**: Broker connection over LAN preferred, WiFi fallback, auto-switch on link change
- **Home Assistant Integration**: MQTT Auto-Discovery — relays, DIs, click counters, bridge sensors appear automatically
- **PSRAM Optimization**: 8MB OPI PSRAM — discovery buffers, large JSON payloads allocated in SPIRAM
- **DI Toggle**: Digital inputs togglable from web UI + API (virtual modules)
- **Web UI**: Dark-themed dashboard — Status, Settings, Pins, Modules, OTA, Admin
- **Digest Auth**: Read endpoints open, write endpoints protected (admin/admin default)
- **Factory Provisioning**: First boot after flash auto-configures WiFi, LAN, MQTT, modules, auth
- **OTA Firmware Update**: Browser upload (POST) or ESP32-initiated download (GET from URL)
- **Backup/Restore**: JSON export/import of all NVRAM settings

## Supported Hardware

| Controller | Address | Relays | DIs | Profile |
|---|---|---|---|---|
| KinCony F16 | 200 | 16 | 6 | KC868-HA V2 |
| KinCony A16 | — | 32 | 0 | KC868-HA V2 |
| Virtual Module | 200 | 6 | 6 | Built-in (no physical Modbus) |
| Generic Modbus RTU | Any | Configurable | Configurable | Custom register map |

## Hardware Requirements

- **Board**: Waveshare ESP32-S3-ETH V1.0 (16MB flash, 8MB OPI PSRAM, built-in W5500)
- **RS485 transceiver**: Connected to UART (for Modbus bus)
- **KinCony controller(s)**: On Modbus bus (optional — virtual module works standalone)

## Pin Configuration

| Function | GPIO | Note |
|---|---|---|
| RS485 RX | 44 | UART RX |
| RS485 TX | 43 | UART TX |
| RS485 DE | 4 | Direction enable |
| ETH MOSI | 11 | SPI to W5500 |
| ETH MISO | 12 | SPI from W5500 |
| ETH SCLK | 13 | SPI clock |
| ETH CS | 14 | Chip select |
| ETH INT | 10 | Interrupt (W5500) |
| ETH RST | 21 | Reset (W5500) |
| Status LED | 2 | Built-in |
| Setup Button | 0 | BOOT button (hold 5s → WiFi portal) |

## Quick Start

### 1. Flash Firmware

```bash
# Erase flash (first time only — clears NVRAM + bootloader)
esptool.py --chip esp32s3 --port /dev/ttyACM0 erase_flash

# Write ALL three images (bootloader + partitions + firmware)
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 460800 \
  write-flash 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```

> ⚠️ **CRITICAL**: After `erase_flash`, you MUST write all three files. Writing only `firmware.bin` causes boot loop (SHA-256 mismatch, TG0WDT reset).

Build artifacts are in `.pio/build/esp32-s3-prod/`:
- `bootloader.bin` → offset `0x0`
- `partitions.bin` → offset `0x8000`
- `firmware.bin` → offset `0x10000`

### 2. First Boot — Auto-Provisioning

After `erase_flash`, the firmware automatically populates NVRAM with factory defaults:

| Setting | Default |
|---|---|
| WiFi SSID | `Air 2` |
| WiFi Password | `decembertizenhat` |
| AP Name | Auto (`hostname-MAC`) |
| AP Password | `12345678` |
| LAN | W5500 enabled, DHCP |
| MQTT Broker | `192.168.1.43:1883` |
| MQTT User | `mqtt` |
| MQTT Password | `2009December16` |
| MQTT Transport | **LAN preferred**, WiFi fallback |
| MQTT Prefix | `modbusmqtt` |
| Web Auth | Enabled (Digest) |
| Admin Password | `admin` |
| Modbus Baud | 9600 |
| HA Discovery | Enabled |
| Virtual Module | Enabled (addr=200, 6 relays, 6 DIs) |

**Customize**: After boot, open `http://<device-ip>/config` and change any settings.

### 3. Access Web UI

- **Status**: `http://<device-ip>/` — real-time network, MQTT transport, Modbus, modules, PSRAM
- **Settings**: `http://<device-ip>/config` — WiFi, LAN, MQTT, Modbus, Auth
- **Pins**: `http://<device-ip>/pins` — GPIO pin assignments
- **Modules**: `http://<device-ip>/modules` — Modbus modules, relay/DI toggle, naming
- **OTA**: `http://<device-ip>/ota` — firmware upload
- **Admin**: `http://<device-ip>/admin` — password management

Default login: `admin` / `admin` (Digest Auth)

## Network Architecture

```
┌──────────────┐   RS485    ┌──────────────────┐
│  KinCony F16 │◄──────────►│     ESP32-S3     │
│  (addr: 200) │            │ + W5500 LAN      │
│              │            │ + 8MB PSRAM      │
└──────────────┘            └──────┬───────────┘
                                   │
                    ┌──────────────┼──────────────┐
                    │              │              │
               LAN (W5500)    WiFi (STA)     WiFi (AP)
              Primary ✅     Fallback      Always-on
              DHCP/Static    DHCP/Static   192.168.4.1
                    │              │
                    └──────┬───────┘
                           │
                    MQTT Broker ──── LAN transport preferred
                  192.168.1.43:1883   (auto-switch on link change)
                           │
                   ┌───────┴───────┐
                   │ Home Assistant │
                   │ Auto-Discovery │
                   └───────────────┘
```

- **LAN** always primary when cable connected — lower latency, no WDT issues
- **WiFi STA** activates as fallback when LAN link drops
- **WiFi AP** always broadcasts (AP+STA mode) — device reachable without router
- **MQTT transport**: LAN preferred (`EthernetClient`), WiFi fallback (`WiFiClient`), auto-switch on reconnect
- **TLS**: WiFi only (`WiFiClientSecure`) — `EthernetClient` has no TLS support

## Home Assistant Integration

When HA Discovery is enabled, the bridge publishes MQTT auto-discovery messages and `homeassistant/status=online` (retained). HA processes these automatically — no manual configuration needed.

### Entity Types (per module)

| Entity | HA Type | MQTT Topic Pattern |
|---|---|---|
| Relay | `switch` | `modbusmqtt/ha_v2/<addr>/relay/<n>/config` |
| Digital Input | `binary_sensor` | `modbusmqtt/ha_v2/<addr>/di/<n>/config` |
| DI Click Counter | `sensor` | `modbusmqtt/ha_v2/<addr>/di/<n>/click/config` |
| Module Status | `binary_sensor` | `modbusmqtt/ha_v2/<addr>/status/config` |
| Bridge Sensor | `sensor` | Various (heap, uptime, etc.) |

### ⚠️ Critical HA Rules

1. **Never** include `"platform": "mqtt"` in discovery payload — HA auto-detects it; if present, HA **rejects** the config
2. **`homeassistant/status=online`** must be published retained — HA won't process retained discovery topics without it
3. After Mosquitto restart, the bridge re-publishes `homeassistant/status=online` on MQTT reconnect, triggering HA to re-process discovery

### Virtual Module (addr=200)

A built-in virtual module with 6 relays + 6 DIs — works without any physical KinCony controller. Useful for testing, HA automation triggers, and integration validation.

- Relays: togglable from web UI, API, and HA
- DIs: togglable from web UI and API; state published to MQTT

## API Endpoints

### Both WiFi + LAN (dual-stack)

| Endpoint | Method | Auth | Description |
|---|---|---|---|
| `/api/status` | GET | No* | JSON: network, MQTT transport, modules, heap, PSRAM |
| `/api/config` | GET | No* | JSON: all config (passwords masked) |
| `/api/relay` | GET | Yes | Toggle relay: `?addr=200&relay=0&state=1` |
| `/api/di` | GET | Yes | Toggle DI: `?addr=200&di=0&state=1` |
| `/api/savemodules` | POST | Yes | Save module config to NVRAM |
| `/api/savepins` | POST | Yes | Save GPIO pin config to NVRAM |
| `/api/backup` | GET | Yes | Full NVRAM backup as JSON |
| `/api/restore` | POST | Yes | Restore NVRAM from JSON backup |

### WiFi Only

| Endpoint | Method | Auth | Description |
|---|---|---|---|
| `/relay` | GET | Yes | Toggle relay (legacy WiFi endpoint) |
| `/di` | GET | Yes | Toggle DI (legacy WiFi endpoint) |
| `/restart` | GET | Yes | Reboot device |
| `/rescan` | POST | Yes | Rescan Modbus bus |
| `/save` | POST | Yes | Save settings to NVRAM + reboot |
| `/saveadmin` | POST | Yes | Save admin/AP passwords |
| `/savemodules` | POST | Yes | Save module names/config |
| `/savemodlist` | POST | Yes | Save module list |

*\*LAN endpoints use Digest Auth via `eth_auth_ok()`*

### MQTT Transport in API Response

```json
{
  "firmware": "2.5.0",
  "mqtt_connected": true,
  "mqtt_transport": "LAN",
  "psram_free": 8368891,
  "psram_total": 8388608
}
```

`mqtt_transport`: `"LAN"` or `"WiFi"` — shows which transport the broker connection uses.

## OTA Firmware Update

### Method 1: Browser Upload (POST)

Upload `.bin` via web UI at `http://<device-ip>/ota`. Device reboots after successful upload.

> ⚠️ At 16KB/s upload speed, the WiFi WebServer WDT may timeout ~800KB. Use 4KB/s (crawl) or Method 2.

### Method 2: ESP32-Initiated Download (GET) — Recommended

The ESP32 downloads firmware from a URL at its own pace — no TCP buffer overflow, no WDT risk:

```
http://<device-ip>/otaurl?url=http://your-server/firmware.bin
```

`yield()` + `delay(1)` between chunks keeps the TCP stack and WDT happy.

## PSRAM Architecture

8MB OPI PSRAM (auto-detected, `CONFIG_SPIRAM_MODE_OCT`):

| Allocation | Size | Purpose |
|---|---|---|
| Discovery JSON buffer | ~4KB per entity | `psram_alloc()` in `discovery_publish()` |
| Free heap fallback | N/A | `psram_alloc()` falls back to internal heap if PSRAM unavailable |

```
psram_alloc(size):
  if PSRAM available → heap_caps_malloc(size, MALLOC_CAP_SPIRAM)
  else               → malloc(size)  // internal heap fallback
```

API exposes `psram_free` and `psram_total` fields for monitoring.

## Safety Features

- **Digest Auth**: Passwords never sent in plaintext
- **Read/Write separation**: Status & config readable without login; control requires auth
- **D4 Bug guard**: `/save` only writes keys present in POST body — empty fields don't zero NVRAM
- **Password masking**: `/api/backup` exports passwords as `***`
- **Admin password verification**: Current password required to change admin password
- **WiFi portal**: Hold BOOT button 5s → captive portal for emergency WiFi setup
- **MQTT connected check**: `mqtt_publish_relay_state()` and `mqtt_publish_di_state()` verify connection before publish

## Backup & Restore

### Backup
```bash
curl --digest -u admin:admin http://192.168.1.67/api/backup -o backup.json
```

### Restore
```bash
curl --digest -u admin:admin -X POST \
  -H "Content-Type: application/json" \
  -d @backup.json \
  http://192.168.1.67/api/restore
```

**Note**: Password fields (`***`) are skipped during restore — set them via `/config` or `/admin` after.

## Build from Source

### Prerequisites
- PlatformIO (`pip install platformio`)
- ESP32-S3 board support (auto-installed by PlatformIO)

### Build
```bash
# Production build (optimized, ~1.2MB)
pio run -e esp32-s3-prod
```

Firmware output: `.pio/build/esp32-s3-prod/`

### Environments
| Environment | Purpose | Serial | Optimized | PSRAM |
|---|---|---|---|---|
| `esp32-s3` | Development | ✅ | ❌ | ✅ |
| `esp32-s3-prod` | Production | ❌ | ✅ | ✅ |

### Flash Size
- **App partition**: 1280KB (0x10000)
- **Firmware**: ~1180KB (91.7% used)
- **Free space**: ~100KB for future features
- **SPIFFS**: 1408KB
- **Coredump**: 64KB

## Configuration File Structure

| Source | When Used | Persisted |
|---|---|---|
| `cfg_defaults()` | RAM initialization, immediate | No (lost on reboot) |
| `factory_provision()` | First boot after erase, writes NVRAM | Yes (writes once) |
| `config_load()` → NVRAM | Every boot, reads NVRAM | Yes |
| `/save` → NVRAM | Web UI save, writes NVRAM | Yes |

NVRAM versioning: `config_validate()` checks config version + CRC. Mismatch → `factory_provision()` fills missing keys.

## Changelog

### v2.5.0
- **MQTT dual-transport**: LAN preferred (EthernetClient), WiFi fallback, auto-switch
- **PSRAM helpers**: `psram_alloc()`, `psram_free()`, `psram_total()` — SPIRAM allocation with heap fallback
- **API PSRAM metrics**: `psram_free`, `psram_total` in `/api/status`
- **LAN API routes**: `/api/relay`, `/api/di`, `/api/savemodules`, `/api/savepins` on Ethernet web server
- **DI toggle**: Web UI clickable buttons + `/di` API endpoint for virtual modules
- **`homeassistant/status=online`**: Retained publish on MQTT connect/reconnect — HA discovery race condition fix
- **Discovery payload fix**: `topic_base()` path correction, `"platform":"mqtt"` removed (HA rejects it)
- **Discovery refactor**: `set_device_block()`, `set_availability()`, `discovery_publish()` helpers — deduplication, PSRAM buffer
- **W5500 pin fix**: MOSI=11, MISO=12, SCLK=13, CS=14, INT=10 (Waveshare pinout)
- **LAN auto-start**: W5500 Ethernet initializes automatically on boot, no manual config
- **WDT fix**: `yield()` + `delay()` in OTA handler prevents TG0WDT_SYS_RST during flash writes

### v2.4.0
- Initial hardening: XSS protection, rate limiting, auth enforcement
- Web template refactor
- NVRAM versioning + config validation
- OTA yield/delay for WDT safety

## License

MIT