/*
 * ambient.c — LDR sampling + auto-brightness engine.
 *
 * Pipeline per sample (every AMBIENT_SAMPLE_MS):
 *   raw ADC  ->  (optional invert)  ->  EMA smoothing
 *            ->  normalise to 0..1 using [LDR_RAW_DARK, LDR_RAW_BRIGHT]
 *            ->  gamma curve
 *            ->  scale into [min_level, max_level]
 *            ->  slew-limit vs previous output (anti-flicker)
 *            ->  apply to render brightness (if enabled)
 *
 * The gamma curve (<1) lets the display brighten quickly as light rises while
 * still settling to a low floor at night. Slew limiting caps how fast the
 * brightness can move so passing shadows/headlights don't cause visible steps.
 */
#include "ambient.h"
#include "clock_config.h"
#include "clock_render.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "ambient";

static adc_oneshot_unit_handle_t s_adc;
static volatile bool     s_enabled   = true;
static volatile uint8_t  s_min_level = 12;    /* night floor  */
static volatile uint8_t  s_max_level = 230;   /* daytime cap  */

static volatile uint16_t s_raw_smooth = 0;
static volatile uint16_t s_lux        = 0;
static volatile uint8_t  s_auto_level = 128;

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void ambient_task(void *arg) {
    (void)arg;
    float ema = -1.0f;
    int   out = s_auto_level;
    uint32_t elapsed = 0;

    for (;;) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, LDR_ADC_CHANNEL, &raw) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(AMBIENT_SAMPLE_MS));
            continue;
        }
#if LDR_INVERT
        raw = 4095 - raw;
#endif
        /* Exponential moving average for a stable reading. */
        if (ema < 0) ema = raw;
        else ema += AMBIENT_EMA_ALPHA * (raw - ema);
        s_raw_smooth = (uint16_t)ema;

        /* Normalise against calibrated dark/bright endpoints. */
        float norm = (ema - LDR_RAW_DARK) / (float)(LDR_RAW_BRIGHT - LDR_RAW_DARK);
        norm = clampf(norm, 0.0f, 1.0f);

        /* Estimated (relative) lux for the Zigbee illuminance sensor. */
        s_lux = (uint16_t)(norm * LDR_LUX_FULLSCALE);

        /* Gamma curve, then scale into the configured brightness window. */
        float curved = powf(norm, AMBIENT_CURVE_GAMMA);
        int target = s_min_level + (int)lroundf(curved * (s_max_level - s_min_level));
        target = (int)clampf(target, 0, 254);

        /* Slew-limit to avoid visible jumps. */
        if (target > out)      out += (target - out > AMBIENT_SLEW_PER_STEP) ? AMBIENT_SLEW_PER_STEP : (target - out);
        else if (target < out) out -= (out - target > AMBIENT_SLEW_PER_STEP) ? AMBIENT_SLEW_PER_STEP : (out - target);

        s_auto_level = (uint8_t)out;

        if (s_enabled) {
            render_set_brightness((uint8_t)out);
        }

        elapsed += AMBIENT_SAMPLE_MS;
        if (elapsed >= 5000) {
            elapsed = 0;
            ESP_LOGI(TAG, "ambient raw=%u norm=%.2f lux~%u auto_level=%u %s",
                     (unsigned)s_raw_smooth, norm, (unsigned)s_lux, (unsigned)out,
                     s_enabled ? "(applied)" : "(manual)");
        }
        vTaskDelay(pdMS_TO_TICKS(AMBIENT_SAMPLE_MS));
    }
}

void ambient_start(void) {
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,        /* full ~0..3.3V input range */
        .bitwidth = ADC_BITWIDTH_12,     /* 0..4095 */
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, LDR_ADC_CHANNEL, &chan_cfg));

    ESP_LOGI(TAG, "LDR on ADC1 CH%d (GPIO%d), dark=%d bright=%d",
             LDR_ADC_CHANNEL, LDR_ADC_GPIO, LDR_RAW_DARK, LDR_RAW_BRIGHT);

    xTaskCreate(ambient_task, "ambient", 3072, NULL, 4, NULL);
}

void ambient_set_enabled(bool en) {
    s_enabled = en;
    ESP_LOGI(TAG, "auto-brightness %s", en ? "ENABLED" : "disabled (manual)");
}

void ambient_set_bounds(uint8_t min_level, uint8_t max_level) {
    if (min_level > max_level) { uint8_t t = min_level; min_level = max_level; max_level = t; }
    s_min_level = min_level;
    s_max_level = max_level;
    ESP_LOGI(TAG, "auto-brightness bounds [%u..%u]", min_level, max_level);
}

uint8_t  ambient_get_auto_level(void) { return s_auto_level; }
uint16_t ambient_get_lux(void)        { return s_lux; }
uint16_t ambient_get_raw(void)        { return s_raw_smooth; }
bool     ambient_is_enabled(void)     { return s_enabled; }
