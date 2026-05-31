/**
 * ota_handler.h — OTA Firmware Update Handler
 */

#ifndef OTA_HANDLER_H
#define OTA_HANDLER_H

#include <Arduino.h>

void ota_init();
void ota_loop();
void handleOtaPage();
void handleOtaUpload();
void handleOtaFromURL();

#endif