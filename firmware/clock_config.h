/*
 * clock_config.h — compile-time hardware & layout configuration.
 *
 * Everything you'd normally tweak for a specific physical build lives here.
 * The LED chain is a single WS2812/SK6812 data line wired as:
 *
 *   [digit0 (H tens)] -> [digit1 (H ones)] -> [colon] -> [digit2 (M tens)] -> [digit3 (M ones)]
 *
 * Each digit has 7 segments; each segment has LEDS_PER_SEGMENT pixels.
 * The colon has COLON_LEDS pixels (two dots).
 */
#pragma once

#include <stdint.h>

/* ---------------- Hardware ---------------- */
#define LED_DATA_GPIO          8      /* WS2812 DIN. Change to your wiring. */
#define STATUS_LED_GPIO        (-1)   /* optional onboard status LED, -1 to disable */
#define FACTORY_RESET_GPIO     9      /* BOOT button on most C6 devkits (hold to reset) */

/* ---------------- Ambient light sensor (analog LDR) ----------------
 * LDR wired as a voltage divider into an ADC1 channel:
 *
 *      3V3 ──[ LDR ]──┬── ADC pin (LDR_ADC_GPIO)
 *                     │
 *                   [ R_fixed ]      (10 kΩ typical)
 *                     │
 *                    GND
 *
 * With this orientation more light => lower LDR resistance => HIGHER voltage.
 * If yours reads inverted, set LDR_INVERThed to 1 (or swap LDR/resistor).
 *
 * ESP32-C6 ADC1 channels map to GPIO0..GPIO6 (CH0..CH6). GPIO3 = ADC1_CH3.
 */
#define LDR_ADC_GPIO           3      /* must be an ADC1-capable pin (GPIO0..6) */
#define LDR_ADC_CHANNEL        ADC_CHANNEL_3
#define LDR_INVERT             0      /* set 1 if brighter light gives lower ADC */

/* Calibration: raw 12-bit ADC (0..4095) at your darkest and brightest ambient.
 * Point a light at it / cover it and read the "ambient raw" log line to tune. */
#define LDR_RAW_DARK           250    /* ADC value in a dark room            */
#define LDR_RAW_BRIGHT         3600   /* ADC value in bright daylight        */

/* Approx lux at full-scale, only used to synthesise a value for the Zigbee
 * Illuminance cluster (an LDR is NOT a calibrated lux meter — treat as relative). */
#define LDR_LUX_FULLSCALE      2000

/* Auto-brightness dynamics */
#define AMBIENT_SAMPLE_MS      250    /* ADC sample period                   */
#define AMBIENT_EMA_ALPHA      0.15f  /* 0..1 smoothing; lower = smoother    */
#define AMBIENT_CURVE_GAMMA    0.55f  /* <1 brightens sooner, >1 stays dim   */
#define AMBIENT_SLEW_PER_STEP  6      /* max brightness change per sample (anti-flicker) */
#define AMBIENT_REPORT_MS      2000   /* how often to push level/lux to Zigbee */

/* ---------------- Temperature/Humidity sensor (DHT11) ----------------
 * Single-wire DHT11 on one GPIO with a 4.7k-10k pull-up to 3V3:
 *
 *      3V3 ──[ 4.7k..10k ]──┬── DATA (DHT_GPIO)
 *      3V3 ─────────────────┤ VCC
 *      GND ─────────────────┤ GND
 *
 * DHT11: 0..50 C (+/-2 C), 20..90 %RH (+/-5 %), ~1 Hz max sample rate.
 */
#define DHT_GPIO               10     /* any free GPIO with the pull-up */
#define DHT_TYPE               11     /* 11 = DHT11 (see dht.c for DHT22) */
#define DHT_READ_INTERVAL_MS   5000   /* >=1000 for DHT11; 5 s is comfortable */
#define ENV_REPORT_MS          10000  /* push temp/humidity to Zigbee this often */

/* ---------------- Display cycling (time <-> temp <-> humidity) ---- */
#define DEFAULT_SHOW_ENV       1      /* 1 = cycle in temp/humidity screens */
#define DEFAULT_TIME_DWELL_S   30     /* seconds showing the clock */
#define DEFAULT_ENV_DWELL_S    4      /* seconds per temp / humidity screen */

/* WS2812B = GRB order; SK6812 RGBW users: adjust color format in clock_render.c */
#define LED_STRIP_RESOLUTION_HZ (10 * 1000 * 1000)

/* ---------------- Layout ------------------ */
#define NUM_DIGITS             4
#define SEGS_PER_DIGIT         7
#define LEDS_PER_SEGMENT       2      /* pixels per segment; set to your build */
#define COLON_LEDS             2      /* two colon dots */

#define LEDS_PER_DIGIT   (SEGS_PER_DIGIT * LEDS_PER_SEGMENT)
#define TOTAL_LEDS       (NUM_DIGITS * LEDS_PER_DIGIT + COLON_LEDS)

/* Index in the chain where the colon pixels start (after 2 digits). */
#define COLON_START_INDEX (2 * LEDS_PER_DIGIT)

/* ---------------- Rendering --------------- */
#define RENDER_FPS             50
#define RENDER_PERIOD_MS       (1000 / RENDER_FPS)

/* ---------------- Zigbee ------------------ */
#define ZB_ENDPOINT            10
#define ZB_MANUF_NAME          "\x0B""OpenMakers"      /* len-prefixed ZCL string */
#define ZB_MODEL_ID            "\x0C""SegClock-C6"     /* len-prefixed ZCL string */

/* Custom cluster for clock behaviour (manufacturer-specific range 0xFC00..0xFFFF). */
#define CLK_CUSTOM_CLUSTER_ID  0xFC00
#define CLK_ATTR_MODE_24H      0x0000  /* bool:  true=24h, false=12h            */
#define CLK_ATTR_LEADING_ZERO  0x0001  /* bool:  blank leading hour zero        */
#define CLK_ATTR_COLON_MODE    0x0002  /* enum8: 0 solid, 1 blink, 2 off        */
#define CLK_ATTR_EFFECT        0x0003  /* enum8: 0 solid,1 breathe,2 rainbow,3 gradient */
#define CLK_ATTR_HOUR_COLOR    0x0004  /* uint24 RGB for hour digits (0=use main)*/
#define CLK_ATTR_MIN_COLOR     0x0005  /* uint24 RGB for minute digits           */
#define CLK_ATTR_AUTO_BRIGHT   0x0006  /* bool:  true=ambient controls brightness */
#define CLK_ATTR_MIN_BRIGHT    0x0007  /* uint8: floor brightness in auto (0..254) */
#define CLK_ATTR_MAX_BRIGHT    0x0008  /* uint8: ceiling brightness in auto (0..254)*/
#define CLK_ATTR_SHOW_ENV      0x0009  /* bool:  cycle temp/humidity screens in    */
#define CLK_ATTR_TIME_DWELL    0x000A  /* uint8: seconds showing the clock         */
#define CLK_ATTR_ENV_DWELL     0x000B  /* uint8: seconds per temp/humidity screen  */
