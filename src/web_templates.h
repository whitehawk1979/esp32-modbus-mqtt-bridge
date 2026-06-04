/**
 * web_templates.h — Shared HTML templates for web_server.cpp
 *
 * Extracts repeated CSS and navigation HTML from inline R"rawliteral"
 * strings into PROGMEM constants.  Each page includes the base CSS
 * via PAGE_CSS_START / PAGE_CSS_END and adds page-specific styles
 * between them.  Navigation uses NAV_HTML(active_page).
 *
 * Memory savings: ~2KB CSS × 9 pages = ~16KB code space reclaimed.
 * PROGMEM keeps strings in flash, not RAM.
 */

#ifndef WEB_TEMPLATES_H
#define WEB_TEMPLATES_H

#include <Arduino.h>
#include <WString.h>

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
    PG_SD
};

// ─── Shared Base CSS (used by every page) ─────────────────────
// Stored in PROGMEM — must be read with pgm_read_ptr / FPSTR()
static const char CSS_BASE[] PROGMEM = R"rawliteral(
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#0d1117;color:#c9d1d9;padding:12px;font-size:15px}
h1{color:#58a6ff;font-size:1.3em;margin:8px 0}
h2{color:#f0883e;background:#161b22;padding:8px 12px;border-radius:6px;margin:16px 0 8px;font-size:1.05em}
.nav{display:flex;gap:8px;margin:8px 0}
.nav a{background:#21262d;color:#58a6ff;padding:8px 16px;border-radius:6px;text-decoration:none;font-size:14px}
.nav a:hover{background:#30363d}
.nav a.active{background:#238636;color:white}
.note{color:#8b949e;font-size:12px;margin:2px 0 8px}
.foot{text-align:center;color:#484f58;font-size:11px;margin-top:16px;border-top:1px solid #21262d;padding-top:8px}
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
.row{display:flex;gap:8px}
.row .fm{flex:1}
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
.rtbl .sm{width:60px}
.rtbl .md{width:90px}
.rtbl .lg{width:140px}
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
.err{color:#f85149;font-size:28px}p{margin:12px 0;color:#8b949e}
a{color:#58a6ff}
)rawliteral";

// ─── Navigation HTML builder ─────────────────────────────────
// Returns nav bar with the correct page highlighted
// authSuffix: if non-empty (e.g. "?auth=admin"), appended to all nav links
//              so LAN clients stay authenticated across page navigation
static String navHtml(WebPage active, const String &authSuffix = "")
{
    String nav;
    nav.reserve(400);
    nav = F("<div class=\"nav\">");

    struct NavItem
    {
        const char *label;
        const char *href;
        WebPage pg;
    };
    static const NavItem items[] = {
        {"Státusz", "/", PG_STATUS},
        {"Beállítások", "/config", PG_CONFIG},
        {"Pinek", "/pins", PG_PINS},
        {"Modulok", "/modules", PG_MODULES},
        {"Regiszterek", "/registers", PG_REGISTERS},
        {"OTA", "/ota", PG_OTA},
        {"Admin", "/admin", PG_ADMIN},
        {"SD Kártya", "/sd", PG_SD},
    };

    for (auto &it : items)
    {
        nav += F("<a ");
        if (it.pg == active)
            nav += F("class=\"active\" ");
        nav += F("href=\"");
        nav += FPSTR(it.href);
        nav += authSuffix;
        nav += F("\">");
        nav += FPSTR(it.label);
        nav += F("</a>");
    }
    nav += F("<a href=\"/logout");
    nav += authSuffix;
    nav += F("\" style=\"float:right\">&#128274; Kilépés</a></div>");
    return nav;
}

// ─── Page start helper ────────────────────────────────────────
// Builds <html><head> with base CSS + page-specific CSS prefix
// The caller appends extra CSS then closes </style></head><body>
static String pageStart(const __FlashStringHelper *title, const char *extraCss = nullptr)
{
    String h;
    h.reserve(1200);
    h = F("<!DOCTYPE html><html lang=\"hu\"><head>\n<meta charset=\"UTF-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n<title>");
    h += FPSTR(reinterpret_cast<PGM_P>(title));
    h += F("</title>\n<style>\n");
    h += FPSTR(CSS_BASE);
    if (extraCss)
        h += FPSTR(extraCss);
    return h;
}

/// Close the style+head+body tag pair (call after appending page CSS)
static String pageStyleEnd()
{
    return F("\n</style></head><body>");
}

/// Standard footer
static String pageFoot()
{
    return F("<p class=\"foot\">Modbus-MQTT HA Bridge &copy; 2025 — ESP32-S3-ETH (6DI+6R)</p>"
             "</body></html>");
}

#endif // WEB_TEMPLATES_H