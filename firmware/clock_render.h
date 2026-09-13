/*
 * clock_render.h — public API for the LED rendering subsystem.
 *
 * The render task owns the LED strip. Zigbee callbacks push desired state via
 * the setters below; the render task reads a shared snapshot each frame.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    COLON_SOLID = 0,
    COLON_BLINK = 1,
    COLON_OFF   = 2,
} colon_mode_t;

typedef enum {
    FX_SOLID    = 0,
    FX_BREATHE  = 1,
    FX_RAINBOW  = 2,
    FX_GRADIENT = 3,
} effect_t;

/* One packed RGB color. */
typedef struct { uint8_t r, g, b; } rgb_t;

/* The full display state the render task reads each frame. */
typedef struct {
    bool     power;          /* On/Off cluster */
    uint8_t  brightness;     /* 0..255, from Level Control */
    rgb_t    main_color;     /* from Color Control */
    rgb_t    hour_color;     /* {0,0,0} => use main_color */
    rgb_t    min_color;      /* {0,0,0} => use main_color */
    bool     mode_24h;
    bool     leading_zero;   /* true => show leading 0 in hour */
    colon_mode_t colon_mode;
    effect_t effect;

    /* Current time, set by the Zigbee Time cluster handler. */
    uint8_t  hour;           /* 0..23 */
    uint8_t  minute;         /* 0..59 */
    uint8_t  second;         /* 0..59 */
    bool     time_valid;     /* false until first sync => shows dashes/pulsing */

    /* Environment (DHT11) + display cycling. */
    bool     show_env;       /* cycle temp/humidity screens into the display */
    uint8_t  time_dwell_s;   /* seconds showing the clock */
    uint8_t  env_dwell_s;    /* seconds per temp / humidity screen */
    int      temp_c;         /* rounded temperature (C) for the display */
    int      humidity;       /* rounded humidity (%RH) for the display */
    bool     env_valid;      /* a valid DHT reading is available */
} display_state_t;

/* Start the render task and initialise the LED strip. */
void clock_render_start(void);

/* Thread-safe partial updates (called from Zigbee callbacks). */
void render_set_power(bool on);
void render_set_brightness(uint8_t level);
void render_set_main_color(rgb_t c);
void render_set_hour_color(rgb_t c);
void render_set_min_color(rgb_t c);
void render_set_mode_24h(bool en);
void render_set_leading_zero(bool en);
void render_set_colon_mode(colon_mode_t m);
void render_set_effect(effect_t fx);
void render_set_time(uint8_t h, uint8_t m, uint8_t s, bool valid);

/* Environment display controls. */
void render_set_show_env(bool en);
void render_set_env_dwell(uint8_t time_dwell_s, uint8_t env_dwell_s);
void render_set_environment(int temp_c, int humidity, bool valid);

/* Tick the internal software clock by one second (called from a 1 Hz timer
 * so the display keeps counting between Zigbee time syncs). */
void render_tick_second(void);

/* Snapshot copy for persistence / reporting. */
void render_get_state(display_state_t *out);
