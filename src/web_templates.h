/**
 * web_templates.h — Shared HTML templates for web_server.cpp
 *
 * Sidebar navigation (collapsible on mobile), dark theme.
 * PROGMEM strings for flash storage. Each page includes CSS_BASE
 * via pageStart() and adds page-specific styles between pageStyleEnd().
 */

#ifndef WEB_TEMPLATES_H
#define WEB_TEMPLATES_H

#include <Arduino.h>
#include <WString.h>
#include "modbus_mqtt_ha_bridge.h"

// ─── Page identifiers for nav highlighting ────────────────────
enum WebPage
{
    PG_STATUS,
    PG_CONFIG,
    PG_PINS,
    PG_MODULES,
    PG_REGISTERS,
    PG_OTA,
    PG_ADMIN,
    PG_SD,
    PG_LED,
    PG_STORAGE,
    PG_SCAN
};

// ─── Shared Base CSS (dark sidebar theme) ─────────────────────
static const char CSS_BASE[] PROGMEM = R"rawliteral(
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#0d1117;color:#c9d1d9;font-size:15px;display:flex;min-height:100vh}
h1{color:#58a6ff;font-size:1.3em;margin:0 0 8px}
h2{color:#f0883e;background:#161b22;padding:8px 12px;border-radius:6px;margin:16px 0 8px;font-size:1.05em}
.sidebar{position:fixed;top:0;left:0;width:220px;height:100vh;background:#010409;border-right:1px solid #21262d;display:flex;flex-direction:column;z-index:50;transform:translateX(0);transition:transform .25s ease}
.sidebar.collapsed{transform:translateX(-220px)}
.sidebar-header{padding:14px 16px;border-bottom:1px solid #21262d;display:flex;align-items:center;gap:8px}
.sidebar-header .logo{color:#58a6ff;font-size:18px;font-weight:700}
.sidebar-header .ver{color:#484f58;font-size:11px;margin-left:auto}
.sidebar-nav{flex:1;overflow-y:auto;padding:8px 0}
.sidebar-nav a{display:flex;align-items:center;gap:10px;padding:10px 16px;color:#8b949e;text-decoration:none;font-size:14px;transition:background .15s,color .15s}
.sidebar-nav a:hover{background:#161b22;color:#c9d1d9}
.sidebar-nav a.active{background:#0d1119;color:#58a6ff;border-right:3px solid #58a6ff;font-weight:600}
.sidebar-nav a .icon{font-size:16px;width:22px;text-align:center}
.sidebar-footer{padding:12px 16px;border-top:1px solid #21262d}
.sidebar-footer a{color:#f85149;text-decoration:none;font-size:13px;display:flex;align-items:center;gap:6px}
.sidebar-footer a:hover{color:#ff7b72}
.main{margin-left:220px;padding:16px 24px;flex:1;transition:margin-left .25s ease;min-width:0}
.main.expanded{margin-left:0}
.hamburger{display:none;position:fixed;top:12px;left:12px;z-index:100;background:#21262d;border:1px solid #30363d;border-radius:6px;color:#c9d1d9;font-size:22px;cursor:pointer;padding:6px 10px;transition:background .15s}
.hamburger:hover{background:#30363d}
.overlay{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.5);z-index:40}
.overlay.active{display:block}
.note{color:#8b949e;font-size:12px;margin:2px 0 8px}
.foot{text-align:center;color:#484f58;font-size:11px;margin-top:24px;border-top:1px solid #21262d;padding-top:12px}
@media(max-width:768px){
  .sidebar{transform:translateX(-220px)}
  .sidebar.open{transform:translateX(0)}
  .main{margin-left:0}
  .hamburger{display:block}
}
)rawliteral";

// ─── Status page extra CSS ────────────────────────────────────
static const char CSS_STATUS[] PROGMEM = R"rawliteral(
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px;margin:6px 0}
.row{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #21262d}
.row:last-child{border:none}
.key{color:#8b949e}.val{color:#c9d1d9;font-weight:600}
.on{color:#3fb950}.off{color:#f85149}.warn{color:#f0883e}
)rawliteral";

// ─── Config/Pins forms extra CSS ──────────────────────────────
static const char CSS_FORMS[] PROGMEM = R"rawliteral(
.fm{margin-bottom:8px}
label{display:block;font-weight:600;color:#7ee787;margin-bottom:2px;font-size:14px}
input,select{width:100%;padding:8px;border:1px solid #30363d;border-radius:4px;background:#0d1117;color:#c9d1d9;font-size:15px}
input:focus,select:focus{outline:none;border-color:#58a6ff}
.row{display:flex;gap:8px}.row .fm{flex:1}
.radio{display:flex;gap:16px;margin:6px 0;padding:8px;background:#161b22;border-radius:6px;border:1px solid #30363d}
.radio label{display:flex;align-items:center;gap:6px;font-weight:normal;color:#c9d1d9;cursor:pointer;margin:0;padding:4px 12px;border-radius:4px}
.radio label:has(input:checked){background:#238636;color:white}
.radio input{width:auto;accent-color:#58a6ff}
.chk{display:flex;align-items:center;gap:6px;margin:6px 0}
.chk label{margin:0;font-weight:normal;color:#c9d1d9;cursor:pointer}
.chk input{width:auto;accent-color:#58a6ff}
button{background:#238636;color:white;border:none;padding:10px 20px;border-radius:6px;font-size:16px;cursor:pointer;margin:10px 0}
button:hover{background:#2ea043}
.pri{background:#161b22;border:1px solid #f0883e;border-radius:6px;padding:12px;margin:8px 0}
.pri b{color:#f0883e}
)rawliteral";

// ─── Pins page extra CSS ──────────────────────────────────────
static const char CSS_PINS[] PROGMEM = R"rawliteral(
.warn-box{background:#161b22;border:1px solid #f0883e;border-radius:6px;padding:12px;margin:8px 0}
.warn-box b{color:#f0883e}
.error-box{background:#161b22;border:2px solid #f85149;border-radius:6px;padding:12px;margin:8px 0}
.error-box b{color:#f85149}
)rawliteral";

// ─── Modules page extra CSS ───────────────────────────────────
static const char CSS_MODULES[] PROGMEM = R"rawliteral(
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px;margin:6px 0}
.row{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #21262d}
.row:last-child{border:none}
.key{color:#8b949e}.val{color:#c9d1d9;font-weight:600}
.on{color:#3fb950}.off{color:#f85149}
.mrow{display:flex;gap:8px;align-items:center;margin:4px 0}
.mrow .fm{flex:1}
.mod-card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px;margin:8px 0}
.badge{display:inline-block;padding:2px 8px;border-radius:4px;font-size:12px;margin:2px}
.badge.on{background:#238636;color:white}
.badge.off{background:#f851494d;color:#f85149}
.badge.sensor{background:#1f6feb4d;color:#58a6ff}
.elabel{color:#8b949e;font-size:12px;margin:4px 0 2px}
.mrow-wrap{display:flex;flex-wrap:wrap;gap:6px;margin:4px 0}
.mod-section{background:#0d1117;border:1px solid #21262d;border-radius:6px;padding:10px;margin:8px 0}
.mod-section-title{color:#f0883e;font-size:13px;font-weight:600;margin:0 0 6px;display:flex;align-items:center;gap:4px}
.entity-row{display:flex;flex-wrap:wrap;gap:6px;margin:4px 0;align-items:center}
.fm-sm{flex:0 0 calc(33% - 4px);min-width:80px}
.fm-sm label{display:block;color:#8b949e;font-size:11px;margin-bottom:2px}
.fm-sm input{width:100%;background:#0d1117;border:1px solid #30363d;color:#c9d1d9;padding:4px 6px;border-radius:4px;font-size:13px}
.room-other{margin-top:4px}
.room-manage{margin:24px 0 0;padding:12px;background:#161b22;border:1px solid #30363d;border-radius:8px;border-top:3px solid #f0883e;clear:both;width:100%}
.room-manage h3{color:#f0883e;font-size:14px;margin:0 0 8px}
.room-tags{display:flex;flex-wrap:wrap;gap:6px;margin:6px 0}
.room-tag{display:inline-flex;align-items:center;gap:4px;background:#21262d;padding:4px 12px;border-radius:20px;font-size:13px;color:#c9d1d9;white-space:nowrap}
.room-tag a{color:#f85149;text-decoration:none;font-weight:bold;font-size:16px;line-height:1}
.room-add{display:flex;gap:6px;margin-top:8px}
.room-add input{flex:1;padding:6px 8px;font-size:13px}
.room-add button{padding:6px 12px;font-size:13px}
.rbtn{display:inline-block;cursor:pointer;padding:3px 10px;border-radius:4px;margin:2px;font-weight:600;font-size:12px;border:none;transition:opacity 0.15s}
.rbtn:active{opacity:0.6}
.rbtn.on{background:#238636;color:white}
.rbtn.off{background:#f851494d;color:#f85149;border:1px solid #f8514940}
@media(max-width:768px){
  .mod-card{padding:10px 8px}
  .mrow{flex-direction:column;gap:4px}
  .mrow .fm{width:100%}
  .fm-sm{flex:0 0 100%!important;min-width:0!important}
  .mrow-wrap{flex-direction:column;gap:4px}
  .rbtn{padding:8px 16px!important;font-size:15px!important;margin:3px!important;min-width:60px;text-align:center}
  .badge{padding:4px 10px;font-size:13px;margin:2px}
  .elabel{font-size:13px;margin:8px 0 4px}
  select{font-size:14px!important;padding:6px 8px!important;min-height:36px}
  input[type=text]{font-size:16px!important;padding:8px!important;min-height:38px}
  .room-manage{padding:8px;margin-top:20px;border-top:3px solid #f0883e;clear:both;width:100%}
  .room-tag{font-size:14px;padding:4px 12px}
  .room-tags{gap:8px}
  .room-add{flex-direction:row;gap:8px}
  .room-add input{min-height:38px}
  .room-add button{min-height:38px}
}
)rawliteral";

// ─── Register Config page extra CSS ──────────────────────────────
static const char CSS_REGISTERS[] PROGMEM = R"rawliteral(
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px;margin:6px 0}
.row{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #21262d}
.row:last-child{border:none}
.key{color:#8b949e}.val{color:#c9d1d9;font-weight:600}
.on{color:#3fb950}.off{color:#f85149}
.rtbl{width:100%;border-collapse:collapse;margin:8px 0;font-size:13px}
.rtbl th{text-align:left;color:#8b949e;font-weight:600;padding:6px 8px;border-bottom:1px solid #30363d;white-space:nowrap}
.rtbl td{padding:5px 8px;border-bottom:1px solid #21262d;vertical-align:middle}
.rtbl tr:hover{background:#161b22}
.rtbl input,.rtbl select{width:100%;padding:4px 6px;border:1px solid #30363d;border-radius:4px;background:#0d1117;color:#c9d1d9;font-size:13px}
.rtbl input:focus,.rtbl select:focus{outline:none;border-color:#58a6ff}
.rtbl .sm{width:60px}.rtbl .md{width:90px}.rtbl .lg{width:140px}
.badge{display:inline-block;padding:2px 8px;border-radius:4px;font-size:12px;margin:2px}
.badge.on{background:#238636;color:white}
.badge.off{background:#f851494d;color:#f85149}
.btn-sm{padding:4px 10px;font-size:12px;border:none;border-radius:4px;cursor:pointer;margin:1px}
.btn-sm:hover{opacity:0.85}
.btn-add{background:#238636;color:white}
.btn-del{background:#da3633;color:white}
.btn-edit{background:#1f6feb;color:white}
.btn-tog{background:#f0883e;color:white}
.add-form{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px;margin:10px 0}
.add-form h3{color:#58a6ff;margin:0 0 8px;font-size:14px}
)rawliteral";

// ─── SD Card page extra CSS ─────────────────────────────────────
static const char CSS_SD[] PROGMEM = R"rawliteral(
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px;margin:6px 0}
.row{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #21262d}
.row:last-child{border:none}
.key{color:#8b949e}.val{color:#c9d1d9;font-weight:600}
.on{color:#3fb950}.off{color:#f85149}
.sd-exclusive-banner{background:#3d2800;border:1px solid #f0883e;border-radius:8px;padding:10px 14px;margin:8px 0;color:#f0883e;font-size:14px;text-align:center}
.sd-init-fail{background:#3d0a0a;border:1px solid #f85149;border-radius:8px;padding:10px 14px;margin:8px 0;color:#f85149;font-size:14px;text-align:center}
.sd-actions{display:flex;gap:8px;flex-wrap:wrap;margin:8px 0}
.sd-actions button{font-size:14px;padding:8px 14px}
.btn-del{background:#da3633}.btn-del:hover{background:#f85149}
.btn-warn{background:#f0883e}.btn-warn:hover{background:#d29922}
.file-list{margin:8px 0}
.file-entry{display:flex;align-items:center;justify-content:space-between;padding:8px 12px;margin:4px 0;background:#161b22;border:1px solid #30363d;border-radius:6px}
.file-entry:hover{border-color:#58a6ff}
.file-info{display:flex;align-items:center;gap:8px;flex:1;min-width:0}
.file-name{color:#58a6ff;cursor:pointer;text-decoration:none;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.file-name:hover{text-decoration:underline}
.file-size{color:#8b949e;font-size:12px;white-space:nowrap}
.file-actions{display:flex;gap:6px;flex-shrink:0}
.file-actions button{font-size:12px;padding:4px 10px}
.dir-icon{color:#f0883e}
.file-icon{color:#8b949e}
.breadcrumb{display:flex;gap:4px;align-items:center;margin:8px 0;flex-wrap:wrap}
.breadcrumb a{color:#58a6ff;text-decoration:none}.breadcrumb a:hover{text-decoration:underline}
.breadcrumb span{color:#484f58}
.upload-area{background:#161b22;border:2px dashed #30363d;border-radius:8px;padding:16px;margin:8px 0;text-align:center}
.upload-area:hover{border-color:#58a6ff}
.modal-overlay{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.7);z-index:100;justify-content:center;align-items:center}
.modal-overlay.active{display:flex}
.modal{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:16px;max-width:90vw;max-height:80vh;overflow:auto;min-width:300px}
.modal pre{background:#0d1117;border:1px solid #30363d;border-radius:4px;padding:12px;overflow:auto;max-height:60vh;color:#c9d1d9;font-size:13px;white-space:pre-wrap;word-break:break-all}
.modal-close{float:right;background:none;border:none;color:#8b949e;font-size:20px;cursor:pointer}
.modal-close:hover{color:#f85149}
.confirm-overlay{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.7);z-index:200;justify-content:center;align-items:center}
.confirm-overlay.active{display:flex}
.confirm-box{background:#161b22;border:1px solid #f0883e;border-radius:8px;padding:16px;text-align:center;min-width:280px}
.confirm-box p{margin:8px 0;color:#c9d1d9}
.confirm-box .btn-row{display:flex;gap:8px;justify-content:center;margin-top:12px}
)rawliteral";

// ─── LED control page extra CSS ──────────────────────────────
static const char CSS_LED[] PROGMEM = R"rawliteral(
.led-preview{width:80px;height:80px;border-radius:50%;margin:10px auto;border:3px solid #30363d;box-shadow:0 0 20px rgba(0,0,0,0.5)}
.led-controls{display:flex;flex-direction:column;gap:12px;max-width:360px;margin:0 auto}
.led-row{display:flex;align-items:center;gap:10px;margin:4px 0}
.led-row label{min-width:90px;color:#8b949e;font-size:14px}
.led-row input[type=range]{flex:1;accent-color:#58a6ff}
.led-row .val{min-width:40px;text-align:right;color:#c9d1d9;font-weight:600;font-size:14px}
.color-picker{width:50px;height:36px;border:1px solid #30363d;border-radius:4px;background:#0d1117;cursor:pointer;padding:2px}
.preset-grid{display:flex;flex-wrap:wrap;gap:8px;margin:8px 0}
.preset-btn{width:44px;height:44px;border-radius:50%;border:2px solid #30363d;cursor:pointer;transition:transform .15s,border-color .15s}
.preset-btn:hover{transform:scale(1.15);border-color:#58a6ff}
.toggle-btn{display:inline-block;padding:12px 32px;border-radius:8px;font-size:18px;cursor:pointer;font-weight:700;border:none;margin:8px 4px;transition:background .2s}
.toggle-btn.on{background:#238636;color:white}
.toggle-btn.off{background:#da3633;color:white}
.toggle-btn:hover{opacity:0.85}
.led-info{text-align:center;color:#8b949e;font-size:13px;margin:4px 0}
.led-section{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:14px;margin:10px 0}
.led-section h3{color:#58a6ff;font-size:14px;margin:0 0 8px}
)rawliteral";

// ─── OTA Firmware page extra CSS ──────────────────────────────
static const char CSS_OTA[] PROGMEM = R"rawliteral(
.card{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px;margin:6px 0}
.row{display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid #21262d}
.row:last-child{border:none}
.key{color:#8b949e}.val{color:#c9d1d9;font-weight:600}
.fm{margin-bottom:8px}
label{display:block;font-weight:600;color:#7ee787;margin-bottom:2px;font-size:14px}
input[type=file]{width:100%;padding:8px;border:1px solid #30363d;border-radius:4px;background:#0d1117;color:#c9d1d9;font-size:15px}
button{background:#238636;color:white;border:none;padding:10px 20px;border-radius:6px;font-size:16px;cursor:pointer;margin:10px 0}
button:hover{background:#2ea043}
button:disabled{background:#484f58;cursor:not-allowed}
.warn-box{background:#161b22;border:1px solid #f85149;border-radius:6px;padding:12px;margin:8px 0}
.warn-box b{color:#f85149}
#progress{display:none;margin:12px 0}
#progressBar{width:100%;height:24px;background:#21262d;border-radius:4px;overflow:hidden}
#progressFill{width:0%;height:100%;background:#238636;transition:width 0.3s}
#progressText{text-align:center;color:#c9d1d9;margin:4px 0;font-size:14px}
#status{margin:12px 0;padding:12px;border-radius:6px;display:none}
.status-ok{background:#0d1117;border:1px solid #3fb950;color:#3fb950}
.status-err{background:#0d1117;border:1px solid #f85149;color:#f85149}
.url-ota{margin:8px 0}
.url-ota input[type=text]{width:100%;padding:8px;border:1px solid #30363d;border-radius:4px;background:#0d1117;color:#c9d1d9;font-size:14px}
.url-ota input[type=text]:focus{outline:none;border-color:#58a6ff}
)rawliteral";

// ─── Admin page extra CSS ─────────────────────────────────────
// Defined inline if needed; placeholder for future extraction

// ─── Minimal CSS for small result pages (saved, restart) ──────
static const char CSS_MINIMAL[] PROGMEM = R"rawliteral(
body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;text-align:center;padding:40px}
.ok{font-size:28px}p{margin:12px 0;color:#8b949e}
)rawliteral";

// ─── Error page CSS ──────────────────────────────────────────
static const char CSS_ERROR[] PROGMEM = R"rawliteral(
body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;text-align:center;padding:40px}
.ok{font-size:28px;color:#f85149}p{margin:12px 0;color:#8b949e}
.err-box{background:#3d0a0a;border:1px solid #f85149;border-radius:8px;padding:16px;margin:12px 0;color:#f85149}
)rawliteral";

// ─── Navigation sidebar HTML ──────────────────────────────────
// Returns sidebar HTML with collapsible mobile support
static String navHtml(WebPage active, const String &authSuffix = "")
{
    struct NavItem {
        const char *icon;   // Emoji icon
        const char *label;
        const char *href;
        WebPage pg;
    };
    static const NavItem items[] = {
        {"📊", "Státusz",     "/",          PG_STATUS},
        {"⚙️", "Beállítások", "/config",    PG_CONFIG},
        {"📌", "Pinek",       "/pins",      PG_PINS},
        {"🔌", "Modulok",    "/modules",   PG_MODULES},
        {"📋", "Regiszterek","/registers",  PG_REGISTERS},
        {"📡", "OTA",         "/ota",       PG_OTA},
        {"🔒", "Admin",       "/admin",     PG_ADMIN},
        {"💾", "SD Kártya",   "/sd",        PG_SD},
        {"📦", "Storage",     "/storage",   PG_STORAGE},
        {"🔍", "Scan",        "/scan",      PG_SCAN},
        {"💡", "LED",         "/led",       PG_LED},
    };
    static const int itemCount = sizeof(items) / sizeof(items[0]);

    String nav;
    nav.reserve(900);
    nav += F("<!-- Hamburger button (mobile) -->"
             "<button class='hamburger' onclick='toggleSidebar()'>&#9776;</button>"
             "<div class='overlay' id='overlay' onclick='toggleSidebar()'></div>"
             "<!-- Sidebar -->"
             "<nav class='sidebar' id='sidebar'>"
             "<div class='sidebar-header'>"
             "<span class='logo'>&#9889; MB-MQTT</span>"
             "<span class='ver'>v");
    nav += F(FIRMWARE_VERSION);
    nav += F("</span>"
             "</div>"
             "<div class='sidebar-nav'>");

    for (int i = 0; i < itemCount; i++)
    {
        nav += F("<a ");
        if (items[i].pg == active)
            nav += F("class='active' ");
        nav += F("href='");
        nav += FPSTR(items[i].href);
        nav += authSuffix;
        nav += F("'><span class='icon'>");
        nav += FPSTR(items[i].icon);
        nav += F("</span>");
        nav += FPSTR(items[i].label);
        nav += F("</a>");
    }

    nav += F("</div>" // close sidebar-nav
             "<div class='sidebar-footer'>"
             "<a href='/logout");
    nav += authSuffix;
    nav += F("'><span class='icon'>&#128274;</span>Kilépés</a>"
             "</div>"
             "</nav>"
             // Sidebar toggle script
             "<script>"
             "function toggleSidebar(){"
             "var s=document.getElementById('sidebar');"
             "var o=document.getElementById('overlay');"
             "s.classList.toggle('open');"
             "o.classList.toggle('active');"
             "}"
             "</script>");

    return nav;
}

// ─── Page start helper ────────────────────────────────────────
// Builds <html><head> with base CSS + page-specific CSS prefix
// The caller appends extra CSS then closes </style></head><body>
static String pageStart(const __FlashStringHelper *title, const char *extraCss = nullptr)
{
    String h;
    h.reserve(1200);
    h = F("<!DOCTYPE html><html lang='hu'><head>\n<meta charset='UTF-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>\n<title>");
    h += FPSTR(reinterpret_cast<PGM_P>(title));
    h += F("</title>\n<style>\n");
    h += FPSTR(CSS_BASE);
    if (extraCss)
        h += FPSTR(extraCss);
    return h;
}

/// Close the style+head+open body and main content div
static String pageStyleEnd()
{
    return F("\n</style></head><body>"
             "<div class='main' id='main'>");
}

/// Standard footer (also closes sidebar structure)
static String pageFoot()
{
    return F("<p class='foot'>Modbus-MQTT HA Bridge &copy; 2025 — ESP32-S3-ETH (6DI+6R)</p>"
             "</div><!-- /main -->"
             "</body></html>");
}

// Page footer with auto-refresh (JS lightweight /api/status poll + DOM update)
static String pageFootAutoRefresh(int intervalSec = 5)
{
    String s;
    s.reserve(2048);
    s += F("<p class='foot'>Modbus-MQTT HA Bridge &copy; 2025 — ESP32-S3-ETH (6DI+6R)</p>"
           "</div><!-- /main -->"
           "<script>"
           "var _iv=setInterval(function(){"
           "var x=new XMLHttpRequest();"
           "x.onload=function(){"
           "if(x.status!=200)return;"
           "var d=JSON.parse(x.responseText);"
           "var u=function(id,v){var e=document.getElementById(id);if(e)e.textContent=v;};"
           "var uc=function(id,v,c){var e=document.getElementById(id);if(e){e.textContent=v;e.className='val '+c;}};"
           "var di=function(id,c){var e=document.getElementById(id);if(e)e.style.display=c;};"
           // Network
           "u('st-iface',d.interface);"
           "u('st-ip',d.ip);"
           "u('st-wifi-ip',d.wifi_ip||'0.0.0.0');"
           "u('st-wifi-rssi',d.wifi_rssi||0);"
           "u('st-lan-ip',d.lan_ip||'0.0.0.0');"
           "uc('st-lan-st',d.lan_connected?'CSATLAKOZVA ✅':'NEM ❌',d.lan_connected?'on':'off');"
           // MQTT
           "uc('st-mqtt',d.mqtt_connected?'CSATLAKOZVA ✅':'NEM CSATLAKOZOTT ❌',d.mqtt_connected?'on':'off');"
           "u('st-mqtt-tr',d.mqtt_transport);"
           "u('st-mqtt-rc',d.mqtt_reconnects||0);"
           "u('st-wifi-rc',d.wifi_reconnects||0);"
           // System
           "u('st-uptime',d.uptime_s);"
           "u('st-heap',d.heap_free_kb);"
           "u('st-wdt',d.wdt_reboots);"
           "u('st-psram',d.psram_free?Math.round(d.psram_free/1024)+' KB':'—');"
           "u('st-fw',d.firmware);"
           "};"
           "x.open('GET','/api/status");
    // Append auth parameter if present
    s += F("'+(location.search?'\\?'+location.search.substr(1):''),true);"
           "x.send();"
           "},");
    s += String(intervalSec * 1000);
    s += F(");"
           "</script>"
           "</body></html>");
    return s;
}

#endif // WEB_TEMPLATES_H