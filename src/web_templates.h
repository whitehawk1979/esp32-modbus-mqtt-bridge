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
    PG_OTA,
    PG_ADMIN
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
static String navHtml(WebPage active)
{
    String nav;
    nav.reserve(350);
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
        {"OTA", "/ota", PG_OTA},
        {"Admin", "/admin", PG_ADMIN},
    };

    for (auto &it : items)
    {
        nav += F("<a ");
        if (it.pg == active)
            nav += F("class=\"active\" ");
        nav += F("href=\"");
        nav += FPSTR(it.href);
        nav += F("\">");
        nav += FPSTR(it.label);
        nav += F("</a>");
    }
    nav += F("<a href=\"/logout\" style=\"float:right\">&#128274; Kilépés</a></div>");
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