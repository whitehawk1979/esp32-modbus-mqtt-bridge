#pragma once

#include <Arduino.h>
#include <stdint.h>

class BitBangSD {
public:
    // ------------------------------------------------------------------ config
    static constexpr uint16_t BLOCK_SIZE = 512;
    static constexpr uint8_t  R1_IDLE    = 0x01;
    static constexpr uint8_t  R1_READY   = 0x00;

    // ------------------------------------------------------------------ public
    bool init(int cs, int miso, int mosi, int sclk, int power_pin, bool doPowerCycle = true) {
        _cs        = cs;
        _miso      = miso;
        _mosi      = mosi;
        _sclk      = sclk;
        _power_pin  = power_pin;
        _card_type  = 0;          // 0=unknown, 1=SDv1, 2=SDv2, 3=SDHC
        _ocr        = 0;

        // --- pin setup ---
        pinMode(_power_pin, OUTPUT);
        pinMode(_cs,   OUTPUT);
        pinMode(_sclk, OUTPUT);
        pinMode(_mosi, OUTPUT);
        pinMode(_miso, INPUT_PULLUP);  // Pull-up for MISO

        // Ensure CS HIGH before power cycle (prevent bus conflict)
        csHigh();
        digitalWrite(_mosi, HIGH);
        digitalWrite(_sclk, HIGH);

        // --- power sequence ---
        if (doPowerCycle) {
            digitalWrite(_power_pin, HIGH);  // power OFF (HIGH = off)
            delay(1000);                     // 1s off
            digitalWrite(_power_pin, LOW);   // power ON  (LOW  = on)
            delay(1000);                     // 1s settle time
        } else {
            // Ensure power is ON
            pinMode(_power_pin, OUTPUT);
            digitalWrite(_power_pin, LOW);   // power ON
            delay(200);
        }

        // --- MISO pre-conditioning (exact copy of /api/sd/debug Phase 1-4) ---
        // Phase 1: MISO drive test
        pinMode(_miso, OUTPUT);
        digitalWrite(_miso, LOW); delayMicroseconds(10);
        (void)digitalRead(_miso);
        digitalWrite(_miso, HIGH); delayMicroseconds(10);
        (void)digitalRead(_miso);

        // Phase 2: MISO rise-time probe (discharge then release)
        pinMode(_miso, OUTPUT); digitalWrite(_miso, LOW); delayMicroseconds(50);
        pinMode(_miso, INPUT);
        (void)digitalRead(_miso);
        delayMicroseconds(5);
        (void)digitalRead(_miso);
        delay(5);
        (void)digitalRead(_miso);

        // Phase 3: Pull-down test
        pinMode(_miso, INPUT_PULLDOWN); delayMicroseconds(100);
        (void)digitalRead(_miso);
        pinMode(_miso, INPUT);

        // Phase 4: Cross-talk test
        pinMode(_mosi, OUTPUT); pinMode(_miso, INPUT);
        digitalWrite(_mosi, HIGH); delayMicroseconds(10); (void)digitalRead(_miso);
        digitalWrite(_mosi, LOW);  delayMicroseconds(10); (void)digitalRead(_miso);
        pinMode(_mosi, INPUT);  // Release MOSI — matches debug Phase 4 end
        delayMicroseconds(10);

        // Phase 5: Set all pins for SPI mode (matches debug Phase 5 start)
        pinMode(_cs, OUTPUT);
        pinMode(_sclk, OUTPUT);
        pinMode(_mosi, OUTPUT);
        pinMode(_miso, INPUT);
        delay(1);

        // --- 80+ init clocks with CS HIGH, MOSI HIGH ---
        // SCLK idle HIGH, 80 clock pulses = 10 bytes
        csHigh();
        mosiHigh();
        digitalWrite(_sclk, HIGH);  // ensure idle HIGH
        delay(5);   // matches /api/sd/debug — 5ms settle (was delay(1))
        for (int i = 0; i < 80; i++) clockPulse();   // 80 clocks

        // --- CMD0  GO_IDLE_STATE ---
        csLow();
        delayMicroseconds(10);  // CS setup time
        {
            const uint8_t cmd[] = {0x40, 0,0,0,0, 0x95};
            uint8_t r1 = sendCmd(cmd, 6);
            // Accept R1=0x01 (IDLE) or R1=0x00 (READY — card already init'd)
            if (r1 != R1_IDLE && r1 != 0x00) { 
                csHigh(); 
                _last_r1 = r1;
                return false; 
            }
        }

        // Inter-command separator — matches /api/sd/debug
        cmdSeparator();

        // --- CMD8  SEND_IF_COND (v2 check) ---
        {
            const uint8_t cmd[] = {0x48, 0,0,0x01,0xAA, 0x87};
            uint8_t r1 = sendCmd(cmd, 6);
            if (r1 == (R1_IDLE | 0x04)) {
                // Illegal command → SD v1
                _card_type = 1;
            } else if (r1 == R1_IDLE) {
                // Read 4-byte response (voltage + echo pattern)
                uint8_t resp[4];
                for (int i = 0; i < 4; i++) resp[i] = spiRecv();
                if (resp[2] == 0x01 && resp[3] == 0xAA) {
                    _card_type = 2;  // SD v2
                } else {
                    csHigh(); return false;
                }
            } else {
                csHigh(); return false;
            }
        }

        // Inter-command separator
        cmdSeparator();

        // --- ACMD41 loop (up to ~2 s) ---
        {
            bool ready = false;
            for (int i = 0; i < 2000; i++) {
                // CMD55  APP_CMD
                {
                    const uint8_t cmd[] = {0x77, 0,0,0,0, 0x65};
                    uint8_t r1 = sendCmd(cmd, 6);
                    if (r1 > 0x01) { csHigh(); return false; }
                }
                cmdSeparator();  // inter-command
                // ACMD41  SD_SEND_OP_COND
                {
                    uint8_t cmd[6] = {0x69, 0x40,0,0,0, 0x77};
                    uint8_t r1 = sendCmd(cmd, 6);
                    if (r1 == R1_READY) { ready = true; break; }
                    if (r1 != R1_IDLE)   { csHigh(); return false; }
                }
                cmdSeparator();  // inter-command
                delay(1);
            }
            if (!ready) { csHigh(); return false; }
        }

        cmdSeparator();  // inter-command before CMD58

        // --- CMD58  READ_OCR ---
        {
            const uint8_t cmd[] = {0x7A, 0,0,0,0, 0xFF};
            uint8_t r1 = sendCmd(cmd, 6);
            if (r1 != R1_READY) { csHigh(); return false; }
            uint8_t ocr[4];
            for (int i = 0; i < 4; i++) ocr[i] = spiRecv();
            _ocr = ((uint32_t)ocr[0] << 24) | ((uint32_t)ocr[1] << 16) |
                   ((uint32_t)ocr[2] << 8)  |  (uint32_t)ocr[3];

            // CCS bit (bit 30) → SDHC if set
            if (_card_type == 2) {
                _card_type = (_ocr & 0x40000000) ? 3 : 2;   // 3=SDHC/SDXC
            }
        }

        // leave card selected – caller can read blocks immediately
        return true;
    }

    // Read a single 512-byte block.  blockNum is either byte-addr (SDv1/v2) or
    // 512-byte-block number (SDHC).
    bool readBlock(uint32_t blockNum, uint8_t* buffer) {
        uint32_t addr = blockNum;
        if (_card_type != 3) addr <<= 9;   // non-SDHC: byte address

        uint8_t cmd[6] = {0x51,
                          (uint8_t)(addr >> 24), (uint8_t)(addr >> 16),
                          (uint8_t)(addr >> 8),  (uint8_t)(addr),
                          0xFF};
        uint8_t r1 = sendCmd(cmd, 6);
        if (r1 != R1_READY) return false;

        // wait for data token 0xFE (up to ~5000 tries)
        uint16_t wait = 0;
        uint8_t  token;
        do {
            token = spiRecv();
            if (++wait > 5000) return false;
        } while (token != 0xFE);

        for (uint16_t i = 0; i < BLOCK_SIZE; i++) buffer[i] = spiRecv();
        spiRecv(); spiRecv();  // CRC (2 bytes, discarded)

        return true;
    }

    String getCardInfo() const {
        String s;
        s.reserve(80);
        switch (_card_type) {
            case 0: s = "Unknown"; break;
            case 1: s = "SDv1 (Standard)"; break;
            case 2: s = "SDv2 (Standard Capacity)"; break;
            case 3: s = "SDHC/SDXC (High Capacity)"; break;
        }
        s += "\nOCR: 0x";
        if (_ocr & (1u << 31)) s += 'F';
        else                    s += '0';
        // print full OCR in hex
        char hex[9];
        snprintf(hex, sizeof(hex), "%08lX", (unsigned long)_ocr);
        s += hex;
        s += "\nCCS: ";
        s += (_ocr & 0x40000000) ? "Block-addressed" : "Byte-addressed";
        return s;
    }

    // ================================================================== FAT helpers
    // Parse FAT16/32 BPB from block 0 and read the root directory.
    // Returns a newline-separated listing of short-name entries.
    // (Read-only – does not mutate the card.)
    String listRootDir(uint8_t* workBuf /* at least 512 */) {
        // Read MBR / boot sector (sector 0, or block 0 for SDHC)
        if (!readBlock(0, workBuf)) return "ERR: read boot sector";

        // Check for MBR partition table – if byte 0 has a jump instr, assume BPB;
        // otherwise look for a type-06/0B/0C partition.
        uint32_t partStart = 0;
        uint8_t  mediaByte = workBuf[0];
        if (mediaByte == 0xEB || mediaByte == 0xE9) {
            partStart = 0;  // already at BPB
        } else {
            // scan partition table entries at offsets 0x1BE, 0x1CE, 0x1DE, 0x1EE
            bool found = false;
            for (int p = 0; p < 4; p++) {
                uint8_t* pe = workBuf + 0x1BE + p * 16;
                uint8_t  pt = pe[4];
                if (pt == 0x06 || pt == 0x0B || pt == 0x0C || pt == 0x04 || pt == 0x0E) {
                    partStart = pe[8] | ((uint32_t)pe[9] << 8) |
                               ((uint32_t)pe[10] << 16) | ((uint32_t)pe[11] << 24);
                    found = true;
                    break;
                }
            }
            if (!found) return "ERR: no FAT partition";
            if (!readBlock(partStart, workBuf)) return "ERR: read BPB";
        }

        // ------ parse BPB ------
        uint16_t bytesPerSec  = workBuf[11] | ((uint16_t)workBuf[12] << 8);
        uint8_t  secPerClus   = workBuf[13];
        uint16_t rsvdSecCnt  = workBuf[14] | ((uint16_t)workBuf[15] << 8);
        uint8_t  numFATs      = workBuf[16];
        uint16_t rootEntCnt   = workBuf[17] | ((uint16_t)workBuf[18] << 8);   // FAT16
        uint16_t totSec16     = workBuf[19] | ((uint16_t)workBuf[20] << 8);
        uint32_t totSec32     = workBuf[32] | ((uint32_t)workBuf[33] << 8) |
                                ((uint32_t)workBuf[34] << 16) | ((uint32_t)workBuf[35] << 24);
        uint32_t fatSz32      = workBuf[36] | ((uint32_t)workBuf[37] << 8) |
                                ((uint32_t)workBuf[38] << 16) | ((uint32_t)workBuf[39] << 24);

        if (bytesPerSec != 512) return "ERR: unsupported sector size";

        uint32_t totSec   = totSec16 ? totSec16 : totSec32;
        uint32_t fatSz    = fatSz32;
        bool     isFAT32  = false;

        if (fatSz == 0) {
            // FAT16: FAT size from old field at offset 22
            fatSz = workBuf[22] | ((uint32_t)workBuf[23] << 8);
            if (fatSz == 0) return "ERR: cannot determine FAT size";
        } else {
            // If rootEntCnt==0 → FAT32
            if (rootEntCnt == 0) isFAT32 = true;
        }

        uint32_t rootDirSectors = ((uint32_t)rootEntCnt * 32 + 511) / 512;
        uint32_t dataSec        = totSec - rsvdSecCnt - (uint32_t)numFATs * fatSz - rootDirSectors;
        uint32_t clusters       = dataSec / secPerClus;

        // Determine FAT type from cluster count
        if (!isFAT32) {
            if (clusters < 4085)      { /* FAT12 – not supported here */ }
            else if (clusters < 65525) { /* FAT16 */ }
            else                       { isFAT32 = true; }
        }

        String listing;
        listing.reserve(512);

        if (isFAT32) {
            // FAT32: root directory is a cluster chain
            uint32_t rootClus = workBuf[44] | ((uint32_t)workBuf[45] << 8) |
                               ((uint32_t)workBuf[46] << 16) | ((uint32_t)workBuf[47] << 24);
            uint32_t fatStart = partStart + rsvdSecCnt;
            uint32_t dataStart = fatStart + (uint32_t)numFATs * fatSz;
            // cluster 2 starts at dataStart sector
            uint32_t cluster = rootClus;
            uint8_t  depth = 0;
            while (cluster >= 2 && cluster < 0x0FFFFFF8 && depth < 128) {
                uint32_t sector = dataStart + (cluster - 2) * secPerClus;
                for (uint8_t s = 0; s < secPerClus; s++) {
                    if (!readBlock(sector + s, workBuf)) break;
                    if (!parseFATDirEntries(workBuf, 512, listing)) goto done32;
                }
                // follow chain
                uint32_t fatEntrySec = fatStart + (cluster * 4) / 512;
                if (!readBlock(fatEntrySec, workBuf)) break;
                uint16_t off = (cluster * 4) % 512;
                cluster = workBuf[off] | ((uint32_t)workBuf[off+1] << 8) |
                          ((uint32_t)workBuf[off+2] << 16) | ((uint32_t)workBuf[off+3] << 24);
                cluster &= 0x0FFFFFFF;
                depth++;
            }
            done32:;
        } else {
            // FAT16: root directory is at fixed location
            uint32_t rootDirStart = partStart + rsvdSecCnt + (uint32_t)numFATs * fatSz;
            for (uint32_t s = 0; s < rootDirSectors; s++) {
                if (!readBlock(rootDirStart + s, workBuf)) break;
                if (!parseFATDirEntries(workBuf, 512, listing)) break;
            }
        }

        return listing;
    }

private:
    int      _cs, _miso, _mosi, _sclk, _power_pin;
    uint8_t  _card_type;         // 1=SDv1, 2=SDv2, 3=SDHC
    uint32_t _ocr;
    uint8_t  _last_r1 = 0xFF;   // Last R1 response for diagnostics

public:
    uint8_t getLastR1() const { return _last_r1; }

    // ---- low-level SPI bit-bang ----
    void delayHalf() const { delayMicroseconds(5); }  // 10 µs full period ≈ 50 kHz at 3.3 V

    void clockPulse() const {
        // Match debug endpoint: LOW 10µs, HIGH 10µs
        digitalWrite(_sclk, LOW);   delayMicroseconds(10);
        digitalWrite(_sclk, HIGH);  delayMicroseconds(10);
    }

    void mosiHigh() const { digitalWrite(_mosi, HIGH); }
    void mosiLow()  const { digitalWrite(_mosi, LOW);  }
    void csHigh()   const { digitalWrite(_cs, HIGH);   }
    void csLow()    const { digitalWrite(_cs, LOW);    }

    // Direct GPIO access — matches proven /api/sd/debug implementation exactly
    // Uses local copies of pin numbers to avoid repeated this-> indirection
    uint8_t spiXfer(uint8_t out) const {
        const int mosi = _mosi;  // local copy — avoid this-> dereference in tight loop
        const int sclk = _sclk;
        const int miso = _miso;
        uint8_t in = 0;
        for (uint8_t bit = 0; bit < 8; bit++) {
            // MOSI setup (MSB first)
            if (out & 0x80) digitalWrite(mosi, HIGH); else digitalWrite(mosi, LOW);
            out <<= 1;
            delayMicroseconds(5);   // MOSI setup time
            // Clock: LOW then HIGH (rising edge = sample point)
            digitalWrite(sclk, LOW);  delayMicroseconds(10);
            digitalWrite(sclk, HIGH); delayMicroseconds(10);
            // Sample MISO
            in <<= 1;
            if (digitalRead(miso)) in |= 1;
        }
        return in;
    }

    // Clock-only byte read — matches /api/sd/debug exactly.
    // MOSI stays HIGH (don't care during response), just clock and sample MISO.
    uint8_t spiRecvOnly() const {
        const int mosi = _mosi;
        const int sclk = _sclk;
        const int miso = _miso;
        digitalWrite(mosi, HIGH);  // MOSI don't care = HIGH
        uint8_t in = 0;
        for (uint8_t bit = 0; bit < 8; bit++) {
            digitalWrite(sclk, LOW);  delayMicroseconds(10);
            digitalWrite(sclk, HIGH); delayMicroseconds(10);
            in <<= 1;
            if (digitalRead(miso)) in |= 1;
        }
        return in;
    }

    uint8_t spiRecv() const { return spiRecvOnly(); }
    void    spiSend(uint8_t b) const { (void)spiXfer(b); }

    // Send a 6-byte command and read R1 response (skip 0xFF fill bytes).
    // CS must already be LOW before calling this.
    uint8_t sendCmd(const uint8_t* cmd6, uint8_t len) const {
        for (uint8_t i = 0; i < len; i++) spiSend(cmd6[i]);
        // read R1 – skip up to 64 fill bytes (0xFF) — recv-only, no MOSI drive
        uint8_t r1 = 0xFF;
        for (uint8_t i = 0; i < 64; i++) {
            uint8_t r = spiRecvOnly();
            if (r != 0xFF) { r1 = r; break; }
        }
        return r1;
    }

    // Inter-command separator: 8 dummy bytes (64 clocks) + CS toggle
    // Matches /api/sd/debug — card needs Ncr separator between commands
    void cmdSeparator() const {
        for (int i = 0; i < 8; i++) spiRecvOnly();  // 8 dummy bytes
        csHigh();
        delayMicroseconds(10);
        csLow();
        delayMicroseconds(10);
    }

    // ---- FAT directory helpers ----
    static bool isShortNameChar(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == ' ' || c == '$' || c == '%' || c == '-' || c == '_' ||
               c == '~' || c == '(' || c == ')' || c == '!' || c == '@';
    }

    // Parse 512-byte directory sector into listing.  Returns false when end marker hit.
    bool parseFATDirEntries(const uint8_t* sec, uint16_t len, String& listing) const {
        for (uint16_t off = 0; off < len; off += 32) {
            uint8_t first = sec[off];
            if (first == 0x00)   return false;   // no more entries
            if (first == 0xE5)   continue;       // deleted
            if (sec[off + 11] == 0x0F) continue;  // LFN entry – skip

            // Build 8.3 name
            char name[13];
            uint8_t ni = 0;
            for (uint8_t i = 0; i < 8; i++) {
                char c = sec[off + i];
                if (c == ' ') break;
                name[ni++] = c;
            }
            if (sec[off + 8] != ' ') {
                name[ni++] = '.';
                for (uint8_t i = 8; i < 11; i++) {
                    char c = sec[off + i];
                    if (c == ' ') break;
                    name[ni++] = c;
                }
            }
            name[ni] = '\0';

            // attributes
            uint8_t attr = sec[off + 11];
            if (attr & 0x08) continue;   // volume label – skip
            if (attr & 0x10) { listing += "[DIR]  "; }
            else            { listing += "       "; }

            // file size
            if (!(attr & 0x10)) {
                uint32_t sz = sec[off+28] | ((uint32_t)sec[off+29] << 8) |
                              ((uint32_t)sec[off+30] << 16) | ((uint32_t)sec[off+31] << 24);
                listing += String(sz);
                listing += ' ';
            } else {
                listing += "0 ";
            }

            listing += name;
            listing += '\n';
        }
        return true;   // more sectors may follow
    }
};