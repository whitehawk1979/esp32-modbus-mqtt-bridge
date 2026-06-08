/**
 * web_adapter.h — Adapter that allows handler functions to work on both
 * WiFi WebServer and EthWebServer without modification.
 *
 * Problem: handler functions call web.send(), web.hasArg(), web.arg() etc.
 *         directly on the global WebServer object.
 *         We want the same handlers to work on LAN (EthWebServer) too.
 *
 * Solution: Before calling a handler, we set the "active" adapter.
 *           ALL handler calls go through WebAdapter methods.
 *           WebAdapter delegates to either WiFi WebServer or EthWebServer.
 *
 * Usage:
 *   // In eth_handler.cpp, before calling a handler:
 *   set_active_web_server(WEB_LAN);
 *   handleStatus();
 *   set_active_web_server(WEB_WIFI);
 *
 *   // In handler functions, replace:
 *   web.send(...)     →  WA.send(...)
 *   web.hasArg(...)   →  WA.hasArg(...)
 *   web.arg(...)      →  WA.arg(...)
 *   web.authenticate  →  WA.authenticate(...)
 *   web.requestAuth   →  WA.requestAuthentication(...)
 *   web.client()      →  WA.clientIP()
 *
 * But that requires modifying ALL handler calls...
 *
 * EASIER TRICK: Override the global `web` reference!
 * We can't override WebServer, but we CAN make the handler functions
 * use a macro that resolves to the active server.
 *
 * Actually, the simplest approach: use a global WebInterface pointer.
 */

#ifndef WEB_ADAPTER_H
#define WEB_ADAPTER_H

#include <Arduino.h>
#include <WebServer.h>

#ifdef USE_W5500
#include "EthWebServer.h"
#endif

// ─── WebInterface — abstract interface for web operations ─────
// Both WebServer and EthWebServer support these methods.
// We use this to make handler functions interface-agnostic.

class WebInterface
{
public:
    virtual ~WebInterface() = default;

    virtual bool hasArg(const String &name) = 0;
    virtual bool hasArg(const char *name) = 0;
    virtual String arg(const String &name) = 0;
    virtual String arg(const char *name) = 0;
    virtual String arg(int i) = 0;
    virtual int args() = 0;
    virtual String pathArg(int i) = 0;
    virtual String header(const String &name) = 0;

    virtual void send(int code, const String &type, const String &content) = 0;
    virtual void send(int code, const char *type, const String &content) = 0;
    virtual void send(int code, const char *type, const char *content) = 0;
    virtual void send(int code) = 0;
    virtual void send_P(int code, PGM_P type, PGM_P content) = 0;
    virtual void sendHeader(const String &name, const String &value, bool first = false) = 0;
    virtual void setContentLength(size_t len) = 0;

    virtual bool authenticate(const char *user, const char *pass) = 0;
    virtual void requestAuthentication() = 0;

    virtual IPAddress clientIP() = 0;

    // ─── Raw client stream access (for OTA raw API) ────────
    // Returns reference to the underlying Client (WiFiClient or EthernetClient).
    // Use for low-level stream reads (e.g., OTA raw firmware upload).
    virtual Client &clientStream() = 0;

    // ─── Multipart file upload support ────────────────────────
    // WiFi WebServer supports multipart upload; LAN (EthWebServer) does not.
    // Callers should check isUploadSupported() before using upload().
    virtual HTTPUpload &upload() = 0;
    virtual bool isUploadSupported() const = 0;
};

// ─── WiFi WebServer adapter ───────────────────────────────────
class WiFiWebAdapter : public WebInterface
{
public:
    WiFiWebAdapter(WebServer &server) : _server(server) {}

    bool hasArg(const String &name) override { return _server.hasArg(name); }
    bool hasArg(const char *name) override { return _server.hasArg(name); }
    String arg(const String &name) override { return _server.arg(name); }
    String arg(const char *name) override { return _server.arg(name); }
    String arg(int i) override { return _server.arg(i); }
    int args() override { return _server.args(); }
    String pathArg(int i) override { return _server.pathArg(i); }
    String header(const String &name) override { return _server.header(name); }

    void send(int code, const String &type, const String &content) override { _server.send(code, type, content); }
    void send(int code, const char *type, const String &content) override { _server.send(code, type, content); }
    void send(int code, const char *type, const char *content) override { _server.send(code, type, content); }
    void send(int code) override { _server.send(code); }
    void send_P(int code, PGM_P type, PGM_P content) override { _server.send_P(code, type, content); }
    void sendHeader(const String &name, const String &value, bool first = false) override { _server.sendHeader(name, value, first); }
    void setContentLength(size_t len) override { _server.setContentLength(len); }

    bool authenticate(const char *user, const char *pass) override { return _server.authenticate(user, pass); }
    void requestAuthentication() override { _server.requestAuthentication(DIGEST_AUTH, "ModbusMQTT"); }

    IPAddress clientIP() override { return _server.client().remoteIP(); }
    Client &clientStream() override { _activeClient = _server.client(); return _activeClient; }

    // ─── Multipart upload — supported on WiFi ──────────────────
    HTTPUpload &upload() override { return _server.upload(); }
    bool isUploadSupported() const override { return true; }

private:
    WebServer &_server;
    WiFiClient _activeClient; // cached for clientStream() reference
};

#ifdef USE_W5500
// ─── Ethernet (W5500) Web Server adapter ──────────────────────
class EthWebAdapter : public WebInterface
{
public:
    EthWebAdapter(EthWebServer &server) : _server(server) {}

    bool hasArg(const String &name) override { return _server.hasArg(name); }
    bool hasArg(const char *name) override { return _server.hasArg(name); }
    String arg(const String &name) override { return _server.arg(name); }
    String arg(const char *name) override { return _server.arg(name); }
    String arg(int i) override { return _server.arg(i); }
    int args() override { return _server.args(); }
    String pathArg(int i) override { return _server.pathArg(i); }
    String header(const String &name) override { return _server.header(name); }

    void send(int code, const String &type, const String &content) override { _server.send(code, type, content); }
    void send(int code, const char *type, const String &content) override { _server.send(code, type, content); }
    void send(int code, const char *type, const char *content) override { _server.send(code, type, content); }
    void send(int code) override { _server.send(code, "text/plain", ""); }
    void send_P(int code, PGM_P type, PGM_P content) override { _server.send_P(code, type, content); }
    void sendHeader(const String &name, const String &value, bool first = false) override { _server.sendHeader(name, value, first); }
    void setContentLength(size_t len) override { _server.setContentLength(len); }

    bool authenticate(const char *user, const char *pass) override { return _server.authenticate(user, pass); }
    void requestAuthentication() override { _server.requestAuthentication(); }

    IPAddress clientIP() override
    {
        if (_server.client())
            return _server.client().remoteIP();
        return IPAddress(0, 0, 0, 0);
    }
    Client &clientStream() override { return _server.client(); }

    // ─── Multipart upload — NOT supported on LAN ───────────────
    // Returns a static dummy; callers must check isUploadSupported() first.
    HTTPUpload &upload() override { static HTTPUpload dummy; return dummy; }
    bool isUploadSupported() const override { return false; }

private:
    EthWebServer &_server;
};
#endif

// ─── Global active adapter pointer ───────────────────────────
// Handler functions call WS->send(), WS->hasArg(), etc.
// Set before calling handlers:
//   set_active_web(WIFI_ADAPTER or LAN_ADAPTER)

extern WebInterface *WS; // Active web server (WiFi or LAN)

// Adapters (created in setup)
extern WiFiWebAdapter wifiAdapter;
#ifdef USE_W5500
extern EthWebAdapter lanAdapter;
extern EthWebServer ethWeb;
#endif

#endif // WEB_ADAPTER_H