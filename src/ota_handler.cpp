/**
 * ota_handler.cpp — OTA Firmware Update via Web Upload + ArduinoOTA
 *
 * Handles firmware .bin upload via HTTP POST multipart.
 * Uses ESP32 Update library for in-place flash writing.
 * Also supports PlatformIO espota protocol (ArduinoOTA on port 3232).
 * Maximum OTA size: 1MB (1048576 bytes).
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include "web_adapter.h"
#include "modbus_mqtt_ha_bridge.h"
#include "ota_handler.h"
#include "web_templates.h"

// ─── External web adapter (from web_server.cpp) ────────────────
extern WebInterface *WS;

// ─── Constants ────────────────────────────────────────────────
// OTA_MAX_SIZE defined in modbus_mqtt_ha_bridge.h

// ─── OTA Page ─────────────────────────────────────────────────
void handleOtaPage()
{
    String authSfx = WS->hasArg("auth") ? "?auth=" + WS->arg("auth") : "";

    String html;
    html.reserve(3000);
    html = pageStart(F("OTA Firmware"), CSS_OTA) + pageStyleEnd();
    html += navHtml(PG_OTA, authSfx);

    // Warning
    html += F("<div class='warn-box'><b>&#9888; Figyelem!</b> A firmware frissítés során az eszköz újraindul. Hibás firmware brickelheti az eszközt! Csak megbízható .bin fájlt tölts fel!</div>");

    // Upload form
    html += F("<div class='card'>"
              "<h2>&#128190; Firmware feltöltés</h2>"
              "<form id='otaForm' action='/otaupload");
    html += authSfx;
    html += F("' method='POST' enctype='multipart/form-data'>"
              "<div class='fm'><label>Firmware .bin fájl</label>"
              "<input type='file' name='firmware' accept='.bin' id='fwFile' onchange='onFileSelect()'></div>"
              "<p class='note'>Maximum méret: 1.25 MB. Fájlformátum: ESP32-S3 .bin firmware | Jelenlegi: ");
    html += String(FIRMWARE_VERSION);
    html += F("</p>"
              "<button type='submit' id='uploadBtn' disabled>&#128230; Feltöltés &amp; Frissítés</button>"
              "</form></div>");

    // Progress bar
    html += F("<div id='progress'>"
              "<h2>&#128259; Feltöltés folyamatban...</h2>"
              "<div id='progressBar'><div id='progressFill'></div></div>"
              "<div id='progressText'>0%</div>"
              "</div>");

    // Status placeholder
    html += F("<div id='status'></div>");

    // System info
    html += F("<div class='card'>"
              "<h2>&#9881; Rendszer infó</h2>");
    html += "<div class='row'><span class='key'>SDK verzió</span><span class='val'>" +
            String(ESP.getSdkVersion()) + "</span></div>";
    html += "<div class='row'><span class='key'>Chip revision</span><span class='val'>" +
            String(ESP.getChipRevision()) + "</span></div>";
    html += "<div class='row'><span class='key'>Flash méret</span><span class='val'>" +
            String(ESP.getFlashChipSize() / 1024 / 1024) + " MB</span></div>";
    html += F("<div class='row'><span class='key'>OTA max méret</span><span class='val'>1.25 MB</span></div>");
    html += F("</div>");

    // URL OTA (optional)
    html += F("<div class='card'>"
              "<h2>&#127760; OTA URL-ről</h2>"
              "<div class='url-ota'>"
              "<div class='fm'><label>Firmware URL</label>"
              "<input type='text' id='otaUrl' placeholder='https://example.com/firmware.bin'></div>"
              "<button type='button' id='otaUrlBtn' onclick='startUrlOta()'>&#128259; Letöltés &amp; Frissítés</button>"
              "</div></div>");

    // JavaScript for upload + URL OTA
    html += F("<script>"
              "function onFileSelect(){"
              "var f=document.getElementById('fwFile').files[0];"
              "document.getElementById('uploadBtn').disabled=!f;"
              "if(f&&f.size>1310720){"
              "alert('A fájl túllépi az 1.25 MB korlátot! ('+Math.round(f.size/1024)+' KB)');"
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
              "s.innerHTML='&#10004; Firmware sikeresen frissítve! Az eszköz újraindul 3 másodperc múlva...';"
              "s.style.display='block';"
              "setTimeout(function(){window.location='/'+location.search},8000);"
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
              "s.innerHTML='&#10008; Hálózati hiba a feltöltés során!';"
              "s.style.display='block';"
              "document.getElementById('uploadBtn').disabled=false;"
              "};"
              "xhr.send(new FormData(this));"
              "return false;"
              "};"
              "function startUrlOta(){"
              "var url=document.getElementById('otaUrl').value.trim();"
              "if(!url){alert('Adj meg egy URL-t!');return;}"
              "var s=document.getElementById('status');"
              "s.className='';"
              "s.style.display='block';"
              "s.innerHTML='&#128259; Firmware letöltése URL-ről...';"
              "var xh=new XMLHttpRequest();"
              "var as=location.search?('&'+location.search.substr(1)):'';"
              "xh.open('GET','/otaurl?url='+encodeURIComponent(url)+as,true);"
              "xh.onload=function(){"
              "if(xh.status==200){"
              "s.className='status-ok';"
              "s.innerHTML='&#10004; Firmware sikeresen frissítve! Újraindulás...';"
              "setTimeout(function(){window.location='/'+location.search},8000);"
              "}else{"
              "s.className='status-err';"
              "s.innerHTML='&#10008; Hiba: '+xh.responseText;"
              "}"
              "};"
              "xh.onerror=function(){"
              "s.className='status-err';"
              "s.innerHTML='&#10008; Hálózati hiba!';"
              "};"
              "xh.send();"
              "}"
              "</script>");

    html += pageFoot();
    WS->send(200, "text/html", html);
}

// ─── OTA Upload Handler ──────────────────────────────────────
void handleOtaUpload()
{
    // Auth check for OTA — must validate before processing upload
    if (!web_auth_ok())
        return;

    // Multipart upload only works on WiFi interface
    if (!WS->isUploadSupported())
    {
        WS->send(501, "text/plain", "Multipart upload not supported on LAN. Use WiFi interface or storage OTA.");
        return;
    }

    HTTPUpload &upload = WS->upload();

    if (upload.status == UPLOAD_FILE_START)
    {
        // totalSize may be 0 when curl sends multipart without Content-Length in the part header
        // Fallback: use OTA_MAX_SIZE — Update.end() will validate the actual written size
        size_t updateSize = (upload.totalSize > 0) ? upload.totalSize : OTA_MAX_SIZE;
        LOG_I("[OTA] Upload start: %s (declared: %d, using: %u)\n", upload.filename.c_str(), upload.totalSize, updateSize);

        if (upload.totalSize > 0 && upload.totalSize > OTA_MAX_SIZE)
        {
            LOG_ELN("[OTA] ERROR: Firmware too large!");
            WS->send(500, "text/plain", "A firmware túl nagy! Maximum 1.25 MB.");
            return;
        }

        if (!Update.begin(updateSize))
        {
            LOG_E("[OTA] ERROR: Update.begin failed! (%s)\n", Update.errorString());
            WS->send(500, "text/plain", String("Nem sikerült elindítani a frissítést: ") + Update.errorString());
            return;
        }
        LOG_ILN("[OTA] Update started...");

        // Disable software WDT during OTA — flash writes take priority,
        // and the WiFi stack will be starved of its normal loop cycles.
        // Hardware WDT remains active as a last-resort safety net.
        esp_task_wdt_reset();
    }
    else if (upload.status == UPLOAD_FILE_WRITE)
    {
        LOG_D("[OTA] Writing %d bytes...\n", upload.currentSize);

        // Feed the software WDT before each flash write
        esp_task_wdt_reset();

        size_t written = Update.write(upload.buf, upload.currentSize);
        if (written != upload.currentSize)
        {
            LOG_E("[OTA] ERROR: Write failed! Written %d / %d\n", written, upload.currentSize);
            WS->send(500, "text/plain", String("Írási hiba: ") + Update.errorString());
            return;
        }

        // CRITICAL: Give WiFi/TCP stack time to process ACKs and update
        // the TCP receive window. Without this, the client's send window
        // fills up → TCP stall → client timeout → WDT reset at ~768KB.
        yield();
        delay(5); // 5ms pause lets lwIP process pending packets
        esp_task_wdt_reset(); // Re-feed WDT after delay
    }
    else if (upload.status == UPLOAD_FILE_END)
    {
        LOG_ILN("[OTA] Upload complete, finalizing...");
        esp_task_wdt_reset();
        if (Update.end(true))
        {
            LOG_I("[OTA] SUCCESS! Firmware updated (%d bytes). Restarting...\n", Update.size());
            WS->send(200, "text/plain", "OK");
            delay(3000);
            eth_hard_reset_and_restart();
        }
        else
        {
            LOG_E("[OTA] ERROR: Update.end failed! (%s)\n", Update.errorString());
            WS->send(500, "text/plain", String("Frissítés sikertelen: ") + Update.errorString());
        }
    }
    else if (upload.status == UPLOAD_FILE_ABORTED)
    {
        LOG_ELN("[OTA] Upload aborted!");
        Update.abort();
        WS->send(500, "text/plain", "Feltöltés megszakítva!");
    }
}

// ─── HTTP GET OTA: ESP32 downloads firmware from URL ────────────
// Solves the TCP buffer overflow problem with synchronous WebServer upload.
// The ESP32 controls the download pace — no zero-window stall possible.
void handleOtaFromURL()
{
    if (!web_auth_ok())
        return;

    if (!WS->hasArg("url"))
    {
        WS->send(400, "application/json",
                  "{\"error\":\"missing 'url' parameter\"}");
        return;
    }

    String fwUrl = WS->arg("url");
    LOG_I("[OTA-URL] Downloading firmware from: %s\n", fwUrl.c_str());

    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(60000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // Follow GitHub 302 redirects
    http.setRedirectLimit(3);

    if (!http.begin(fwUrl))
    {
        LOG_ELN("[OTA-URL] Failed to connect to URL");
        WS->send(500, "application/json",
                 "{\"error\":\"connection_failed\"}");
        return;
    }

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK)
    {
        LOG_E("[OTA-URL] HTTP error: %d\n", httpCode);
        http.end();
        WS->send(500, "application/json",
                 "{\"error\":\"http_error\",\"code\":" + String(httpCode) + "}");
        return;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0 || contentLength > OTA_MAX_SIZE)
    {
        LOG_E("[OTA-URL] Invalid content length: %d\n", contentLength);
        http.end();
        WS->send(500, "application/json",
                 "{\"error\":\"invalid_size\",\"size\":" + String(contentLength) + "}");
        return;
    }

    if (!Update.begin(contentLength))
    {
        LOG_E("[OTA-URL] Update.begin failed: %s\n", Update.errorString());
        http.end();
        WS->send(500, "application/json",
                 "{\"error\":\"update_begin_failed\"}");
        return;
    }

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[4096];
    size_t written = 0;
    size_t totalWritten = 0;

    LOG_I("[OTA-URL] Starting download: %d bytes\n", contentLength);

    while (totalWritten < (size_t)contentLength)
    {
        esp_task_wdt_reset(); // Feed WDT during download+write
        size_t available = stream->available();
        if (available)
        {
            int readBytes = stream->readBytes(buf, min(available, sizeof(buf)));
            written = Update.write(buf, readBytes);
            if (written != (size_t)readBytes)
            {
                LOG_E("[OTA-URL] Write error at %d bytes\n", totalWritten);
                Update.abort();
                http.end();
                WS->send(500, "application/json",
                         "{\"error\":\"write_failed\",\"at\":" + String(totalWritten) + "}");
                return;
            }
            totalWritten += written;

            // Progress log every 10%
            static uint8_t lastPct = 0;
            uint8_t pct = (totalWritten * 100) / contentLength;
            if (pct >= lastPct + 10)
            {
                LOG_I("[OTA-URL] Progress: %d%% (%d/%d bytes)\n",
                      pct, totalWritten, contentLength);
                lastPct = pct;
            }
        }

        // CRITICAL: yield() between reads lets WiFi/TCP stack process ACKs
        yield();
        delay(1); // Small delay to prevent tight loop consuming CPU
    }

    http.end();

    if (Update.end(true))
    {
        LOG_I("[OTA-URL] SUCCESS! Firmware updated (%d bytes). Rebooting...\n", totalWritten);
        char okBuf[80];
        snprintf(okBuf, sizeof(okBuf), "{\"status\":\"ok\",\"bytes\":%d,\"rebooting\":true}", totalWritten);
        WS->send(200, "application/json", okBuf);
        delay(3000);
        eth_hard_reset_and_restart();
    }
    else
    {
        LOG_E("[OTA-URL] Update.end failed: %s\n", Update.errorString());
        WS->send(500, "application/json",
                 "{\"error\":\"update_end_failed\"}");
    }
}

// ─── Init & Loop ──────────────────────────────────────────────
void ota_init()
{
    // ── ArduinoOTA (PlatformIO espota protocol, port 3232) ──
    ArduinoOTA.setHostname("modbusmqtt");
    ArduinoOTA.setPort(3232);

    ArduinoOTA.onStart([]() {
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
        LOG_I("[OTA] ArduinoOTA start: %s\n", type.c_str());
    });
    ArduinoOTA.onEnd([]() { LOG_ILN("[OTA] ArduinoOTA end — rebooting"); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        static uint8_t last_pct = 0;
        uint8_t pct = (progress * 100) / total;
        if (pct != last_pct && pct % 10 == 0)
        {
            LOG_I("[OTA] Progress: %u%%\n", pct);
            last_pct = pct;
        }
    });
    ArduinoOTA.onError([](ota_error_t error) {
        LOG_E("[OTA] ArduinoOTA error[%u]: ", error);
        if (error == OTA_AUTH_ERROR)
            LOG_ELN("Auth Failed");
        else if (error == OTA_BEGIN_ERROR)
            LOG_ELN("Begin Failed");
        else if (error == OTA_CONNECT_ERROR)
            LOG_ELN("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR)
            LOG_ELN("Receive Failed");
        else if (error == OTA_END_ERROR)
            LOG_ELN("End Failed");
    });

    ArduinoOTA.begin();
    LOG_ILN("[OTA] ArduinoOTA ready on port 3232");

    // Routes for web-based OTA are registered in web_server_init()
    LOG_ILN("[OTA] Web upload handler ready");
}

// ─── OTA Raw Binary Upload (curl-friendly) ────────────────────
// POST /api/ota/raw — body = raw firmware.bin, Content-Length required
// Only registered on WiFi web server (uses 'web' object directly)
void handleApiOtaRaw()
{
    // Auth check via web server credentials
    if (cfg.web_auth && strlen(cfg.web_pass) > 0)
    {
        if (!WS->authenticate("admin", cfg.web_pass) &&
            !(WS->hasArg("auth") && WS->arg("auth") == String(cfg.web_pass)))
        {
            WS->requestAuthentication();
            return;
        }
    }

    // Get firmware size from query param (required: ?size=NNNN)
    // WebServer doesn't parse Content-Length header by default
    int contentLen = WS->arg("size").toInt();
    LOG_I("[OTA-RAW] Requested size: %d\n", contentLen);

    if (contentLen <= 0 || contentLen > OTA_MAX_SIZE)
    {
        WS->send(400, "application/json",
                 "{\"error\":\"invalid_size\",\"size\":" + String(contentLen) + "}");
        return;
    }

    if (!Update.begin((size_t)contentLen))
    {
        LOG_E("[OTA-RAW] Update.begin failed: %s\n", Update.errorString());
        WS->send(500, "application/json",
                 "{\"error\":\"update_begin_failed\",\"detail\":\"" + String(Update.errorString()) + "\"}");
        return;
    }

    LOG_ILN("[OTA-RAW] Update started...");
    disableLoopWDT();

    uint8_t buf[4096];
    size_t totalWritten = 0;
    size_t remaining = (size_t)contentLen;

    while (remaining > 0)
    {
        size_t toRead = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
        size_t avail = WS->clientStream().available();
        if (avail < toRead)
        {
            // Wait for data with timeout
            uint32_t t0 = millis();
            while (WS->clientStream().available() < (int)(toRead) && millis() - t0 < 5000)
            {
                delay(1);
            }
            avail = WS->clientStream().available();
            if (avail == 0)
            {
                LOG_ELN("[OTA-RAW] Data timeout!");
                Update.abort();
                enableLoopWDT();
                WS->send(500, "application/json", "{\"error\":\"data_timeout\"}");
                return;
            }
        }
        size_t bytesRead = WS->clientStream().readBytes(buf, toRead);
        if (bytesRead == 0)
        {
            LOG_ELN("[OTA-RAW] Read 0 bytes — connection lost?");
            Update.abort();
            enableLoopWDT();
            WS->send(500, "application/json", "{\"error\":\"read_failed\"}");
            return;
        }
        size_t written = Update.write(buf, bytesRead);
        if (written != bytesRead)
        {
            LOG_E("[OTA-RAW] Write mismatch: written=%d expected=%d\n", written, bytesRead);
            Update.abort();
            enableLoopWDT();
            WS->send(500, "application/json", "{\"error\":\"write_failed\"}");
            return;
        }
        totalWritten += written;
        remaining -= bytesRead;
        if (totalWritten % 65536 == 0)
            LOG_I("[OTA-RAW] %d / %d\n", totalWritten, contentLen);
    }

    if (Update.end(true))
    {
        LOG_ILN("[OTA-RAW] ✅ Update OK! Rebooting...");
        enableLoopWDT();
        WS->send(200, "application/json", "{\"ok\":true,\"size\":" + String(totalWritten) + "}");
        delay(500);
        eth_hard_reset_and_restart();
    }
    else
    {
        LOG_E("[OTA-RAW] Update.end failed: %s\n", Update.errorString());
        enableLoopWDT();
        WS->send(500, "application/json",
                 "{\"error\":\"update_end_failed\",\"detail\":\"" + String(Update.errorString()) + "\"}");
    }
}

void ota_loop()
{
    ArduinoOTA.handle(); // Process PlatformIO espota requests (safe after ota_init())
}