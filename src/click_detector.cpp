/**
 * click_detector.cpp — Single/Double/Multi-Click + Hold Detection
 * 
 * State machine for digital inputs that detects:
 *  - Single click   (short press & release)
 *  - Double click   (two clicks within ~350ms)
 *  - Triple click   (three clicks within ~200ms each)
 *  - Quad/Penta      (up to 5 clicks)
 *  - Long press/hold (press >500ms)
 *  - Hold release    (release after hold)
 * 
 * Uses polling-based approach with debouncing.
 * Supports callback mechanism for MQTT publishing.
 */

#include <Arduino.h>
#include "modbus_mqtt_ha_bridge.h"

// ─── Click Callback ──────────────────────────────────────────
static ClickCallback click_callback_fn = nullptr;

void click_set_callback(ClickCallback cb) {
    click_callback_fn = cb;
}

// ─── Initialize DI State ────────────────────────────────────────
void click_init(DI_State *di, uint8_t count) {
    for (uint8_t i = 0; i < count && i < 16; i++) {
        di[i].current = false;
        di[i].prev = false;
        di[i].press_start_ms = 0;
        di[i].last_click_ms = 0;
        di[i].click_count = 0;
        di[i].holding = false;
        di[i].pending = CLICK_NONE;
        di[i].hold_fired = false;
    }
}

// ─── Update DI State Machine ───────────────────────────────────
void click_update(DI_State *di, uint8_t idx, bool physical_state) {
    DI_State &s = di[idx];
    uint32_t now = millis();
    
    // Edge detection
    s.prev = s.current;
    
    // Debounce: only accept change if stable for 20ms
    static uint32_t last_change_ms[16] = {0};
    static bool debounced[16] = {0};
    
    if (physical_state != debounced[idx]) {
        if (now - last_change_ms[idx] > 20) {
            debounced[idx] = physical_state;
        }
        last_change_ms[idx] = now;
    }
    
    s.current = debounced[idx];
    
    bool rising = s.current && !s.prev;    // Button pressed
    bool falling = !s.current && s.prev;  // Button released
    
    // ─── Rising Edge (button pressed) ──────────────────────────
    if (rising) {
        s.press_start_ms = now;
        s.hold_fired = false;
        
        // If within multi-click window, increment counter
        if (s.click_count > 0 && (now - s.last_click_ms) < CLICK_MULTI_MS) {
            // Still in multi-click sequence
        } else if (s.click_count > 0 && (now - s.last_click_ms) < CLICK_DOUBLE_MS) {
            // Double click window
        } else {
            // New click sequence
            s.click_count = 0;
            s.pending = CLICK_NONE;
        }
    }
    
    // ─── While Held ─────────────────────────────────────────────
    if (s.current && !s.hold_fired) {
        if ((now - s.press_start_ms) >= CLICK_HOLD_MS) {
            s.holding = true;
            s.hold_fired = true;
            s.pending = CLICK_HOLD;
            s.click_count = 0;  // Hold overrides click sequence
            
            // Fire callback for hold immediately
            if (click_callback_fn) {
                click_callback_fn(idx, 4);  // 4 = long
            }
        }
    }
    
    // ─── Falling Edge (button released) ────────────────────────
    if (falling) {
        uint32_t press_duration = now - s.press_start_ms;
        
        if (s.holding) {
            // End of hold
            s.holding = false;
            s.pending = CLICK_HOLD_RELEASE;
        } else if (press_duration >= CLICK_SINGLE_MS) {
            // Valid click
            s.click_count++;
            s.last_click_ms = now;
            
            // Wait for possible double/multi click or timeout
            s.pending = CLICK_NONE;  // Will be resolved on timeout or next click
        }
        // Too short press — ignore (noise)
    }
    
    // ─── Timeout Resolution ─────────────────────────────────────
    if (!s.current && s.click_count > 0 && !s.holding) {
        uint32_t since_last_click = now - s.last_click_ms;
        
        if (s.click_count >= CLICK_MAX_COUNT) {
            // Max clicks reached — report immediately
            s.pending = (ClickType)s.click_count;
            // Fire callback
            if (click_callback_fn) {
                uint8_t ct = (s.click_count >= 4) ? 3 : s.click_count;  // map to 1=single,2=double,3=triple
                click_callback_fn(idx, ct);
            }
            s.click_count = 0;
        } else if (since_last_click > CLICK_DOUBLE_MS && s.click_count == 1) {
            // Single click confirmed (no second click within window)
            s.pending = CLICK_SINGLE;
            // Fire callback
            if (click_callback_fn) {
                click_callback_fn(idx, 1);  // 1 = single
            }
            s.click_count = 0;
        } else if (since_last_click > CLICK_MULTI_MS && s.click_count >= 2) {
            // Multi-click confirmed
            s.pending = (ClickType)s.click_count;
            // Fire callback: 1=single,2=double,3=triple,4=long
            if (click_callback_fn) {
                uint8_t ct = (s.click_count >= 3) ? 3 : s.click_count;  // 2=double, 3+=triple
                click_callback_fn(idx, ct);
            }
            s.click_count = 0;
        }
    }
}

// ─── Get Pending Click Event ───────────────────────────────────
ClickType click_get_event(DI_State *di, uint8_t idx) {
    DI_State &s = di[idx];
    ClickType evt = s.pending;
    s.pending = CLICK_NONE;
    return evt;
}

// ─── Click Type to String ──────────────────────────────────────
const char* click_type_str(ClickType ct) {
    switch (ct) {
        case CLICK_NONE:         return "none";
        case CLICK_SINGLE:       return "single";
        case CLICK_DOUBLE:       return "double";
        case CLICK_TRIPLE:       return "triple";
        case CLICK_QUAD:         return "quad";
        case CLICK_PENTA:        return "penta";
        case CLICK_HOLD:         return "hold";
        case CLICK_HOLD_RELEASE: return "hold_release";
        default:                 return "unknown";
    }
}

// ─── Click type uint8_t to MQTT string ──────────────────────────
const char* click_type_mqtt_str(uint8_t ct) {
    switch (ct) {
        case 1:  return "single";
        case 2:  return "double";
        case 3:  return "triple";
        case 4:  return "long";
        default: return "unknown";
    }
}