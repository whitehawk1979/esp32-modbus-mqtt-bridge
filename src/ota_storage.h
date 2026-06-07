/**
 * ota_storage.h — Storage-based OTA: upload firmware to LittleFS, then self-update
 *
 * Flow:
 *  1. POST /otaupload → save firmware.bin to /ota/firmware.bin on LittleFS
 *  2. User clicks "Apply Update" or auto-apply after upload
 *  3. ESP32 reads /ota/firmware.bin from LittleFS and flashes to app1 partition
 *  4. Reboot into new firmware
 *
 * Benefits over direct OTA:
 *  - No TCP buffer overflow (upload → filesystem, not flash)
 *  - MD5 verification before flashing
 *  - Resume-capable (partial upload can be retried)
 *  - 10MB LittleFS can hold firmware + have room for checksums
 *
 * Uses: LittleFS (storage partition), Update library
 */

#ifndef OTA_STORAGE_H
#define OTA_STORAGE_H

#include <Arduino.h>

// ─── Init: ensure /ota/ directory exists ─────────────────────
void ota_storage_init();

// ─── Web page: OTA upload with storage-first approach ────────
void handleOtaStoragePage();

// ─── Upload handler: save firmware to /ota/firmware.bin ─────
// Multipart upload → LittleFS file, NOT direct flash
void handleOtaStorageUpload();

// ─── Apply stored firmware: read from LittleFS → flash → reboot
// Called via POST /ota/apply or UI button
void handleOtaStorageApply();

// ─── Check stored firmware info (size, MD5, version) ────────
// Returns JSON: {size, md5, has_update, current_version}
void handleOtaStorageInfo();

// ─── Cancel: delete stored firmware ─────────────────────────
void handleOtaStorageCancel();

// ─── Constants ───────────────────────────────────────────────
#define OTA_STORAGE_PATH "/ota/firmware.bin"
#define OTA_STORAGE_MD5_PATH "/ota/firmware.md5"
#define OTA_STORAGE_VERSION_PATH "/ota/firmware.version"
#define OTA_STORAGE_DIR "/ota"

#endif // OTA_STORAGE_H