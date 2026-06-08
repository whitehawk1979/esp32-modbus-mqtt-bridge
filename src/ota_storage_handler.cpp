/**
 * ota_storage_handler.cpp — Storage-based OTA for ESP32-S3-ETH
 *
 * Uploads firmware to LittleFS first, then flashes from storage.
 * This avoids TCP buffer overflow during direct OTA flash.
 *
 * Flow:
 *  1. Upload .bin → save to /ota/firmware.bin on LittleFS
 *  2. Verify size + optional MD5
 *  3. User clicks "Apply" → read from LittleFS → Update.write() → reboot
 */

#include <Arduino.h>
#include <WebServer.h>
#include <Update.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include "modbus_mqtt_ha_bridge.h"
#include "web_templates.h"

#ifdef USE_STORAGE

#include "ota_storage.h"
#include "storage_handler.h"
#include "web_adapter.h"

// ─── Web adapter (defined in web_server.cpp) ────────────────
// WS points to the active WebInterface (WiFi or LAN depending on context)
extern WebInterface *WS;
extern WebServer web; // Needed for multipart upload (WiFi-only handler)

// ─── State ────────────────────────────────────────────────────
static size_t ota_upload_size = 0;
static bool ota_upload_active = false;

// ─── Init ────────────────────────────────────────────────────
void ota_storage_init()
{
    if (!storage_mounted())
        return;

    // Ensure /ota directory exists
    if (!LittleFS.exists(OTA_STORAGE_DIR))
    {
        LittleFS.mkdir(OTA_STORAGE_DIR);
        LOG_I("[OTA-STORAGE] Created %s directory\n", OTA_STORAGE_DIR);
    }

    LOG_I("[OTA-STORAGE] Ready (storage-based OTA)\n");
}

// ─── Helper: check if stored firmware exists ────────────────
static bool ota_has_stored_firmware()
{
    return LittleFS.exists(OTA_STORAGE_PATH);
}

// ─── Helper: get stored firmware size ───────────────────────
static size_t ota_stored_firmware_size()
{
    if (!ota_has_stored_firmware())
        return 0;
    File f = LittleFS.open(OTA_STORAGE_PATH, "r");
    size_t sz = f.size();
    f.close();
    return sz;
}

// ─── OTA Storage Page ────────────────────────────────────────
void handleOtaStoragePage()
{
    if (!web_auth_ok())
        return;

    String authSfx = WS->hasArg("auth") ? "?auth=" + WS->arg("auth") : "";

    String html;
    html.reserve(8192);
    html = pageStart(F("OTA Firmware"), CSS_OTA) + pageStyleEnd();
    html += navHtml(PG_OTA, authSfx);

    // ── Warning ──
    html += F("<div class='warn-box'><b>&#9888; Figyelem!</b> A firmware frissítés során az eszköz újraindul. "
              "Hibás firmware brickelheti az eszközt!</div>");

    // ── Step 1: Upload ──
    html += F("<div class='card'><h2>&#128190; 1. Feltöltés</h2>"
              "<p class='note'>A firmware fájl először a belső tárhelyre mentődik, majd ellenőrzés után flash-elődik. "
              "Jelenlegi verzió: ");
    html += String(FIRMWARE_VERSION);
    html += F("</p>"
              "<form id='otaForm' action='/otastorageupload");
    html += authSfx;
    html += F("' method='POST' enctype='multipart/form-data'>"
              "<div class='fm'><label>Firmware .bin fájl</label>"
              "<input type='file' name='firmware' accept='.bin' id='fwFile' onchange='onFileSelect()'></div>"
              "<p class='note'>Maximum 3 MB. Formátum: ESP32-S3 .bin firmware</p>"
              "<button type='submit' id='uploadBtn' disabled>&#128228; Feltöltés tárhelyre</button>"
              "</form></div>");

    // ── Progress bar ──
    html += F("<div id='progress'>"
              "<h2>&#128259; Feltöltés folyamatban...</h2>"
              "<div id='progressBar'><div id='progressFill'></div></div>"
              "<div id='progressText'>0%</div>"
              "</div>");

    // ── Step 2: Stored firmware info + Apply ──
    bool hasStored = ota_has_stored_firmware();
    size_t storedSize = ota_stored_firmware_size();

    html += F("<div class='card'><h2>&#128230; 2. Tárolt firmware</h2>");
    if (hasStored)
    {
        html += F("<div class='row'><span class='key'>Fájl</span><span class='val'>/ota/firmware.bin</span></div>"
                  "<div class='row'><span class='key'>Méret</span><span class='val'>");
        html += String(storedSize) + F(" bytes (") + String(storedSize / 1024) + F(" KB)</span></div>");
        html += F("<div class='row'><span class='key'>Állapot</span><span class='val on'>&#10004; Készen alkalmazásra</span></div>");

        html += F("<div style='margin-top:12px;display:flex;gap:8px;'>"
                  "<button type='button' onclick='applyUpdate()' style='background:#238636;padding:10px 24px;font-size:16px;border:none;border-radius:6px;color:white;cursor:pointer'>"
                  "&#128260; Alkalmazás &amp; Újraindítás</button>"
                  "<button type='button' onclick='cancelUpdate()' style='background:#da3633;padding:10px 24px;font-size:16px;border:none;border-radius:6px;color:white;cursor:pointer'>"
                  "&#128465; Törlés</button>"
                  "</div>");
    }
    else
    {
        html += F("<div class='row'><span class='key'>Állapot</span><span class='val off'>&#10060; Nincs tárolt firmware</span></div>"
                  "<p class='note'>Tölts fel egy .bin fájlt a fenti űrlapon.</p>");
    }
    html += F("</div>");

    // ── Status placeholder ──
    html += F("<div id='status'></div>");

    // ── System info ──
    html += F("<div class='card'><h2>&#9881; Rendszer infó</h2>");
    html += "<div class='row'><span class='key'>SDK verzió</span><span class='val'>" +
            String(ESP.getSdkVersion()) + "</span></div>";
    html += "<div class='row'><span class='key'>Chip revision</span><span class='val'>" +
            String(ESP.getChipRevision()) + "</span></div>";
    html += "<div class='row'><span class='key'>Flash méret</span><span class='val'>" +
            String(ESP.getFlashChipSize() / 1024 / 1024) + " MB</span></div>";
    html += F("<div class='row'><span class='key'>OTA max méret</span><span class='val'>3 MB</span></div>");
    html += "<div class='row'><span class='key'>Storage szabad</span><span class='val'>" +
            String((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024) + " KB</span></div>";
    html += F("</div>");

    // ── JavaScript ──
    html += F("<script>"
              "var wifiIP='");
    html += WiFi.localIP().toString();
    html += F("';"
              "function onFileSelect(){"
              "var f=document.getElementById('fwFile').files[0];"
              "document.getElementById('uploadBtn').disabled=!f;"
              "if(f&&f.size>3145728){"
              "alert('A fájl túl nagy! Max 3 MB. ('+Math.round(f.size/1024)+' KB)');"
              "document.getElementById('uploadBtn').disabled=true;"
              "}"
              "}"
              "document.getElementById('otaForm').onsubmit=function(){"
              "document.getElementById('progress').style.display='block';"
              "document.getElementById('uploadBtn').disabled=true;"
              "var xhr=new XMLHttpRequest();"
              "xhr.open('POST',this.action,true);"
              "xhr.upload.onprogress=function(e){"
              "if(e.lengthComputable){"
              "var pct=Math.round(e.loaded/e.total*100);"
              "document.getElementById('progressFill').style.width=pct+'%';"
              "document.getElementById('progressText').textContent=pct+'% ('+Math.round(e.loaded/1024)+' / '+Math.round(e.total/1024)+' KB)';"
              "}"
              "};"
              "xhr.onload=function(){"
              "document.getElementById('progress').style.display='none';"
              "var s=document.getElementById('status');"
              "if(xhr.status==200){"
              "s.className='status-ok';"
              "s.innerHTML='&#10004; Firmware feltöltve a tárhelyre! Kattints az Alkalmazás gombra.';"
              "s.style.display='block';"
              "setTimeout(function(){location.reload()},2000);"
              "}else if(xhr.status==501){"
              "s.className='status-err';"
              "s.innerHTML='&#10008; A feltöltés LAN-on nem támogatott. <a href=\"http://' + wifiIP + '/ota\">Használd a WiFicímet</a> a feltöltéshez.';"
              "s.style.display='block';"
              "document.getElementById('uploadBtn').disabled=false;"
              "}else{"
              "s.className='status-err';"
              "s.innerHTML='&#10008; Hiba: '+xhr.responseText;"
              "s.style.display='block';"
              "document.getElementById('uploadBtn').disabled=false;"
              "}"
              "};"
              "xhr.onerror=function(){"
              "document.getElementById('progress').style.display='none';"
              "var s=document.getElementById('status');"
              "s.className='status-err';"
              "s.innerHTML='&#10008; Hálózati hiba!';"
              "s.style.display='block';"
              "document.getElementById('uploadBtn').disabled=false;"
              "};"
              "xhr.send(new FormData(this));"
              "return false;"
              "};"
              "function applyUpdate(){"
              "if(!confirm('Biztosan alkalmazod a firmware-t? Az eszköz újraindul!'))return;"
              "var s=document.getElementById('status');"
              "s.className='';"
              "s.style.display='block';"
              "s.innerHTML='&#128260; Firmware alkalmazása és flash-elése...';"
              "var as=location.search?('&'+location.search.substr(1)):'';"
              "var xhr=new XMLHttpRequest();"
              "xhr.open('POST','/ota/apply'+as,true);"
              "xhr.setRequestHeader('Content-Type','application/x-www-form-urlencoded');"
              "xhr.timeout=30000;"
              "xhr.onload=function(){"
              "if(xhr.status==200){"
              "s.className='status-ok';"
              "s.innerHTML='&#10004; Firmware sikeresen flash-elve! Újraindulás...';"
              "setTimeout(function(){location.reload()},10000);"
              "}else{"
              "s.className='status-err';"
              "s.innerHTML='&#10008; Flash hiba: '+xhr.responseText;"
              "}"
              "};"
              "xhr.onerror=function(){"
              "s.className='status-err';"
              "s.innerHTML='&#10008; Hálózati hiba a flash-elés során! (Az eszköz valószínűleg újraindult)';"
              "setTimeout(function(){location.reload()},5000);"
              "};"
              "xhr.send();"
              "}"
              "function cancelUpdate(){"
              "var as=location.search?('&'+location.search.substr(1)):'';"
              "var xhr=new XMLHttpRequest();"
              "xhr.open('POST','/ota/cancel'+as,true);"
              "xhr.onload=function(){location.reload()};"
              "xhr.send();"
              "}"
              "</script>");

    html += pageFoot();
    WS->send(200, "text/html", html);
}

// ─── Upload handler: save to LittleFS ────────────────────────
// NOTE: This handler is registered on WiFi WebServer ONLY (web.on).
// multipart upload is NOT supported on LAN (EthWebServer has no upload()).
// Therefore we use `web.` directly instead of `WS->` here.
void handleOtaStorageUpload()
{
    if (!web_auth_ok())
        return;

    // Upload not supported on LAN (EthWebServer has no multipart parser)
    if (!WS->isUploadSupported())
    {
        WS->send(501, "application/json", "{\"ok\":false,\"error\":\"upload_requires_wifi\"}");
        return;
    }

    if (!storage_mounted())
    {
        WS->send(500, "text/plain", "Storage not mounted!");
        return;
    }

    HTTPUpload &upload = WS->upload();

    if (upload.status == UPLOAD_FILE_START)
    {
        ota_upload_size = 0;
        ota_upload_active = true;

        if (upload.totalSize > 0 && upload.totalSize > OTA_MAX_SIZE)
        {
            LOG_ELN("[OTA-STORAGE] File too large!");
            WS->send(500, "text/plain", "A firmware túl nagy! Max 3 MB.");
            ota_upload_active = false;
            return;
        }

        LOG_I("[OTA-STORAGE] Upload start: %s (%d bytes)\n",
              upload.filename.c_str(), upload.totalSize);

        // Delete old firmware file if exists
        if (LittleFS.exists(OTA_STORAGE_PATH))
            LittleFS.remove(OTA_STORAGE_PATH);

        // Ensure /ota directory exists
        if (!LittleFS.exists(OTA_STORAGE_DIR))
            LittleFS.mkdir(OTA_STORAGE_DIR);
    }
    else if (upload.status == UPLOAD_FILE_WRITE)
    {
        // Open file for append (write mode creates new, append mode adds)
        File f = LittleFS.open(OTA_STORAGE_PATH, "a");
        if (!f)
        {
            LOG_ELN("[OTA-STORAGE] Failed to open file for writing!");
            WS->send(500, "text/plain", "Nem sikerült megnyitni a fájlt írásra!");
            ota_upload_active = false;
            return;
        }
        size_t written = f.write(upload.buf, upload.currentSize);
        f.close();
        ota_upload_size += written;

        if (written != upload.currentSize)
        {
            LOG_E("[OTA-STORAGE] Write error: %d / %d\n", written, upload.currentSize);
            WS->send(500, "text/plain", "Írási hiba a tárhelyre!");
            ota_upload_active = false;
            return;
        }

        // CRITICAL: Give WiFi/TCP stack time to process ACKs and update
        // the TCP receive window. Without this, the client's send window
        // fills up → TCP stall → client timeout → WDT reset.
        esp_task_wdt_reset();
        yield();
        delay(5); // 5ms pause lets lwIP process pending packets

        // Progress log every 64KB
        if (ota_upload_size % 65536 < upload.currentSize)
            LOG_I("[OTA-STORAGE] Written %d bytes\n", ota_upload_size);
    }
    else if (upload.status == UPLOAD_FILE_END)
    {
        LOG_I("[OTA-STORAGE] Upload complete: %d bytes\n", ota_upload_size);
        ota_upload_active = false;
        WS->send(200, "text/plain", String("OK:") + String(ota_upload_size));
    }
    else if (upload.status == UPLOAD_FILE_ABORTED)
    {
        LOG_ELN("[OTA-STORAGE] Upload aborted!");
        ota_upload_active = false;
        // Clean up partial file
        if (LittleFS.exists(OTA_STORAGE_PATH))
            LittleFS.remove(OTA_STORAGE_PATH);
        WS->send(500, "text/plain", "Feltöltés megszakítva!");
    }
}

// ─── Apply: read from LittleFS → flash → reboot ────────────
void handleOtaStorageApply()
{
    if (!web_auth_ok())
        return;

    if (!storage_mounted())
    {
        WS->send(500, "application/json", "{\"error\":\"storage_not_mounted\"}");
        return;
    }

    if (!LittleFS.exists(OTA_STORAGE_PATH))
    {
        WS->send(404, "application/json", "{\"error\":\"no_firmware\"}");
        return;
    }

    File f = LittleFS.open(OTA_STORAGE_PATH, "r");
    size_t fileSize = f.size();
    LOG_I("[OTA-STORAGE] Applying firmware: %d bytes\n", fileSize);

    if (fileSize == 0 || fileSize > OTA_MAX_SIZE)
    {
        f.close();
        WS->send(400, "application/json", "{\"error\":\"invalid_size\"}");
        return;
    }

    if (!Update.begin(fileSize))
    {
        f.close();
        String err = Update.errorString();
        LOG_E("[OTA-STORAGE] Update.begin failed: %s\n", err.c_str());
        WS->send(500, "application/json",
                 "{\"error\":\"update_begin_failed\",\"detail\":\"" + err + "\"}");
        return;
    }

    // Read from LittleFS in chunks → write to flash
    uint8_t buf[4096];
    size_t totalWritten = 0;

    while (totalWritten < fileSize)
    {
        esp_task_wdt_reset();
        size_t toRead = min(sizeof(buf), fileSize - totalWritten);
        size_t bytesRead = f.read(buf, toRead);

        if (bytesRead == 0)
        {
            LOG_ELN("[OTA-STORAGE] Read error from LittleFS!");
            Update.abort();
            f.close();
            WS->send(500, "application/json", "{\"error\":\"read_failed\"}");
            return;
        }

        size_t written = Update.write(buf, bytesRead);
        if (written != bytesRead)
        {
            LOG_E("[OTA-STORAGE] Flash write error at %d\n", totalWritten);
            Update.abort();
            f.close();
            WS->send(500, "application/json", "{\"error\":\"write_failed\"}");
            return;
        }

        totalWritten += written;
        yield();
    }

    f.close();

    if (Update.end(true))
    {
        LOG_I("[OTA-STORAGE] SUCCESS! Flashed %d bytes. Rebooting...\n", totalWritten);
        WS->send(200, "application/json",
                 "{\"ok\":true,\"size\":" + String(totalWritten) + ",\"rebooting\":true}");
        delay(1000);

        // Clean up stored firmware after successful flash
        if (LittleFS.exists(OTA_STORAGE_PATH))
            LittleFS.remove(OTA_STORAGE_PATH);

        eth_hard_reset_and_restart();
    }
    else
    {
        String err = Update.errorString();
        LOG_E("[OTA-STORAGE] Update.end failed: %s\n", err.c_str());
        WS->send(500, "application/json",
                 "{\"error\":\"update_end_failed\",\"detail\":\"" + err + "\"}");
    }
}

// ─── Info: stored firmware status ────────────────────────────
void handleOtaStorageInfo()
{
    if (!web_auth_ok())
        return;

    JsonDocument doc(PsramAllocator::instance());

    if (ota_has_stored_firmware())
    {
        File f = LittleFS.open(OTA_STORAGE_PATH, "r");
        doc["has_update"] = true;
        doc["size"] = f.size();
        doc["size_kb"] = f.size() / 1024;
        f.close();
    }
    else
    {
        doc["has_update"] = false;
        doc["size"] = 0;
    }

    doc["current_version"] = FIRMWARE_VERSION;
    doc["ota_max_size"] = OTA_MAX_SIZE;
    doc["storage_free_kb"] = (LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024;

    String json;
    serializeJson(doc, json);
    WS->send(200, "application/json", json);
}

// ─── Cancel: delete stored firmware ──────────────────────────
void handleOtaStorageCancel()
{
    if (!web_auth_ok())
        return;

    if (LittleFS.exists(OTA_STORAGE_PATH))
    {
        LittleFS.remove(OTA_STORAGE_PATH);
        LOG_ILN("[OTA-STORAGE] Stored firmware deleted");
    }

    WS->send(200, "application/json", "{\"ok\":true,\"deleted\":true}");
}

#endif // USE_STORAGE