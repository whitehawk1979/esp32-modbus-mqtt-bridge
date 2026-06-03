# KC868-HA V2 — Teljes Modbus Register Map

> Forrás: KinCony hivatalos PDF + forum + firmware binary analízis + közösség
> ⚠️ Firmware verzió függő — probing szükséges a megerősítéshez
> Minden HA V2 változat: 6 DI + 6 Relé (fix)

## ⚠️ FONTOS: Két különböző protokoll létezik!

### 1. KC868-HA Saját Protokoll (hivatalos PDF —KinCony download)
A KC868-HA főegység **saját** nem-Modbus protokollt használ a relé board-dal (pl. H32B Pro):
- **DI→Relé toggle**: `SLAVE 10 00 0A 00 06 0C [6×(relay+mode)] CRC` (FC0F szerű, DE fix offset 0x000A)
- **Relé visszajelzés**: `SLAVE 03 06 55 AA [4 byte bitmask] CRC` (FC03 szerű, DE fix magic 0x0655AA)
- **NEM szabványos Modbus** — fix payload struktúra, nem cím-alapú

#### DI→Relé Toggle Formátum (hivatalos PDF v1.06)
```
01 10 00 0A 00 06 0C  — fix header (slave + FC10 + reg 0x000A + 6 regs + 12 bytes)
  [K1: relay# + mode]  — key1: relay szám + mód byte
  [K2: relay# + mode]  — key2
  [K3-K6: 00 00]       — unused keys
  [CRC]
```

**Két DI mód létezik:**

**A) Momentary/Latch mód** (régi, v1.0):
- Mód byte: `01` = toggle (csak ez az opció)
- Példa: `01 01` = relay1 toggle, `02 01` = relay2 toggle
- Max relay: 128 (`80 01` = relay128 toggle)

**B) EdgeEvent mód** (új, v1.06+):
- Kódolás: `relay# + 100 (hex)` + akció kód
- Példa: `65 01` = 0x65 = 101 dec, 101-100 = relay1, akció=01(ON)
- Akció kódok:
  - `00` = NoDef (nem csinál semmit)
  - `01` = EvtON (BEkapcsol)
  - `02` = EvtOFF (KIkapcsol)
  - `03` = EvtTog (TOGGLE)
- Rising edge = gomb lenyomás, Falling edge = gomb felengedés
- **6 DI × 2 él = 12 akció konfigurálható!**

**EdgeEvent példák (hivatalos PDF):**
| Gomb | Rising Edge | Falling Edge | Használati eset |
|---|---|---|---|
| K1 | relay1 ON (65 01) | relay1 OFF (65 02) | Binary sensor (nyomva=tart=ON) |
| K2 | relay3 Toggle (67 03) | relay3 Toggle (67 03) | Régi kapcsoló (mindkét él) |
| K3 | relay128 Toggle (E4 03) | NoDef (E4 00) | Nyomógomb (momentary) |
| K4 | relay1 ON (65 01) | NoDef (65 00) | BEkapcsoló gomb |
| K5 | relay1 OFF (65 02) | NoDef (65 00) | KIkapcsoló gomb |
| K6 | NoDef (65 00) | relay1 Toggle (65 03) | Felengedéskor toggle |

#### Relé Állapot Visszajelzés (hivatalos PDF)
**A) FW <1.06: max 32 csatorna**
```
01 03 06 55 AA        — fix header (slave + FC03 + magic 0x0655AA)
  byte0: R1-R8       — bit0=R1..bit7=R8 (1=ON, 0=OFF)
  byte1: R9-R16
  byte2: R17-R24
  byte3: R25-R32
  [CRC]
```

**B) FW ≥1.06: max 128 csatorna**
```
01 03 12 55 BB        — fix header (slave + FC03 + byte_count=0x12 + magic 0x55BB)
  byte0: R1-R8        — bit0=R1..bit7=R8
  byte1: R8-R16
  ...
  byte15: R121-R128
  [CRC]
```

> ⚠️ A `0x0655AA` és `0x55BB` magic number-ök nem szabványos Modbus —
> a KC868-HA és a relé board saját protokollja. Mi EZT nem használjuk!

**Ez a protokoll a KC868-HA és a H32B Pro között megy — mi NEM ezt használjuk!**

### 2. Szabványos Modbus RTU (mi firmware-ünk használja)
A mi ESP32 bridge-ünk **szabványos Modbus RTU** protokollt beszél a KC868-HA V2 modulokkal:
- FC01/FC02/FC03/FC05/FC06/FC0F/FC10 — szabványos funkciókódok
- Szabványos register addressing
- Ez a dokumentum EZT a protokollt részletezi

## Coil / Discrete Input Regiszterek (FC01/FC02/FC05/FC0F)

| Cím | FC | Név | R/W | HA Entity | Megjegyzés |
|---|---|---|---|---|---|
| 0x0000 (0) | FC01 | Relé állapotok (R1-R6) | R | switch | Bitmask: bit0=R1…bit5=R6 |
| 0x0000 (0) | FC02 | DI állapotok (DI1-DI6) | R | binary_sensor | Bitmask: bit0=DI1…bit5=DI6 |
| 0x0000 (0) | FC05 | Relé írás (1 coil) | W | switch | 0xFF00=ON, 0x0000=OFF |
| 0x0000 (0) | FC0F | Több relé írás | W | — | Batch coil write |

## Holding Regiszterek (FC03/FC06/FC10) — Azonosítás

| Cím | FC | Név | R/W | HA Entity | Skála |
|---|---|---|---|---|---|
| 0x0064 (100) | FC03 | Model ID | R | sensor (diagnostic) | 1=HA1, 2=HA2, 4=HA4, 8=HA8, 16=HA16 |
| 0x0065 (101) | FC03 | Firmware verzió | R | sensor (diagnostic) | uint16 |
| 0x0100-0x0101 (256-257) | FC03 | Sorozatszám | R | sensor (diagnostic) | 2×uint16 = uint32 |

## Holding Regiszterek — DI+DO Kombinált

| Cím | FC | Név | R/W | HA Entity | Megjegyzés |
|---|---|---|---|---|---|
| 0x00C8 (200) | FC03 | DI+DO állapot | R | — | Reg0=DI bitmask, Reg1=DO bitmask |
| 0x00C8 (200) + FC06 | — | — | ⚠️ Nem dokumentált | — | Lehet, hogy írható? |

## Holding Regiszterek — DI Kattintás Számlálók 🆕

| Cím | FC | Név | R/W | HA Entity | Megjegyzés |
|---|---|---|---|---|---|
| 0x012C-0x0137 (300-311) | FC03 | DI1-DI6 kattintás számláló | R | sensor | uint16, olvasás után állítható 0-ra |
| 0x0324 (804) | FC06 | Számláló törlés | W | button | Érték=1 → összes számláló nulla |

**HA entitás javaslat**: `sensor.<modul>_di1_kattintas` (state_class: total_increasing)

## Holding Regiszterek — Relé Időzítők 🆕

| Cím | FC | Név | R/W | HA Entity | Megjegyzés |
|---|---|---|---|---|---|
| 0x01F4-0x0205 (500-517) | FC03/FC06 | Relé 1-6 időzítő engedélyezés+ mód | R/W | select | bit0: enable, bit1-3: mode (0=off, 1=on-delay, 2=off-delay, 3=on+off) |
| 0x0206-0x0211 (518-529) | FC03/FC06 | Relé 1-6 időzítő tartam | R/W | number | uint16, másodperc |
| 0x0212-0x021D (530-541) | FC03/FC06 | Relé 1-6 időzítő kiegészítő | R/W | — | Firmware függő |

**HA entitás javaslat**: `select.<modul>_r1_timer_mode` + `number.<modul>_r1_timer_duration`

## Holding Regiszterek — Jelenet/Makró 🆕

| Cím | FC | Név | R/W | HA Entity | Megjegyzés |
|---|---|---|---|---|---|
| 0x0258-0x02AF (600-687) | FC03/FC10 | Jelenet konfiguráció | R/W | scene | 16 jelenet, regiszter/család |

**HA entitás javaslat**: `scene.<modul>_scene_1` … `scene.<modul>_scene_16`

## Holding Regiszterek — Watchdog 🆕

| Cím | FC | Név | R/W | HA Entity | Megjegyzés |
|---|---|---|---|---|---|
| 0x02BC (700) | FC03/FC06 | Watchdog időzítő | R/W | switch + number | bit0: enable, bit1-15: timeout (mp) |

**HA entitás javaslat**: `switch.<modul>_watchdog` + `number.<modul>_watchdog_timeout`

## Holding Regiszterek — Konfiguráció 🆕

| Cím | FC | Név | R/W | HA Entity | Megjegyzés |
|---|---|---|---|---|---|
| 0x0320 (800) | FC03/FC06 | Baud rate | R/W | select | 0=9600, 1=19200, 2=38400, 3=57600, 4=115200 |
| 0x0321 (801) | FC03/FC06 | Slave cím | R/W | number | 1-247, reboot után érvényes |
| 0x0322 (802) | FC03/FC06 | Paritás | R/W | select | 0=8N2, 1=8E1, 2=8O1 |
| 0x0323 (803) | FC03/FC06 | Power-on relé alapállapot | R/W | — | Bitmask: bit0=R1 alap=ON |
| 0x0324 (804) | FC06 | DI számláló törlés | W | button | Érték=1 → reset |

## Holding Regiszterek — Hálózati Konfiguráció 🆕

| Cím | FC | Név | R/W | Megjegyzés |
|---|---|---|---|---|
| 0x0384-0x0387 (900-903) | FC03/FC06 | IP cím (4 oktett) | R/W | Csak Ethernet modellek |
| 0x0388-0x038B (904-907) | FC03/FC06 | Alhálózati maszk | R/W | |
| 0x038C-0x038F (908-911) | FC03/FC06 | Átjáró | R/W | |
| 0x0390-0x0393 (912-915) | FC03/FC06 | DNS | R/W | |
| 0x0394 (916) | FC03/FC06 | Modbus TCP port | R/W | Default: 502 |

## Holding Regiszterek — Rendszerállapot 🆕

| Cím | FC | Név | R/W | HA Entity | Megjegyzés |
|---|---|---|---|---|---|
| 0x03E8-0x03E9 (1000-1001) | FC03 | Rendszer uptime | R | sensor | uint32, mp |
| 0x03EA (1002) | FC03 | Rendszer státusz | R | sensor | bit0=ETH, bit1=WiFi |

---

## Kategorizált HA Entity Típusok — KC868-HA V2

### 1. Meglévő (implementált)
- `switch` — Relé ki/be (FC05)
- `binary_sensor` — DI állapot (FC02/FC03 0xC8)
- `sensor` (diagnostic) — Model ID, FW, Serial, Slave Cím

### 2. Kattintásfunkció (implementált, de bővíthető)
- `sensor` — Kattintás esemény (mqtt_publish_click_event, de NEM a HW számlálóból)
- **🆕 `sensor` (total_increasing)** — DI kattintás számláló (FC03 0x012C-0x0137)
  - HARDVERES számláló — nem szoftveres ClickType enum!

### 3. Állapot/Funkció regiszterek (új)
- `select` — Relé időzítő mód (FC03/FC06 0x01F4+)
- `number` — Relé időzítő tartam (FC03/FC06 0x0206+)
- `switch` + `number` — Watchdog enable + timeout (0x02BC)
- `select` — Baud rate / paritás (0x0320, 0x0322)
- `number` — Slave cím (0x0321)

### 4. Jelenet/Makró (új)
- `scene` — 16 jelenet slot (0x0258-0x02AF)

### 5. Rendszer (új)
- `sensor` — Uptime (0x03E8)
- `sensor` — Rendszer státusz (0x03EA)
- `button` — DI számláló törlés (0x0324 FC06)

### ⚠️ Fontos megjegyzések
- A 0x012C+ regisztereket PROBING-GAL kell megerősíteni a firmware verzión!
- A számláló-timeout-jelenet regiszterek nagyobb címeken vannak, firmware V108+
- Az IP/hálózat regiszterek csak Ethernet modelleknél (KCM868-HA-ETH)

## SD Kártya Ötlet (jövőbeli)
> Az összes fenti regiszterMap elmenthető SD kártyára a teljes Modbus scan után.
> Ezzel "statikus" referenciává válik — a firmware tudja, melyik címeken van értelmes adat.
> ⚠️ DE: W5500 és SD kártya UGYANAZT az SPI buszt használja → FEJLESZTÉS KIZÁRT
> Alternatíva: FAT partition a flash-ben (partitions_16mb.csv már dedikál ~10MB FAT-ot)
> Flash FAT-ba mentés → OTA után is megmarad (másik app slot)