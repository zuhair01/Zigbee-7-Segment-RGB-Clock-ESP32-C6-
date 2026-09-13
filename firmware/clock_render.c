/*
 * clock_render.c — LED strip driver + 7-segment font + effects engine.
 *
 * Segment naming (CHAIN ORDER — letters follow the data wire from the input,
 * matching the CAD wiring drawing):
 *
 *        ddd
 *       c   e
 *       c   e
 *        fff
 *       b   g
 *       b   g
 *        aaa
 *
 *   a = bottom       (input, 1st segment in the chain)
 *   b = lower-left
 *   c = upper-left
 *   d = top
 *   e = upper-right
 *   f = middle
 *   g = lower-right  (last segment in the chain)
 *
 * Because the letters ARE the chain order, SEGMENT_ORDER is the identity map.
 * SEGMENT_MAP below defines which segments are lit for digits 0-9 in this
 * naming. Each segment is LEDS_PER_SEGMENT consecutive pixels. If you ever
 * rewire, change SEGMENT_ORDER only — the font (SEGMENT_MAP) stays the same.
 */
#include "clock_render.h"
#include "clock_config.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

static const char *TAG = "render";

/* ---- 7-segment font ----
 * Bit order {a,b,c,d,e,f,g} where a is the MSB (bit 6), g is the LSB (bit 0).
 * With the chain-order naming above:
 *   a=bottom  b=lower-left  c=upper-left  d=top  e=upper-right  f=middle  g=lower-right
 *
 *        ddd
 *       c   e
 *        fff
 *       b   g
 *        aaa
 */
static const uint8_t SEGMENT_MAP[10] = {
    /*        abcdefg */
    /* 0 */ 0b1111101,  /* all but middle(f) */
    /* 1 */ 0b0000101,  /* e + g (right side) */
    /* 2 */ 0b1101110,  /* d,e,f,b,a */
    /* 3 */ 0b1001111,  /* d,a,e,f,g */
    /* 4 */ 0b0010111,  /* c,e,f,g */
    /* 5 */ 0b1011011,  /* d,c,f,g,a */
    /* 6 */ 0b1111011,  /* d,c,b,f,g,a */
    /* 7 */ 0b0001101,  /* d,e,g */
    /* 8 */ 0b1111111,  /* all */
    /* 9 */ 0b1011111,  /* d,c,e,f,g,a */
};
#define SEG_DASH 0b0000010  /* just the middle 'f' segment => "-" */

/* ---- Letter / symbol glyphs (same chain-order bit convention) ----
 * Used by the temperature/humidity screens, e.g. "23°C" and "48%RH" style. */
#define GLYPH_BLANK  0b0000000
#define GLYPH_C      0b1111000  /* 'C' : top, upper-left, lower-left, bottom */
#define GLYPH_H      0b0110111  /* 'H' : both sides + middle */
#define GLYPH_DEG    0b0011110  /* '°' : small ring top-upper-left-upper-right-middle */
#define GLYPH_F      0b0111010  /* 'F' : top, both left, middle */
#define GLYPH_h      0b0110011  /* 'h' : upper/lower-left, lower-right, middle */
#define GLYPH_P      0b0111110  /* 'P' : top, both left, upper-right, middle */

/* Map logical segment (a=0..g=6) to its position in the wiring chain.
 * With chain-order naming the letters already follow the wire, so this is the
 * identity map: a=pos0 (input) ... g=pos6 (last). Rewire => edit this only. */
static const uint8_t SEGMENT_ORDER[SEGS_PER_DIGIT] = {0, 1, 2, 3, 4, 5, 6};

static led_strip_handle_t s_strip;
static SemaphoreHandle_t  s_lock;
static display_state_t    s_state;

/* ------------- helpers ------------- */
static inline rgb_t pick_color(rgb_t specific, rgb_t fallback) {
    if (specific.r || specific.g || specific.b) return specific;
    return fallback;
}

static inline rgb_t scale(rgb_t c, uint8_t level) {
    c.r = (uint16_t)c.r * level / 255;
    c.g = (uint16_t)c.g * level / 255;
    c.b = (uint16_t)c.b * level / 255;
    return c;
}

/* HSV(0..255) -> RGB, for rainbow effect */
static rgb_t hsv(uint8_t h, uint8_t s, uint8_t v) {
    rgb_t out;
    uint8_t region = h / 43;
    uint8_t rem = (h - region * 43) * 6;
    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * rem) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;
    switch (region) {
        case 0:  out = (rgb_t){v, t, p}; break;
        case 1:  out = (rgb_t){q, v, p}; break;
        case 2:  out = (rgb_t){p, v, t}; break;
        case 3:  out = (rgb_t){p, q, v}; break;
        case 4:  out = (rgb_t){t, p, v}; break;
        default: out = (rgb_t){v, p, q}; break;
    }
    return out;
}

/* Segment writer. led_strip_set_pixel takes (strip, idx, R, G, B); the driver
 * reorders to GRB on the wire because we set color_component_format at init. */
static void draw_segment(int digit_first_led, int logical_seg, rgb_t c, bool on) {
    int seg_pos = SEGMENT_ORDER[logical_seg];
    int base = digit_first_led + seg_pos * LEDS_PER_SEGMENT;
    for (int i = 0; i < LEDS_PER_SEGMENT; i++) {
        if (on)  led_strip_set_pixel(s_strip, base + i, c.r, c.g, c.b);
        else     led_strip_set_pixel(s_strip, base + i, 0, 0, 0);
    }
}

/* Draw an arbitrary 7-bit segment mask onto a digit position. */
static void draw_mask(int digit_index, uint8_t mask, rgb_t color) {
    int first = digit_index * LEDS_PER_DIGIT;
    for (int seg = 0; seg < SEGS_PER_DIGIT; seg++) {
        bool on = (mask >> (6 - seg)) & 0x01;       /* seg 0=a is MSB */
        draw_segment(first, seg, color, on);
    }
}

/* Draw a single digit (0-9 or -1 for blank, -2 for dash). */
static void draw_digit(int digit_index, int value, rgb_t color) {
    uint8_t mask;
    if (value == -2)      mask = SEG_DASH;
    else if (value < 0)   mask = GLYPH_BLANK;       /* blank */
    else                  mask = SEGMENT_MAP[value % 10];
    draw_mask(digit_index, mask, color);
}

static void draw_colon(rgb_t color, bool on) {
    for (int i = 0; i < COLON_LEDS; i++) {
        if (on) led_strip_set_pixel(s_strip, COLON_START_INDEX + i, color.r, color.g, color.b);
        else    led_strip_set_pixel(s_strip, COLON_START_INDEX + i, 0, 0, 0);
    }
}

/* ------------- the frame renderer ------------- */
static void render_frame(uint32_t frame) {
    display_state_t st;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st = s_state;
    xSemaphoreGive(s_lock);

    if (!st.power) {
        led_strip_clear(s_strip);
        led_strip_refresh(s_strip);
        return;
    }

    /* Effect-driven brightness / color modulation */
    uint8_t level = st.brightness;
    rgb_t hour_c = pick_color(st.hour_color, st.main_color);
    rgb_t min_c  = pick_color(st.min_color,  st.main_color);

    if (st.effect == FX_BREATHE) {
        /* 0.25 Hz breathing using a raised cosine */
        float phase = (frame % (RENDER_FPS * 4)) / (float)(RENDER_FPS * 4);
        float b = 0.15f + 0.85f * (0.5f - 0.5f * cosf(phase * 2.0f * (float)M_PI));
        level = (uint8_t)(st.brightness * b);
    } else if (st.effect == FX_RAINBOW) {
        uint8_t base_h = (frame * 2) & 0xFF;
        hour_c = hsv(base_h, 255, 255);
        min_c  = hsv(base_h + 64, 255, 255);
    } else if (st.effect == FX_GRADIENT) {
        uint8_t base_h = (frame) & 0xFF;
        hour_c = hsv(base_h, 220, 255);
        min_c  = hsv(base_h + 96, 220, 255);
    }

    rgb_t hc = scale(hour_c, level);
    rgb_t mc = scale(min_c, level);
    rgb_t ec = scale(st.main_color, level);   /* env screens use main color */

    /* ---- Screen selection: cycle time -> temp -> humidity ---- */
    /* screen: 0 = time, 1 = temperature, 2 = humidity */
    int screen = 0;
    if (st.show_env && st.env_valid) {
        uint8_t td = st.time_dwell_s ? st.time_dwell_s : 1;
        uint8_t ed = st.env_dwell_s ? st.env_dwell_s : 1;
        uint32_t period = td + 2u * ed;                 /* full cycle, seconds */
        uint32_t sec = (frame / RENDER_FPS) % period;   /* free-running phase */
        if (sec < td)            screen = 0;
        else if (sec < td + ed)  screen = 1;
        else                     screen = 2;
    }

    if (screen == 1) {
        /* Temperature: e.g. "23°C". Sign-aware: negatives show "-9°C" style. */
        int t = st.temp_c;
        bool neg = t < 0; if (neg) t = -t;
        int tens = t / 10, ones = t % 10;
        draw_colon(ec, false);
        if (neg && tens == 0) { draw_digit(0, -2, ec); }        /* '-' */
        else if (tens == 0)   { draw_digit(0, -1, ec); }        /* blank leading */
        else                  { draw_digit(0, tens, ec); }
        draw_digit(1, ones, ec);
        draw_mask(2, GLYPH_DEG, ec);
        draw_mask(3, GLYPH_C, ec);
        led_strip_refresh(s_strip);
        return;
    }
    if (screen == 2) {
        /* Humidity: e.g. "48H" (H = humidity marker). */
        int hpct = st.humidity; if (hpct > 99) hpct = 99;
        int tens = hpct / 10, ones = hpct % 10;
        draw_colon(ec, false);
        if (tens == 0) draw_digit(0, -1, ec); else draw_digit(0, tens, ec);
        draw_digit(1, ones, ec);
        draw_mask(2, GLYPH_H, ec);
        draw_mask(3, GLYPH_BLANK, ec);
        led_strip_refresh(s_strip);
        return;
    }

    /* Compute displayed digits */
    if (!st.time_valid) {
        /* Not synced yet: show "--:--" gently */
        rgb_t dim = scale(st.main_color, level / 3 ? level / 3 : 20);
        draw_digit(0, -2, dim); draw_digit(1, -2, dim);
        draw_digit(2, -2, dim); draw_digit(3, -2, dim);
        draw_colon(dim, (frame / (RENDER_FPS/2)) & 1);
        led_strip_refresh(s_strip);
        return;
    }

    int hh = st.hour;
    if (!st.mode_24h) {
        hh = st.hour % 12;
        if (hh == 0) hh = 12;
    }
    int h_tens = hh / 10;
    int h_ones = hh % 10;
    int m_tens = st.minute / 10;
    int m_ones = st.minute % 10;

    /* Leading-zero blanking for the hour tens digit */
    if (!st.leading_zero && h_tens == 0) h_tens = -1;

    draw_digit(0, h_tens, hc);
    draw_digit(1, h_ones, hc);
    draw_digit(2, m_tens, mc);
    draw_digit(3, m_ones, mc);

    /* Colon behaviour */
    bool colon_on = true;
    if (st.colon_mode == COLON_OFF)   colon_on = false;
    if (st.colon_mode == COLON_BLINK) colon_on = (st.second & 1) == 0;
    rgb_t colon_c = scale(st.main_color, level);
    draw_colon(colon_c, colon_on);

    led_strip_refresh(s_strip);
}

/* ------------- task ------------- */
static void render_task(void *arg) {
    (void)arg;
    uint32_t frame = 0;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        render_frame(frame++);
        vTaskDelayUntil(&last, pdMS_TO_TICKS(RENDER_PERIOD_MS));
    }
}

/* ------------- init + setters ------------- */
void clock_render_start(void) {
    s_lock = xSemaphoreCreateMutex();

    /* Sensible defaults until Zigbee/NVS overrides them */
    s_state = (display_state_t){
        .power = true,
        .brightness = 160,
        .main_color = {0, 180, 255},   /* cyan-ish */
        .hour_color = {0, 0, 0},
        .min_color  = {0, 0, 0},
        .mode_24h = true,
        .leading_zero = false,
        .colon_mode = COLON_BLINK,
        .effect = FX_SOLID,
        .hour = 0, .minute = 0, .second = 0,
        .time_valid = false,
        .show_env = DEFAULT_SHOW_ENV,
        .time_dwell_s = DEFAULT_TIME_DWELL_S,
        .env_dwell_s = DEFAULT_ENV_DWELL_S,
        .temp_c = 0, .humidity = 0, .env_valid = false,
    };

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_DATA_GPIO,
        .max_leds = TOTAL_LEDS,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_STRIP_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .flags = { .with_dma = false },
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);

    ESP_LOGI(TAG, "LED strip ready: %d pixels on GPIO %d", TOTAL_LEDS, LED_DATA_GPIO);
    xTaskCreate(render_task, "render", 4096, NULL, 5, NULL);
}

#define WITH_LOCK(body) do { \
    xSemaphoreTake(s_lock, portMAX_DELAY); body; xSemaphoreGive(s_lock); } while (0)

void render_set_power(bool on)              { WITH_LOCK(s_state.power = on); }
void render_set_brightness(uint8_t level)   { WITH_LOCK(s_state.brightness = level); }
void render_set_main_color(rgb_t c)         { WITH_LOCK(s_state.main_color = c); }
void render_set_hour_color(rgb_t c)         { WITH_LOCK(s_state.hour_color = c); }
void render_set_min_color(rgb_t c)          { WITH_LOCK(s_state.min_color = c); }
void render_set_mode_24h(bool en)           { WITH_LOCK(s_state.mode_24h = en); }
void render_set_leading_zero(bool en)       { WITH_LOCK(s_state.leading_zero = en); }
void render_set_colon_mode(colon_mode_t m)  { WITH_LOCK(s_state.colon_mode = m); }
void render_set_effect(effect_t fx)         { WITH_LOCK(s_state.effect = fx); }

void render_set_time(uint8_t h, uint8_t m, uint8_t s, bool valid) {
    WITH_LOCK({
        s_state.hour = h; s_state.minute = m; s_state.second = s;
        s_state.time_valid = valid;
    });
}

void render_set_show_env(bool en) { WITH_LOCK(s_state.show_env = en); }

void render_set_env_dwell(uint8_t time_dwell_s, uint8_t env_dwell_s) {
    WITH_LOCK({
        s_state.time_dwell_s = time_dwell_s;
        s_state.env_dwell_s = env_dwell_s;
    });
}

void render_set_environment(int temp_c, int humidity, bool valid) {
    WITH_LOCK({
        s_state.temp_c = temp_c;
        s_state.humidity = humidity;
        s_state.env_valid = valid;
    });
}

void render_tick_second(void) {
    WITH_LOCK({
        if (s_state.time_valid) {
            if (++s_state.second >= 60) {
                s_state.second = 0;
                if (++s_state.minute >= 60) {
                    s_state.minute = 0;
                    if (++s_state.hour >= 24) s_state.hour = 0;
                }
            }
        }
    });
}

void render_get_state(display_state_t *out) {
    WITH_LOCK(*out = s_state);
}
