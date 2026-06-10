/**
 * EthWebServer.cpp — Lightweight WebServer wrapper for W5500 EthernetServer
 */
#ifdef USE_W5500

#include <Arduino.h>
#include "EthWebServer.h"
#include "modbus_mqtt_ha_bridge.h"
#include "web_adapter.h"

// ─── Global instances ───
EthWebServer ethWeb(80);
EthWebAdapter lanAdapter(ethWeb);

// ─── EthWebServer implementation ───

EthWebServer::EthWebServer(uint16_t port)
    : _port(port), _server(port), _client(), _route_count(0),
      _client_connected(false), _content_length(0),
      _arg_count(0), _args_parsed(false)
{
}

void EthWebServer::begin()
{
    _server.begin();
}

void EthWebServer::_on(const char *uri, Handler handler)
{
    if (_route_count < MAX_ROUTES)
    {
        _routes[_route_count].uri = String(uri);
        _routes[_route_count].handler = handler;
        _route_count++;
    }
}

void EthWebServer::handleClient()
{
    EthernetClient newClient = _server.available();
    if (!newClient)
        return;

    _client = newClient;
    _client_connected = true;
    _args_parsed = false;
    _arg_count = 0;
    _content_length = 0;

    if (_parseRequest())
    {
        bool handled = false;
        for (int i = 0; i < _route_count; i++)
        {
            if (_uri == _routes[i].uri || _uri.startsWith(_routes[i].uri + "?"))
            {
                if (_routes[i].handler)
                    _routes[i].handler();
                handled = true;
                break;
            }
        }
        if (!handled)
        {
            send(404, "text/plain", "Not Found");
        }
    }

    if (_client_connected)
    {
        _client.stop();
        _client_connected = false;
    }
}

bool EthWebServer::_parseRequest()
{
    unsigned long t0 = millis();

    while (!_client.available() && millis() - t0 < 2000)
    {
        delay(1);
    }
    if (!_client.available())
        return false;

    String reqLine = _client.readStringUntil('\n');
    reqLine.trim();
    LOG_D("[LAN-HTTP] Request: %s\n", reqLine.c_str());

    int sp1 = reqLine.indexOf(' ');
    int sp2 = reqLine.indexOf(' ', sp1 + 1);
    if (sp1 < 0 || sp2 < 0)
        return false;

    String fullUri = reqLine.substring(sp1 + 1, sp2);

    int qpos = fullUri.indexOf('?');
    if (qpos >= 0)
    {
        _uri = fullUri.substring(0, qpos);
        _query = fullUri.substring(qpos + 1);
    }
    else
    {
        _uri = fullUri;
        _query = "";
    }

    // Consume remaining headers
    while (_client.available())
    {
        String hdr = _client.readStringUntil('\n');
        if (hdr.length() <= 2)
            break;
    }

    return true;
}

void EthWebServer::_parseArgs() const
{
    if (_args_parsed)
        return;
    _args_parsed = true;
    _arg_count = 0;

    if (_query.length() == 0)
        return;

    int start = 0;
    while (start < (int)_query.length() && _arg_count < 16)
    {
        int eqPos = _query.indexOf('=', start);
        int ampPos = _query.indexOf('&', start);
        if (eqPos < 0)
            break;

        String key = _query.substring(start, eqPos);
        String val;
        if (ampPos < 0)
        {
            val = _query.substring(eqPos + 1);
            start = _query.length();
        }
        else
        {
            val = _query.substring(eqPos + 1, ampPos);
            start = ampPos + 1;
        }

        // URL decode helper
        auto urlDecode = [](const String &s) -> String {
            String out;
            out.reserve(s.length());
            for (unsigned i = 0; i < s.length(); i++)
            {
                if (s[i] == '%' && i + 2 < s.length())
                {
                    char hex[3] = {s.charAt(i + 1), s.charAt(i + 2), 0};
                    out += (char)strtol(hex, nullptr, 16);
                    i += 2;
                }
                else if (s[i] == '+')
                {
                    out += ' ';
                }
                else
                {
                    out += s[i];
                }
            }
            return out;
        };

        _arg_names[_arg_count] = urlDecode(key);
        _arg_values[_arg_count] = urlDecode(val);
        _arg_count++;
    }
}

String EthWebServer::arg(const char *name) const
{
    _parseArgs();
    for (int i = 0; i < _arg_count; i++)
    {
        if (_arg_names[i] == name)
            return _arg_values[i];
    }
    return "";
}

String EthWebServer::arg(int i) const
{
    _parseArgs();
    if (i >= 0 && i < _arg_count)
        return _arg_values[i];
    return "";
}

void EthWebServer::send(int code, const char *content_type, const String &content)
{
    _sendDefaultHeaders(code, content_type, content.length());
    _client.print(content);
}

void EthWebServer::send_P(int code, PGM_P type, PGM_P content)
{
    // EthernetClient doesn't have write_P — copy to RAM first
    size_t len = strlen_P(content);
    _sendDefaultHeaders(code, type, len);
    char *buf = new char[len + 1];
    if (buf)
    {
        memcpy_P(buf, content, len);
        buf[len] = 0;
        _client.write(buf, len);
        delete[] buf;
    }
}

void EthWebServer::sendHeader(const String &name, const String &value, bool first)
{
    _client.printf("%s: %s\r\n", name.c_str(), value.c_str());
}

void EthWebServer::_sendDefaultHeaders(int code, const char *content_type, size_t content_length)
{
    _client.printf("HTTP/1.1 %d ", code);
    switch (code)
    {
    case 200: _client.print("OK"); break;
    case 302: _client.print("Found"); break;
    case 400: _client.print("Bad Request"); break;
    case 401: _client.print("Unauthorized"); break;
    case 404: _client.print("Not Found"); break;
    case 500: _client.print("Internal Server Error"); break;
    default: _client.print("Unknown"); break;
    }
    _client.printf("\r\nContent-Type: %s\r\nConnection: close\r\nContent-Length: %u\r\n\r\n",
                   content_type, (unsigned)content_length);
}

#endif // USE_W5500