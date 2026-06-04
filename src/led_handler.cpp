// ─── WS2812B RGB LED Handler ─────────────────────────────────────
// Waveshare ESP32-S3-ETH V1.0: WS2812B on GPIO21
// Provides: color set, brightness, on/off, MQTT HA light entity
//
// HA entity type: light (supports color + brightness)
// MQTT topics:
//   modbusmqtt/led/state    → {"state":"ON","color":{"r":255,"g":0,"b":0},"brightness":100}
//   modbusmqtt/led/set      → {"state":"ON","color":{"r":255,"g":0,"b":0},"brightness":100}
//   modbusmqtt/led/set       → "ON" / "OFF" (simple toggle)
//   homeassistant/light/modbusmqtt_led/config → discovery

#include "modbus_mqtt_ha_bridge.h"

#ifdef USE_WS2812
#include <Adafruit_NeoPixel.h>

// ─── LED Hardware ────────────────────────────────────────────────
static Adafruit_NeoPixel *pixel = nullptr;
static bool led_on = false;
static uint8_t led_r = 0, led_g = 255, led_b = 0;  // Default: green
static uint8_t led_brightness_pct = 50;  // 0-100%
#define LED_PIXEL_COUNT 1

// ─── Init ────────────────────────────────────────────────────────
void led_init()
{
    if (pixel) return;  // Already init

    pixel = new Adafruit_NeoPixel(LED_PIXEL_COUNT, PIN_WS2812,
                                  NEO_GRB + NEO_KHZ800);
    if (!pixel)
    {
        LOG_E("[LED] Failed to allocate NeoPixel\n");
        return;
    }

    pixel->begin();
    pixel->setBrightness(map(led_brightness_pct, 0, 100, 0, 255));
    pixel->setPixelColor(0, pixel->Color(0, 0, 0));  // Start OFF
    pixel->show();

    LOG_I("[LED] WS2812B init OK (GPIO%d)\n", PIN_WS2812);
}

// ─── Apply current color/brightness to hardware ──────────────────
static void led_apply()
{
    if (!pixel) return;

    if (!led_on)
    {
        pixel->setPixelColor(0, 0, 0, 0);  // OFF = black
    }
    else
    {
        // Apply brightness scaling
        float scale = (float)led_brightness_pct / 100.0f;
        uint8_t r = (uint8_t)(led_r * scale);
        uint8_t g = (uint8_t)(led_g * scale);
        uint8_t b = (uint8_t)(led_b * scale);
        pixel->setPixelColor(0, pixel->Color(r, g, b));
    }
    pixel->show();
}

// ─── Set Color ───────────────────────────────────────────────────
void led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    led_r = r;
    led_g = g;
    led_b = b;
    if (led_on)
        led_apply();
}

// ─── Set Brightness (0-100%) ─────────────────────────────────────
void led_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    led_brightness_pct = percent;
    if (pixel)
        pixel->setBrightness(map(percent, 0, 100, 0, 255));
    if (led_on)
        led_apply();
}

// ─── Set State (ON/OFF) ──────────────────────────────────────────
void led_set_state(bool on)
{
    led_on = on;
    led_apply();
}

// ─── Getters ──────────────────────────────────────────────────────
bool led_is_on() { return led_on; }
uint8_t led_get_brightness() { return led_brightness_pct; }

void led_get_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (r) *r = led_r;
    if (g) *g = led_g;
    if (b) *b = led_b;
}

// ─── MQTT State Publish ──────────────────────────────────────────
void led_publish_state()
{
    if (!mqtt_is_connected()) return;

    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"state\":\"%s\",\"color\":{\"r\":%d,\"g\":%d,\"b\":%d},\"brightness\":%d}",
             led_on ? "ON" : "OFF",
             led_r, led_g, led_b,
             led_brightness_pct);

    mqtt_publish_topic("modbusmqtt/led/state", payload, true);
}

// ─── MQTT HA Discovery ──────────────────────────────────────────
void led_setup_discovery()
{
    if (!mqtt_is_connected()) return;

    const char *topic = "homeassistant/light/modbusmqtt_led/config";
    char payload[512];
    snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"ESP32 LED\","
        "\"uniq_id\":\"modbusmqtt_led\","
        "\"cmd_t\":\"modbusmqtt/led/set\","
        "\"stat_t\":\"modbusmqtt/led/state\","
        "\"stat_val_tpl\":\"{{value_json.state}}\","
        "\"rgb_cmd_t\":\"modbusmqtt/led/set\","
        "\"rgb_stat_t\":\"modbusmqtt/led/state\","
        "\"rgb_val_tpl\":\"{{value_json.color.r}},{{value_json.color.g}},{{value_json.color.b}}\","
        "\"bri_cmd_t\":\"modbusmqtt/led/set\","
        "\"bri_stat_t\":\"modbusmqtt/led/state\","
        "\"bri_val_tpl\":\"{{value_json.brightness}}\","
        "\"bri_scale\":true,"
        "\"pl_on\":\"ON\","
        "\"pl_off\":\"OFF\","
        "\"ret\":true,"
        "\"dev\":{\"ids\":[\"modbusmqtt_bridge\"],\"name\":\"ESP32 Modbus-MQTT Bridge\",\"mf\":\"Waveshare\",\"mdl\":\"ESP32-S3-ETH\",\"sw\":\"v2.9.1\"}"
        "}");

    mqtt_publish_topic(topic, payload, true);
    LOG_I("[LED] HA discovery published\n");
}

// ─── MQTT Command Handler (to be called from mqtt_handler callback) ──
// Parses HA light command format: {"state":"ON","color":{"r":255,"g":0,"b":0},"brightness":100}
// Or simple: "ON" / "OFF"
bool led_handle_command(const char *topic, const char *payload, size_t len)
{
    // Only handle modbusmqtt/led/set
    if (strncmp(topic, "modbusmqtt/led/set", 18) != 0)
        return false;

    String pl(payload);

    // Simple ON/OFF toggle
    if (pl.equalsIgnoreCase("ON"))
    {
        led_set_state(true);
        led_publish_state();
        return true;
    }
    if (pl.equalsIgnoreCase("OFF"))
    {
        led_set_state(false);
        led_publish_state();
        return true;
    }

    // JSON command
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, len);
    if (err)
    {
        LOG_E("[LED] JSON parse error: %s\n", err.c_str());
        return false;
    }

    // State
    if (!doc["state"].isNull())
    {
        const char *state = doc["state"];
        led_set_state(strcasecmp(state, "ON") == 0);
    }

    // RGB comma-separated (HA sends "255,0,0" for rgb_command_topic)
    if (!doc["rgb"].isNull())
    {
        const char *rgb_str = doc["rgb"];
        int rr = 0, gg = 0, bb = 0;
        if (sscanf(rgb_str, "%d,%d,%d", &rr, &gg, &bb) == 3)
        {
            led_set_color((uint8_t)rr, (uint8_t)gg, (uint8_t)bb);
        }
    }

    // Brightness (0-255 from HA, scale to 0-100)
    if (!doc["brightness"].isNull())
    {
        int bri = doc["brightness"];
        // HA sends 0-255, we store 0-100
        uint8_t pct = (uint8_t)((bri * 100) / 255);
        led_set_brightness(pct);
    }

    // Color (JSON object with r,g,b keys)
    if (!doc["color"].isNull())
    {
        JsonObject color = doc["color"];
        led_set_color(color["r"] | led_r,
                      color["g"] | led_g,
                      color["b"] | led_b);
    }

    // Color temp (ignored — RGB LED doesn't have temp)
    // Transition (ignored — not applicable)

    led_publish_state();
    return true;
}

#else // !USE_WS2812 — Stubs

void led_init() {}
void led_set_color(uint8_t, uint8_t, uint8_t) {}
void led_set_brightness(uint8_t) {}
void led_set_state(bool) {}
bool led_is_on() { return false; }
uint8_t led_get_brightness() { return 0; }
void led_get_color(uint8_t *r, uint8_t *g, uint8_t *b) { if(r) *r=0; if(g) *g=0; if(b) *b=0; }
void led_publish_state() {}
void led_setup_discovery() {}
bool led_handle_command(const char *, const char *, size_t) { return false; }

#endif // USE_WS2812