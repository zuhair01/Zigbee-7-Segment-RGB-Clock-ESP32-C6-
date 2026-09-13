/*
 * settings.h — persistent clock settings stored in NVS.
 * Loaded at boot and applied to the render state; saved when Zigbee writes change them.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "clock_render.h"

typedef struct {
    uint8_t  brightness;
    rgb_t    main_color;
    rgb_t    hour_color;
    rgb_t    min_color;
    bool     mode_24h;
    bool     leading_zero;
    uint8_t  colon_mode;
    uint8_t  effect;
    bool     power;
    bool     auto_bright;    /* ambient light drives brightness */
    uint8_t  min_bright;     /* auto floor  (0..254) */
    uint8_t  max_bright;     /* auto ceiling(0..254) */
    bool     show_env;       /* cycle temp/humidity screens */
    uint8_t  time_dwell_s;   /* seconds showing the clock */
    uint8_t  env_dwell_s;    /* seconds per temp/humidity screen */
} clk_settings_t;

void settings_init(void);                 /* init NVS handle */
void settings_load(clk_settings_t *out);  /* load, filling defaults if absent */
void settings_save(const clk_settings_t *in);
