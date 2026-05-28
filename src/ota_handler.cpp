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
#include "modbus_mqtt_ha_bridge.h"
#include "ota_handler.h"

// ─── External web server (from web_server.cpp) ────────────────
extern WebServer web;

// ─── Constants ────────────────────────────────────────────────
#define OTA_MAX_SIZE 1310720  // 1.25MB max firmware size (app partition - boot partition)

// ─── OTA Page ─────────────────────────────────────────────────
void handleOtaPage() {
    String html = R"rawliteral(<!DOCTYPE html><html lang="hu"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Modbus-MQTT Bridge — OTA Firmware</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#0d1117;color:#c9d1d9;padding:12px;font-size:15px}
h1{color:#58a6ff;font-size:1.3em;margin:8px 0}
h2{color:#f0883e;background:#161b22;padding:8px 12px;border-radius:6px;margin:16px 0 8px;font-size:1.05em}
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px;margin:6px 0}
.row{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #21262d}
.row:last-child{border:none}
.key{color:#8b949e}.val{color:#c9d1d9;font-weight:600}
.nav{display:flex;gap:8px;margin:8px 0}
.nav a{background:#21262d;color:#58a6ff;padding:8px 16px;border-radius:6px;text-decoration:none;font-size:14px}
.nav a:hover{background:#30363d}
.nav a.active{background:#238636;color:white}
.fm{margin-bottom:8px}
label{display:block;font-weight:600;color:#7ee787;margin-bottom:2px;font-size:14px}
input[type=file]{width:100%;padding:8px;border:1px solid #30363d;border-radius:4px;background:#0d1117;color:#c9d1d9;font-size:15px}
button{background:#238636;color:white;border:none;padding:10px 20px;border-radius:6px;font-size:16px;cursor:pointer;margin:10px 0}
button:hover{background:#2ea043}
button:disabled{background:#484f58;cursor:not-allowed}
.note{color:#8b949e;font-size:12px;margin:2px 0 8px}
.warn-box{background:#161b22;border:1px solid #f85149;border-radius:6px;padding:12px;margin:8px 0}
.warn-box b{color:#f85149}
.foot{text-align:center;color:#484f58;font-size:11px;margin-top:16px;border-top:1px solid #21262d;padding-top:8px}
#progress{display:none;margin:12px 0}
#progressBar{width:100%;height:24px;background:#21262d;border-radius:4px;overflow:hidden}
#progressFill{width:0%;height:100%;background:#238636;transition:width 0.3s}
#progressText{text-align:center;color:#c9d1d9;margin:4px 0;font-size:14px}
#status{margin:12px 0;padding:12px;border-radius:6px;display:none}
.status-ok{background:#0d1117;border:1px solid #3fb950;color:#3fb950}
.status-err{background:#0d1117;border:1px solid #f85149;color:#f85149}
</style></head><body>
<h1>&#128230; OTA Firmware Frissítés</h1>
<div class="nav"><a href="/">Státusz</a><a href="/config">Beállítások</a><a href="/pins">Pinek</a><a href="/modules">Modulok</a><a class="active" href="/ota">OTA</a><a href="/admin">Admin</a></div>

<div class="warn-box"><b>&#9888; Figyelem!</b> A firmware frissítés során az eszköz újraindul. Hibás firmware brickelheti az eszközt! Csak megbízható .bin fájlt tölts fel!</div>

<div class="card">
<h2>&#128190; Firmware feltöltés</h2>
<form id="otaForm" action="/otaupload" method="POST" enctype="multipart/form-data">
<div class="fm"><label>Firmware .bin fájl</label><input type="file" name="firmware" accept=".bin" id="fwFile" onchange="onFileSelect()"></div>
<p class="note">Maximum méret: 1.25 MB. Fájlformátum: ESP32-S3 .bin firmware | Jelenlegi: )rawliteral";

    html += String(FIRMWARE_VERSION);

    html += R"rawliteral(</p>
<button type="submit" id="uploadBtn" disabled>&#128230; Feltöltés & Frissítés</button>
</form>
</div>

<div id="progress">
<h2>&#128259; Feltöltés folyamatban...</h2>
<div id="progressBar"><div id="progressFill"></div></div>
<div id="progressText">0%</div>
</div>

<div id="status"></div>

<div class="card">
<h2>&#9881; Rendszer infó</h2>
<div class="row"><span class="key">SDK verzió</span><span class="val">)rawliteral";

    html += String(ESP.getSdkVersion()) + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Chip revision</span><span class=\"val\">" + String(ESP.getChipRevision()) + "</span></div>";
    html += "<div class=\"row\"><span class=\"key\">Flash méret</span><span class=\"val\">" + String(ESP.getFlashChipSize() / 1024 / 1024) + " MB</span></div>";
    html += "<div class=\"row\"><span class=\"key\">OTA max méret</span><span class=\"val\">1 MB</span></div>";
    html += "</div>";

    html += "<div class=\"foot\">Modbus-MQTT Bridge v2.0 — ESP32-S3-ETH (6DI+6R) — ESP32-S3</div>";

    html += R"rawliteral(<script>
function onFileSelect(){
    var f=document.getElementById('fwFile').files[0];
    document.getElementById('uploadBtn').disabled=!f;
    if(f&&f.size>1310720){
        alert('A fájl túllépi az 1.25 MB korlátot! ('+Math.round(f.size/1024)+' KB)');
        document.getElementById('uploadBtn').disabled=true;
    }
}
document.getElementById('otaForm').onsubmit=function(){
    document.getElementById('progress').style.display='block';
    document.getElementById('uploadBtn').disabled=true;
    var xhr=new XMLHttpRequest();
    xhr.open('POST','/otaupload',true);
    xhr.upload.onprogress=function(e){
        if(e.lengthComputable){
            var pct=Math.round(e.loaded/e.total*100);
            document.getElementById('progressFill').style.width=pct+'%';
            document.getElementById('progressText').textContent=pct+'% ('+Math.round(e.loaded/1024)+' / '+Math.round(e.total/1024)+' KB)';
        }
    };
    xhr.onload=function(){
        document.getElementById('progress').style.display='none';
        var s=document.getElementById('status');
        if(xhr.status==200){
            s.className='status-ok';
            s.innerHTML='&#10004; Firmware sikeresen frissítve! Az eszköz újraindul 3 másodperc múlva...';
            s.style.display='block';
            setTimeout(function(){window.location='/'},8000);
        }else{
            s.className='status-err';
            s.innerHTML='&#10008; Hiba: '+xhr.responseText;
            s.style.display='block';
            document.getElementById('uploadBtn').disabled=false;
        }
    };
    xhr.onerror=function(){
        document.getElementById('progress').style.display='none';
        var s=document.getElementById('status');
        s.className='status-err';
        s.innerHTML='&#10008; Hálózati hiba a feltöltés során!';
        s.style.display='block';
        document.getElementById('uploadBtn').disabled=false;
    };
    xhr.send(new FormData(this));
    return false;
};
</script></body></html>)rawliteral";

    web.send(200, "text/html", html);
}

// ─── OTA Upload Handler ──────────────────────────────────────
void handleOtaUpload() {
    // Auth check for OTA — must validate before processing upload
    if (!web_auth_ok()) return;
    HTTPUpload &upload = web.upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        LOG_I("[OTA] Upload start: %s (%d bytes)\n", upload.filename.c_str(), upload.totalSize);
        
        if (upload.totalSize > OTA_MAX_SIZE) {
            LOG_ELN("[OTA] ERROR: Firmware too large!");
            web.send(500, "text/plain", "A firmware túl nagy! Maximum 1 MB.");
            return;
        }
        
        if (!Update.begin(upload.totalSize)) {
            LOG_E("[OTA] ERROR: Update.begin failed! (%s)\n", Update.errorString());
            web.send(500, "text/plain", String("Nem sikerült elindítani a frissítést: ") + Update.errorString());
            return;
        }
        LOG_ILN("[OTA] Update started...");
        
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        LOG_D("[OTA] Writing %d bytes...\n", upload.currentSize);
        size_t written = Update.write(upload.buf, upload.currentSize);
        if (written != upload.currentSize) {
            LOG_E("[OTA] ERROR: Write failed! Written %d / %d\n", written, upload.currentSize);
            web.send(500, "text/plain", String("Írási hiba: ") + Update.errorString());
            return;
        }
        
    } else if (upload.status == UPLOAD_FILE_END) {
        LOG_ILN("[OTA] Upload complete, finalizing...");
        if (Update.end(true)) {
            LOG_I("[OTA] SUCCESS! Firmware updated (%d bytes). Restarting...\n", Update.size());
            web.send(200, "text/plain", "OK");
            delay(3000);
            ESP.restart();
        } else {
            LOG_E("[OTA] ERROR: Update.end failed! (%s)\n", Update.errorString());
            web.send(500, "text/plain", String("Frissítés sikertelen: ") + Update.errorString());
        }
        
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
            LOG_ELN("[OTA] Upload aborted!");
        Update.abort();
        web.send(500, "text/plain", "Feltöltés megszakítva!");
    }
}

// ─── Init & Loop ──────────────────────────────────────────────
void ota_init() {
    // ── ArduinoOTA (PlatformIO espota protocol, port 3232) ──
    ArduinoOTA.setHostname("modbusmqtt");
    ArduinoOTA.setPort(3232);
    
    ArduinoOTA.onStart([]() {
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
        LOG_I("[OTA] ArduinoOTA start: %s\n", type.c_str());
    });
    ArduinoOTA.onEnd([]() {
        LOG_ILN("[OTA] ArduinoOTA end — rebooting");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        static uint8_t last_pct = 0;
        uint8_t pct = (progress * 100) / total;
        if (pct != last_pct && pct % 10 == 0) {
            LOG_I("[OTA] Progress: %u%%\n", pct);
            last_pct = pct;
        }
    });
    ArduinoOTA.onError([](ota_error_t error) {
        LOG_E("[OTA] ArduinoOTA error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) LOG_ELN("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) LOG_ELN("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) LOG_ELN("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) LOG_ELN("Receive Failed");
        else if (error == OTA_END_ERROR) LOG_ELN("End Failed");
    });
    
    ArduinoOTA.begin();
    LOG_ILN("[OTA] ArduinoOTA ready on port 3232");
    
    // Routes for web-based OTA are registered in web_server_init()
    LOG_ILN("[OTA] Web upload handler ready");
}

void ota_loop() {
    ArduinoOTA.handle();  // Process PlatformIO espota requests (safe after ota_init())
}