# ⚡ ESP32-S3 Modbus-MQTT Bridge

Firmware for ESP32-S3 that bridges KinCony Modbus relay controllers (RS485) to Home Assistant via MQTT. Dual-network (LAN+WIFI), Digest Auth, auto-provisioning, OTA updates.

## Features

- **Dual Network**: W5500 Ethernet (primary) + WiFi (fallback), automatic failover
- **Home Assistant Integration**: MQTT Auto-Discovery — relays & DIs appear automatically
- **Web UI**: Dark-themed dashboard with 6 tabs (Status, Settings, Pins, Modules, OTA, Admin)
- **Digest Auth**: Read endpoints open, write endpoints protected (admin/admin default)
- **Factory Provisioning**: First boot after flash auto-configures WiFi, LAN, MQTT, modules, auth
- **OTA Firmware Update**: Upload `.bin` via web browser
- **Backup/Restore**: JSON export/import of all NVRAM settings

## Supported Hardware

| Controller | Address | Profile |
|---|---|---|
| KinCony F16 (16 relays + 6 DI) | 200 | KC868-HA V2 |
| KinCony A16 (32 relays) | — | KC868-HA V2 |
| Generic Modbus RTU | Any | Configurable register map |

## Hardware Requirements

- ESP32-S3 with USB-C (for flashing)
- W5500 Ethernet module (SPI) — Waveshare pinout
- RS485 transceiver (connected to UART)
- KinCony controller(s) on Modbus bus

## Pin Configuration (Waveshare W5500 + ESP32-S3)

| Function | GPIO | ESP32-S3 Pin |
|---|---|---|
| RS485 RX | 44 | UART RX |
| RS485 TX | 43 | UART TX |
| RS485 DE | 4 | Direction |
| ETH MOSI | 11 | SPI |
| ETH MISO | 9 | SPI |
| ETH SCLK | 13 | SPI |
| ETH CS | 12 | SPI Chip Select |
| ETH INT | 14 | Interrupt |
| ETH RST | 21 | Reset |
| Status LED | 2 | Built-in |
| Setup Button | 0 | BOOT button (hold 5s → WiFi portal) |

## Quick Start

### 1. Flash Firmware

```bash
# Erase flash (first time only — clears NVRAM)
esptool.py --chip esp32s3 --port /dev/ttyACM0 erase_flash

# Write firmware
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 460800 \
  write-flash 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```

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
| MQTT Prefix | `modbusmqtt` |
| Web Auth | Enabled (Digest) |
| Admin Password | `admin` |
| Modbus Baud | 9600 |
| HA Discovery | Enabled |
| Module 0 | KinCony F16, addr=200 |

**Customize**: After boot, open `http://<device-ip>/config` and change any settings.

### 3. Access Web UI

- **Status**: `http://<device-ip>/` — real-time network, Modbus, module overview
- **Settings**: `http://<device-ip>/config` — WiFi, LAN, MQTT, Modbus, Auth
- **Admin**: `http://<device-ip>/admin` — password management
- **OTA**: `http://<device-ip>/ota` — firmware upload

Default login: `admin` / `admin` (Digest Auth)

## Web UI Tabs

### 📊 Status (`/`)
Network interfaces (WiFi + LAN), MQTT state, Modbus scan stats, module list with relay states, heap/uptime.

### ⚙️ Settings (`/config`)
Full device configuration: WiFi (SSID, password, AP name, AP password, mode, DHCP/static), LAN (W5500, DHCP/static), MQTT (broker, user, password, prefix), Web auth, HA discovery, Modbus (baud, parity, scan range, profile), TCP server.

### 📌 Pins (`/pins`)
GPIO pin assignments for RS485, Ethernet SPI, LED, button. Changes saved to NVRAM.

### 🔌 Modules (`/modules`)
Modbus module list: scan, address assignment, relay & DI naming, room/area assignment. HA discovery publishes entities per relay/DI.

### 📦 OTA (`/ota`)
Upload `.bin` firmware file directly. Device reboots after successful upload.

### 🔐 Admin (`/admin`)
- **Admin password**: Change with current password verification + confirmation. Reset to default (`admin`).
- **AP password**: Change WiFi Access Point password (min 8 chars for WPA2). Reset to default (`12345678`). Changes take effect immediately.

## Home Assistant Integration

When HA Discovery is enabled, the bridge publishes MQTT auto-discovery messages:

```
modbusmqtt/KinCony_F16/relay/1/config  → switch entity
modbusmqtt/KinCony_F16/di/1/config    → binary_sensor entity
```

Entities appear automatically in HA under **Settings → Devices**. No manual configuration needed.

## API Endpoints

| Endpoint | Method | Auth | Description |
|---|---|---|---|
| `/api/status` | GET | No | JSON: network, MQTT, modules, heap |
| `/api/config` | GET | No | JSON: all config (passwords masked) |
| `/api/backup` | GET | Yes | Full NVRAM backup as JSON |
| `/api/restore` | POST | Yes | Restore NVRAM from JSON backup |
| `/relay` | GET | Yes | Toggle relay: `?addr=200&r=0` |
| `/restart` | GET | Yes | Reboot device |
| `/rescan` | POST | Yes | Rescan Modbus bus |
| `/save` | POST | Yes | Save settings to NVRAM + reboot |
| `/saveadmin` | POST | Yes | Save admin/AP passwords |
| `/savepins` | POST | Yes | Save GPIO pin config |
| `/savemodules` | POST | Yes | Save module names/config |
| `/savemodlist` | POST | Yes | Save module list |

## Backup & Restore

### Backup
```bash
curl --digest -u admin:admin http://192.168.1.45/api/backup -o backup.json
```

Passwords are exported as `***`. The backup includes a `"type": "modbus-mqtt-bridge-backup"` field required for restore validation.

### Restore
```bash
curl --digest -u admin:admin -X POST \
  -H "Content-Type: application/json" \
  -d @backup.json \
  http://192.168.1.45/api/restore
```

**Note**: Password fields (`***`) are skipped during restore — set them via `/config` or `/admin` after.

## Build from Source

### Prerequisites
- PlatformIO (`pip install platformio`)
- ESP32-S3 board support (auto-installed by PlatformIO)

### Build
```bash
# Development build (debug symbols, serial output)
pio run -e esp32-s3

# Production build (optimized, ~1MB)
pio run -e esp32-s3-prod
```

Firmware output: `.pio/build/esp32-s3-prod/firmware.bin`

### Environments
| Environment | Purpose | Serial | Optimized |
|---|---|---|---|
| `esp32-s3` | Development | ✅ | ❌ |
| `esp32-s3-prod` | Production | ❌ | ✅ |

## Network Architecture

```
┌──────────────┐   RS485    ┌──────────────┐
│  KinCony F16 │◄──────────►│   ESP32-S3   │
│  (addr: 200) │            │  + W5500 LAN │
└──────────────┘            └──────┬───────┘
                                   │
                    ┌──────────────┼──────────────┐
                    │              │              │
               LAN (W5500)    WiFi (STA)     WiFi (AP)
              Primary ✅     Fallback      Always-on
              DHCP/Static    DHCP/Static   192.168.4.1
                                   │
                              MQTT Broker
                           192.168.1.43:1883
                                   │
                           ┌───────┴───────┐
                           │  Home Assistant │
                           │  Auto-Discovery  │
                           └───────────────┘
```

- **LAN** is always primary when cable is connected
- **WiFi STA** activates as fallback when LAN link drops
- **WiFi AP** always broadcasts (AP+STA mode) — device reachable without router
- Automatic failback: when LAN reconnects, traffic shifts back

## Configuration File Structure

| Source | When Used | Persisted |
|---|---|---|
| `cfg_defaults()` | RAM initialization, immediate | No (lost on reboot) |
| `factory_provision()` | First boot after erase, writes NVRAM | Yes (writes once) |
| `config_load()` → NVRAM | Every boot, reads NVRAM | Yes |
| `/save` → NVRAM | Web UI save, writes NVRAM | Yes |

Priority: NVRAM > `cfg_defaults()` > `factory_provision()` (only if key missing)

## Safety Features

- **Digest Auth**: Passwords never sent in plaintext over the network
- **Read/Write separation**: Status & config readable without login; control & config changes require auth
- **D4 Bug guard**: `/save` only writes keys present in POST body — empty fields don't zero NVRAM
- **Password masking**: `/api/backup` exports passwords as `***`
- **Admin password verification**: Current password required to change admin password
- **WiFi portal**: Hold BOOT button 5s → captive portal for emergency WiFi setup

## License

MIT

## Author

**Zsolt Lakatos** — [GitHub](https://github.com/whitehawk1979/esp32-modbus-mqtt-bridge)