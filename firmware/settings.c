/* settings.c — NVS-backed persistence for clock preferences. */
#include "settings.h"
#include "clock_config.h"
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static const char *NS  = "segclock";
static const char *KEY = "cfg";

void settings_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void defaults(clk_settings_t *s) {
    *s = (clk_settings_t){
        .brightness = 160,
        .main_color = {0, 180, 255},
        .hour_color = {0, 0, 0},
        .min_color  = {0, 0, 0},
        .mode_24h = true,
        .leading_zero = false,
        .colon_mode = COLON_BLINK,
        .effect = FX_SOLID,
        .power = true,
        .auto_bright = true,   /* always-automatic by default */
        .min_bright = 12,
        .max_bright = 230,
        .show_env = DEFAULT_SHOW_ENV,
        .time_dwell_s = DEFAULT_TIME_DWELL_S,
        .env_dwell_s = DEFAULT_ENV_DWELL_S,
    };
}

void settings_load(clk_settings_t *out) {
    defaults(out);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored settings, using defaults");
        return;
    }
    size_t len = sizeof(*out);
    clk_settings_t tmp;
    if (nvs_get_blob(h, KEY, &tmp, &len) == ESP_OK && len == sizeof(tmp)) {
        *out = tmp;
        ESP_LOGI(TAG, "loaded settings from NVS");
    }
    nvs_close(h);
}

void settings_save(const clk_settings_t *in) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed, not saving");
        return;
    }
    if (nvs_set_blob(h, KEY, in, sizeof(*in)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}
