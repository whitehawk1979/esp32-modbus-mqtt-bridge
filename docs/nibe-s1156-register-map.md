# NIBE S1156-16 Modbus RTU Register Map — Hőszivattyú

> Forrás: NIBE myUplink API + közösség (yozik0412/nibe_heatpump, elupus/nibe2mqtt)
> Paraméter ID = Holding Register cím (legtöbb implementációban)
> Baud: 9600, 8N1, Slave: 0x01, Byte order: Big-endian
> ⚠️ NEM nyilvános hivatalos doksi — probing szükséges!
> ⚠️ Írható regiszterek: CSAK "system property" paraméterek, garancia érvénytelenítés kockázat!

## Hőmérséklet Szenzorok (csak olvasható, FC03)

| Param/Reg | Név | Skála | HA device_class | Jelenlegi érték | HA entitás |
|---|---|---|---|---|---|
| 4 | BT1 Külső hőmérséklet (pillanat) | ×0.1 | temperature | 17.5°C | ✅ myUplink |
| 8 | BT2 Fűtési előremenő | ×0.1 | temperature | 28.0°C | ✅ myUplink |
| 10 | BT3 Visszatérő vezeték | ×0.1 | temperature | 27.8°C | ✅ myUplink |
| 12 | BT6 Melegvízkészítés | ×0.1 | temperature | 46.0°C | ✅ myUplink |
| 13 | BT10 Talajköri folyadék be | ×0.1 | temperature | 16.7°C | ✅ myUplink |
| 14 | BT11 Talajköri folyadék ki | ×0.1 | temperature | 13.5°C | ✅ myUplink |
| 15 | BT12 Kondenzátor érzékelő | ×0.1 | temperature | 27.9°C | ✅ myUplink |
| 16 | BT14 Folyadékág | ×0.1 | temperature | 46.6°C | ✅ myUplink |
| 17 | BT15 Folyadékvezeték | ×0.1 | temperature | 21.8°C | ✅ myUplink |
| 19 | BT17 Szívó ág | ×0.1 | temperature | 33.7°C | ✅ myUplink |
| 54 | BT1 Külső hőmérséklet (átlag) | ×0.1 | temperature | 16.8°C | ✅ myUplink |
| 57 | BT25 Külső előremenő | ×0.1 | temperature | 27.8°C | ✅ myUplink |
| 7026 | BT84 Elpárologtató | ×0.1 | temperature | 20.2°C | ✅ myUplink |
| 8042 | BP9 Magas nyomás harmatpont | ×0.1 | temperature | 17.9°C | ✅ myUplink |
| 8043 | BP8 Alacsony nyomás harmatpont | ×0.1 | temperature | 18.3°C | ✅ myUplink |

## Kompresszor & Teljesítmény (hiányzik a myUplink-ból!) 🆕

| Param/Reg | Név | Skála | HA Entity | Megjegyzés |
|---|---|---|---|---|
| 4316 | Kompresszor frekvencia | ×1 Hz | sensor (frequency) | Kompresszor sebesség |
| 43150 | Jelenlegi COP | ×10 | sensor (COP) | 450 = COP 4.5 — **KRITIKUS** |
| 40270 | Kompresszor hőteljesítmény | ×10 W | sensor (power) | Termikus kimenet |
| 40271 | Kompresszor villamos teljesítmény | ×10 W | sensor (power) | Villamos bemenet |
| 47136 | Rendszer hőteljesítmény (összes) | ×10 W | sensor (power) | Teljes hőleadás |
| 47138 | Rendszer villamos telj. (összes) | ×10 W | sensor (power) | Teljes áramfelvétel |
| 43084 | Kompresszor üzemóra | ×1 h | sensor (duration) | Összes üzemóra |
| 43116 | Kompresszor indítás számláló | ×1 | sensor | Indítások száma |

## Áramlási sebesség & Nyomás (hiányzik!) 🆕

| Param/Reg | Név | Skála | HA Entity | Megjegyzés |
|---|---|---|---|---|
| 40883 | Talajköri áramlás | ×1 l/min | sensor (flow) | Földhurok áramlás |
| 41570 | Rendszer 1 áramlás | ×1 l/min | sensor (flow) | Fűtési áramlás |
| 40179 | Magas nyomás (hűtőközeg) | ×1 bar | sensor (pressure) | |
| 40181 | Alacsony nyomás (hűtőközeg) | ×1 bar | sensor (pressure) | |
| 40397 | Előremenő nyomás (víz) | ×10 bar | sensor (pressure) | |
| 40405 | Visszatérő nyomás (víz) | ×10 bar | sensor (pressure) | |

## Üzemmód & Státusz

| Param/Reg | Név | Skála | HA Entity | Megjegyzés |
|---|---|---|---|---|
| 47213 | Aktuális üzemmód | enum | sensor | 0=Ki, 1=Fűtés, 2=Melegvíz, 3=Hűtés, 4=Fűtés+MV, 5=Hűtés+MV, 20=Leolvasztás, 21=Olajvisszatérés |
| 45001 | Üzemmód (részletes) | enum | sensor | |
| 4316 | Kompresszor állapot | enum | sensor | 0=ki, 1=fut |

## Riasztások (hiányzik!) 🆕

| Param/Reg | Név | Skála | HA Entity | Megjegyzés |
|---|---|---|---|---|
| 44052 | Riasztás 1 aktív | ×1 | sensor (diagnostic) | Riasztási kód |
| 44053 | Riasztás 2 aktív | ×1 | sensor (diagnostic) | Riasztási kód |
| 44054 | Riasztás 3 aktív | ×1 | sensor (diagnostic) | Riasztási kód |
| 44055 | Riasztás 4 aktív | ×1 | sensor (diagnostic) | Riasztási kód |
| 44170 | Riasztás napló 1 (legutóbbi) | ×1 | sensor (diagnostic) | Tárolt kódok |

## Írható Setpoint-ok (FC06/FC16 — ÓVATOSAN!)

| Param/Reg | Név | Skála | HA Entity | Tartomány | Megjegyzés |
|---|---|---|---|---|---|
| 47011 | Előremenő setpoint (rendszer 1) | ×0.1 °C | number | 15-55°C | Fűtési görbe |
| 47016 | Melegvíz setpoint | ×0.1 °C | number | 40-60°C | MV célhőmérséklet |
| 47024 | Fűtési görbe offset | ×0.1 °C | number | -10…+10°C | Görbe eltolás |
| 47025 | Fűtési görbe meredekség | ×1 | number | | Görbe meredekség |
| 47270 | Üzemmód felülbírálás | enum | select | | Kényszerített mód |
| 4789 | Smart Price Adaption | 0/1 | switch | | ✅ már myUplink-ben |
| 27233 | Vill. fűtőbetét gyorsindítás | 0/1 | switch | | ✅ már myUplink-ben |
| 7086 | Több melegvíz | 0/1 | switch | | ✅ már myUplink-ben |
| 10613 | SG Ready aktiválás | 0/1 | switch | | ✅ már myUplink-ben |

## Rendszer (myUplink-ből már megvan)

| Param/Reg | Név | Skála | HA Entity |
|---|---|---|---|
| 1708 | Számított előremenő fűtés | ×0.1 °C | sensor ✅ |
| 2695 | Számított hűtési előremenő | ×0.1 °C | sensor ✅ |
| 30337 | Smart control | 0/1 | switch ✅ |
| 32628 | Meleg víz hőmérséklete | ×0.1 °C | sensor ✅ |
| 48351 | Klímarendszer 1 setpoint | ×0.1 °C | sensor ✅ |
| 50660 | BT50 Szobahőmérséklet | ×0.1 °C | sensor ✅ |

---

## Optimált Polling Csoportok (ESP32 Bridge-hez)

### Group 1: Hőmérsékletek (10s ciklus) — FC03 batch
- Start: 4, Qty: 16 → Param 4-19 (külső, előremenő, visszatérő, MV, talajköri be/ki, kondenzátor, folyadékág, szívóág)

### Group 2: Átlag/Külső (30s ciklus)
- Start: 54, Qty: 4 → Param 54-57
- Start: 1708, Qty: 1

### Group 3: Kompresszor & Teljesítmény (10s ciklus) — 🔑 KRITIKUS
- Start: 40270, Qty: 2 → Termikus + Villamos teljesítmény
- Start: 4316, Qty: 1 → Kompresszor frekvencia
- Start: 43150, Qty: 1 → COP

### Group 4: Rendszer összesítő (10s ciklus)
- Start: 47136, Qty: 3 → Összes hő+villamos+üzemmód

### Group 5: Riasztások (60s ciklus)
- Start: 44052, Qty: 4 → 4 riasztási kód

### Group 6: Nyomás/Áramlás (30s ciklus)
- Start: 40179, Qty: 2 → Magas/alacsony nyomás
- Start: 40397, Qty: 2 → Víz nyomás
- Start: 40883, Qty: 1 → Áramlás
- Start: 41570, Qty: 1 → Rendszer áramlás

### Group 7: Évszak átlag (300s ciklus)
- Start: 10845, Qty: 2 → EB100-BT10, EB100-BT11
- Start: 8042, Qty: 2 → Harmatpontok
- Start: 7026, Qty: 1 → Elpárologtató

---

## Batch olvasási példa (ESP32 ModbusMaster)

```cpp
// Group 1: Hőmérsékletek — 1 FC03 tranzakció = 16 regiszter
node.begin(1, Serial2); // NIBE slave = 1
uint8_t result = node.readHoldingRegisters(4, 16);
if (result == node.ku8MBSuccess) {
    float outdoor = (int16_t)node.getResponseBuffer(0) * 0.1;  // Reg 4
    float supply  = (int16_t)node.getResponseBuffer(4) * 0.1;  // Reg 8 (offset +4)
    float ret     = (int16_t)node.getResponseBuffer(6) * 0.1;  // Reg 10
    float dhw     = (int16_t)node.getResponseBuffer(8) * 0.1;  // Reg 12
    float brine_in = (int16_t)node.getResponseBuffer(9) * 0.1; // Reg 13
    float brine_out= (int16_t)node.getResponseBuffer(10) * 0.1; // Reg 14
}
```

⚠️ **Fontos**: A getResponseBuffer() index a start offset-től számít!
- readHoldingRegisters(4, 16) → buffer[0] = reg 4, buffer[4] = reg 8

## GitHub Referenciák

| Repo | Protokoll | Regiszter Map |
|---|---|---|
| yozik0412/nibe_heatpump | NIBE internal bus (UDP) | nibe_data.py — legteljesebb param DB |
| elupus/nibe2mqtt | NIBE internal bus → MQTT | nibe_data.py — param + unit + scale |
| t0bg4s/nibe-modbus | Modbus RTU | Direkt Modbus RTU címek |
| pendingcosmos/nibe-modbus-esp32 | Modbus RTU | S1xxx sorozat specifikus |

## Probing Stratégia (éles firmware teszt)

1. Olvasd a már ismert címeket (4, 8, 10, 12, 13, 14) → validálja a kapcsolatot
2. Szkenneld: 4000-4100, 43000-43200, 44000-44200, 47000-47200
3. Minden érvényes válasz (nem exception 0x02) → mentés referenciaként
4. Cross-check: hasonlítsd a yozik0412/nibe_heatpump nibe_data.py-vel
5. **SOHA ne írj** setpoint regiszterbe probing közben!