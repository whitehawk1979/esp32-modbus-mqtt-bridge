/**
 * storage_handler.h — LittleFS Storage on 10MB Flash Partition
 *
 * Waveshare ESP32-S3-ETH V1.0 Modbus-MQTT Bridge
 * Provides persistent file storage on the 'storage' flash partition
 * using LittleFS filesystem.
 *
 * Partition: storage @ 0x620000, size 0x9E0000 (~10MB)
 * NOTE: Partition type must be 'spiffs' (0x82) for LittleFS —
 *       not 'fat' (0x01). See partitions_16mb.csv.
 *
 * Compile with -DUSE_STORAGE to enable.
 */

#ifndef STORAGE_HANDLER_H
#define STORAGE_HANDLER_H

#include "modbus_mqtt_ha_bridge.h"

#ifdef USE_STORAGE

// ─── Initialization ────────────────────────────────────────────
bool storage_init();    // Mount LittleFS, format if first use, create dirs

// ─── Directory listing ────────────────────────────────────────
// Returns JSON array of files: [{"name":"...","size":123},...]
bool storage_list_dir(const char *path, String &json_output);

// ─── File operations ──────────────────────────────────────────
bool storage_read_file(const char *path, String &content);
bool storage_write_file(const char *path, const char *content, size_t len);
bool storage_delete_file(const char *path);
bool storage_exists(const char *path);

// ─── Space info ───────────────────────────────────────────────
uint64_t storage_total_bytes();
uint64_t storage_used_bytes();

#endif // USE_STORAGE

#endif // STORAGE_HANDLER_H