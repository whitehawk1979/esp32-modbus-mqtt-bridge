/**
 * EthWebServer.h — Minimal WebServer-compatible interface over EthernetServer (W5500)
 */

#ifndef ETH_WEBSERVER_H
#define ETH_WEBSERVER_H

#ifdef USE_W5500

#include <Arduino.h>
#include <Ethernet.h>
#include <map>
#include "modbus_mqtt_ha_bridge.h"

// ─── HTTP Methods ────────────────────────────────────────────
enum EthHTTPMethod
{
    ETH_HTTP_GET,
    ETH_HTTP_POST,
    ETH_HTTP_ANY
};

// ─── Route handler type ──────────────────────────────────────
typedef void (*EthHandler)();

// ─── Route entry ──────────────────────────────────────────────
struct EthRoute
{
    String path;
    EthHTTPMethod method;
    EthHandler handler;
};

#define ETH_MAX_ROUTES 40

// ─── EthernetServer subclass with begin() override ───────────
class EthEthernetServer : public EthernetServer
{
public:
    explicit EthEthernetServer(uint16_t port) : EthernetServer(port) {}
    void begin(uint16_t) override { EthernetServer::begin(); }
    using EthernetServer::begin;
};

class EthWebServer
{
public:
    EthWebServer(uint16_t port = 80) : _server(port), _port(port) {}

    void begin()
    {
        _server.begin();
        LOG_I("[ETH-WEB] Server started on port %d\n", _port);
    }

    void handleClient()
    {
        EthernetClient client = _server.available();
        if (!client) return;

        // Read request with timeout
        unsigned long reqStart = millis();
        String reqLine = "";
        bool gotRequest = false;

        while (client.connected() && (millis() - reqStart) < 3000)
        {
            if (client.available())
            {
                char c = client.read();
                reqLine += c;
                if (reqLine.endsWith("\r\n\r\n"))
                {
                    gotRequest = true;
                    break;
                }
                if (reqLine.length() > 4096)
                    break;
            }
            else
            {
                delay(1);
            }
        }

        if (!gotRequest || reqLine.length() < 10)
        {
            client.stop();
            return;
        }

        _client = client;

        // Parse request line
        int space1 = reqLine.indexOf(' ');
        int space2 = reqLine.indexOf(' ', space1 + 1);
        if (space1 < 0 || space2 < 0)
        {
            client.stop();
            return;
        }

        String method = reqLine.substring(0, space1);
        String fullUri = reqLine.substring(space1 + 1, space2);

        if (method == "GET") _method = ETH_HTTP_GET;
        else if (method == "POST") _method = ETH_HTTP_POST;
        else { client.stop(); return; }

        // Parse path and query
        int qPos = fullUri.indexOf('?');
        if (qPos >= 0)
        {
            _path = fullUri.substring(0, qPos);
            _queryString = fullUri.substring(qPos + 1);
        }
        else
        {
            _path = fullUri;
            _queryString = "";
        }

        // Parse headers
        _headers.clear();
        _args.clear();
        _contentLength = 0;

        int hdrStart = reqLine.indexOf("\r\n") + 2;
        while (hdrStart < (int)reqLine.length() - 3)
        {
            int lineEnd = reqLine.indexOf("\r\n", hdrStart);
            if (lineEnd < 0) break;
            String hdrLine = reqLine.substring(hdrStart, lineEnd);
            int colon = hdrLine.indexOf(':');
            if (colon > 0)
            {
                String key = hdrLine.substring(0, colon);
                key.trim();
                String val = hdrLine.substring(colon + 1);
                val.trim();
                _headers[key] = val;
                if (key.equalsIgnoreCase("Content-Length"))
                    _contentLength = val.toInt();
            }
            hdrStart = lineEnd + 2;
        }

        // Parse query-string args
        _parseArgs(_queryString);

        // For POST: read body
        if (_method == ETH_HTTP_POST && _contentLength > 0)
        {
            unsigned long bodyStart = millis();
            String body = "";
            while (client.connected() && (int)body.length() < _contentLength && (millis() - bodyStart) < 5000)
            {
                if (client.available())
                    body += (char)client.read();
                else
                    delay(1);
            }
            _parseArgs(body);
        }

        LOG_D("[ETH-WEB] %s %s\n", method.c_str(), _path.c_str());

        // Route matching
        bool handled = false;
        for (int i = 0; i < _routeCount; i++)
        {
            if (_routes[i].path == _path &&
                (_routes[i].method == _method || _routes[i].method == ETH_HTTP_ANY))
            {
                _routes[i].handler();
                handled = true;
                break;
            }
        }

        if (!handled)
        {
            client.println("HTTP/1.1 404 Not Found");
            client.println("Content-Type: text/plain");
            client.println("Connection: close");
            client.println();
            client.println("Not Found");
        }

        if (client.connected())
            client.stop();
        _client = EthernetClient();
    }

    // ── Route registration ─────────────────────────────────
    void on(const String &path, EthHTTPMethod method, EthHandler handler)
    {
        if (_routeCount >= ETH_MAX_ROUTES)
        {
            LOG_E("[ETH-WEB] Too many routes!\n");
            return;
        }
        _routes[_routeCount].path = path;
        _routes[_routeCount].method = method;
        _routes[_routeCount].handler = handler;
        _routeCount++;
    }

    // ── WebServer-compatible API ───────────────────────────
    bool hasArg(const String &name) const
    {
        return _args.find(name) != _args.end();
    }

    bool hasArg(const char *name) const
    {
        return _args.find(String(name)) != _args.end();
    }

    String arg(const String &name) const
    {
        auto it = _args.find(name);
        return (it != _args.end()) ? it->second : "";
    }

    String arg(const char *name) const
    {
        return arg(String(name));
    }

    String arg(int i) const
    {
        int idx = 0;
        for (auto &kv : _args)
        {
            if (idx == i) return kv.second;
            idx++;
        }
        return "";
    }

    int args() const { return _args.size(); }
    String pathArg(int i) const { return ""; }

    String header(const String &name) const
    {
        auto it = _headers.find(name);
        return (it != _headers.end()) ? it->second : "";
    }

    // ── Response ──────────────────────────────────────────
    void send(int code, const String &type, const String &content)
    {
        if (!_client) return;
        _client.printf("HTTP/1.1 %d %s\r\n", code, _statusText(code));
        _client.printf("Content-Type: %s\r\n", type.c_str());
        _client.printf("Content-Length: %d\r\n", (int)content.length());
        _client.println("Connection: close");
        _client.println();
        // Chunked write — W5500 TX buffer is ~2KB, must flush between chunks
        const int CHUNK = 1024;
        int total = content.length();
        int sent = 0;
        while (sent < total)
        {
            int len = min(CHUNK, total - sent);
            _client.write((const uint8_t *)content.c_str() + sent, len);
            _client.flush();
            sent += len;
            yield(); // feed WDT
        }
    }

    void send(int code, const char *type, const String &content)
    {
        send(code, String(type), content);
    }

    void send(int code, const char *type, const char *content)
    {
        send(code, String(type), String(content));
    }

    void send(int code)
    {
        send(code, "text/html", "");
    }

    void send_P(int code, PGM_P type, PGM_P content)
    {
        if (!_client) return;
        String typeStr = FPSTR(type);
        String contentStr = FPSTR(content);
        send(code, typeStr, contentStr);
    }

    void sendHeader(const String &name, const String &value, bool first = false)
    {
        if (!_client) return;
        _client.printf("%s: %s\r\n", name.c_str(), value.c_str());
    }

    void setContentLength(size_t len) { /* no-op for now */ }

    // ── Auth ─────────────────────────────────────────────
    bool authenticate(const char *user, const char *pass)
    {
        // LAN: query-param auth only
        if (hasArg("auth"))
            return arg("auth") == String(pass);
        return false;
    }

    void requestAuthentication()
    {
        // LAN: no Digest auth, send 401 with info
        send(401, "text/plain", "Auth required — use ?auth=PASSWORD");
    }

    // ── Client ────────────────────────────────────────────
    EthernetClient &client() { return _client; }

    void redirect(const String &url)
    {
        if (!_client) return;
        _client.println("HTTP/1.1 302 Found");
        _client.printf("Location: %s\r\n", url.c_str());
        _client.println("Connection: close");
        _client.println();
    }

private:
    EthEthernetServer _server;
    EthernetClient _client;
    uint16_t _port;

    EthRoute _routes[ETH_MAX_ROUTES];
    int _routeCount = 0;

    String _path;
    String _queryString;
    EthHTTPMethod _method;
    std::map<String, String> _args;
    std::map<String, String> _headers;
    int _contentLength = 0;

    void _parseArgs(const String &data)
    {
        if (data.length() == 0) return;
        int start = 0;
        while (start < (int)data.length())
        {
            int amp = data.indexOf('&', start);
            int segEnd = (amp >= 0) ? amp : (int)data.length();
            String seg = data.substring(start, segEnd);
            int eq = seg.indexOf('=');
            if (eq > 0)
            {
                String key = _urlDecode(seg.substring(0, eq));
                String val = _urlDecode(seg.substring(eq + 1));
                _args[key] = val;
            }
            else if (seg.length() > 0)
            {
                _args[_urlDecode(seg)] = "";
            }
            start = segEnd + 1;
        }
    }

    String _urlDecode(const String &s)
    {
        String result = "";
        for (unsigned i = 0; i < s.length(); i++)
        {
            char c = s[i];
            if (c == '%' && i + 2 < s.length())
            {
                char hex[3] = {(char)s[i + 1], (char)s[i + 2], 0};
                result += (char)strtol(hex, nullptr, 16);
                i += 2;
            }
            else if (c == '+')
            {
                result += ' ';
            }
            else
            {
                result += c;
            }
        }
        return result;
    }

    const char *_statusText(int code)
    {
        switch (code)
        {
        case 200: return "OK";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        default: return "Unknown";
        }
    }
};

#endif // USE_W5500
#endif // ETH_WEBSERVER_H