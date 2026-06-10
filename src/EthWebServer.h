/**
 * EthWebServer.h — Lightweight WebServer wrapper for W5500 EthernetServer
 *
 * The Arduino Ethernet library uses its own socket stack (not lwIP),
 * so WiFi's WebServer can't see W5500 connections. This wrapper provides
 * a minimal WebServer-like API on top of EthernetServer/EthernetClient.
 */
#ifndef ETH_WEBSERVER_H
#define ETH_WEBSERVER_H

#ifdef USE_W5500

#include <Ethernet.h>

// ─── HTTP Method compat enums (for on(uri, method, handler) calls) ──
enum EthHTTPMethod {
    ETH_HTTP_GET,
    ETH_HTTP_POST,
    ETH_HTTP_DELETE,
    ETH_HTTP_PUT
};

// ─── EthEthernetServer: thin wrapper around EthernetServer ───
// EthernetServer is abstract (missing begin(uint16_t)), so we subclass it
class EthEthernetServer : public EthernetServer {
public:
    EthEthernetServer(uint16_t port) : EthernetServer(port) {}
    void begin(uint16_t port) override { (void)port; EthernetServer::begin(); }
    void begin() override { EthernetServer::begin(); }
    EthernetClient available() { return accept(); }
};

// ─── EthWebServer: minimal WebServer-like for Ethernet ───
class EthWebServer {
public:
    typedef void (*Handler)(void);

    EthWebServer(uint16_t port);

    void begin();
    void handleClient();

    // Route registration (method param ignored — all methods handled the same)
    void on(const char *uri, Handler handler) { _on(uri, handler); }
    template<typename M> void on(const char *uri, M method, Handler handler) { (void)method; _on(uri, handler); }

    // Request info
    String uri() const { return _uri; }
    String arg(const char *name) const;
    String arg(const String &name) const { return arg(name.c_str()); }
    bool hasArg(const char *name) const { return arg(name).length() > 0; }
    bool hasArg(const String &name) const { return hasArg(name.c_str()); }
    int args() const { return _arg_count; }
    String arg(int i) const;
    String pathArg(int i) const { return (i == 0) ? _uri : String(); }
    String header(const String &name) const { return String(); }

    // Response
    void send(int code, const char *content_type, const String &content);
    void send(int code, const String &type, const String &content) { send(code, type.c_str(), content); }
    void send(int code, const char *type, const char *content) { send(code, type, String(content)); }
    void send(int code) { send(code, "text/plain", ""); }
    void send_P(int code, PGM_P type, PGM_P content);
    void sendHeader(const String &name, const String &value, bool first = false);
    void setContentLength(size_t len) { _content_length = len; }

    // Auth stubs (LAN uses query-param auth)
    bool authenticate(const char *user, const char *pass) { return false; }
    void requestAuthentication() { send(401, "text/plain", "Unauthorized"); }

    // Raw client access
    EthernetClient &client() { return _client; }
    operator bool() { return (bool)_client; }

private:
    uint16_t _port;
    EthEthernetServer _server;
    EthernetClient _client;

    String _uri;
    String _query;
    bool _client_connected;
    size_t _content_length;

    static const int MAX_ROUTES = 32;
    struct Route {
        String uri;
        Handler handler;
    } _routes[MAX_ROUTES];
    int _route_count;

    mutable String _arg_names[16];
    mutable String _arg_values[16];
    mutable int _arg_count;
    mutable bool _args_parsed;

    void _parseArgs() const;
    void _on(const char *uri, Handler handler);
    bool _parseRequest();
    void _sendDefaultHeaders(int code, const char *content_type, size_t content_length);
};

// ethWeb global is defined in EthWebServer.cpp
// lanAdapter is defined in EthWebServer.cpp (after web_adapter.h is included)
// Both are declared extern in include/web_adapter.h

extern EthWebServer ethWeb;

#endif // USE_W5500
#endif // ETH_WEBSERVER_H