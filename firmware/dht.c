/*
 * dht.c — bit-banged DHT11 / DHT22 driver for ESP32-C6 (ESP-IDF 5.3+).
 *
 * The single-wire DHT protocol:
 *   1. MCU pulls the line LOW for ~18 ms (DHT11) to request a sample, releases.
 *   2. Sensor answers: ~80 us LOW, ~80 us HIGH.
 *   3. 40 data bits: each bit = ~50 us LOW then a HIGH whose length encodes the
 *      bit (~26-28 us = '0', ~70 us = '1').
 *   4. 40 bits = humidity(16) + temperature(16) + checksum(8).
 *
 * Timing is measured with esp_timer inside a short critical section. This is the
 * classic, dependency-free approach and coexists fine with the Zigbee stack
 * because a full frame is < 5 ms and only runs once every few seconds.
 *
 * DHT11 vs DHT22 differ only in how the 16-bit fields are decoded (see below),
 * selected by DHT_TYPE in clock_config.h.
 */
#include "dht.h"
#include "clock_config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"          /* esp_rom_delay_us */
#include "driver/gpio.h"

static const char *TAG = "dht";

static SemaphoreHandle_t s_lock;
static float s_temp_c = 0.0f;
static float s_hum    = 0.0f;
static bool  s_valid  = false;

/* Wait until the line reaches `level`, up to `timeout_us`. Returns elapsed us,
 * or -1 on timeout. */
static int wait_level(int level, int timeout_us) {
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(DHT_GPIO) != level) {
        if ((esp_timer_get_time() - start) > timeout_us) return -1;
    }
    return (int)(esp_timer_get_time() - start);
}

/* Perform one full read into raw[5]. Returns true on success (checksum ok). */
static bool dht_read_raw(uint8_t raw[5]) {
    int64_t high_us[40];
    memset(raw, 0, 5);

    /* ---- Start signal: hold low, then release ---- */
    gpio_set_direction(DHT_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT_GPIO, 0);
    /* DHT11 needs >=18 ms low; DHT22 needs >=1 ms. Use 20 ms to be safe. */
    esp_rom_delay_us(20 * 1000);
    gpio_set_level(DHT_GPIO, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(DHT_GPIO, GPIO_MODE_INPUT);

    /* ---- Timing-critical capture ---- */
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&mux);

    bool ok = true;
    /* Sensor response: ~80us low then ~80us high. */
    if (wait_level(0, 200) < 0) ok = false;   /* line goes low  */
    else if (wait_level(1, 200) < 0) ok = false; /* then high    */
    else if (wait_level(0, 200) < 0) ok = false; /* start of first bit's low */

    if (ok) {
        for (int i = 0; i < 40; i++) {
            /* Each bit: ~50us low (already at low), then a high pulse. */
            if (wait_level(1, 100) < 0) { ok = false; break; }  /* rising edge */
            int64_t t0 = esp_timer_get_time();
            if (wait_level(0, 200) < 0) { ok = false; break; }  /* falling edge */
            high_us[i] = esp_timer_get_time() - t0;
        }
    }
    taskEXIT_CRITICAL(&mux);

    if (!ok) return false;

    /* Decode: high pulse > ~40us => bit '1'. */
    for (int i = 0; i < 40; i++) {
        raw[i / 8] <<= 1;
        if (high_us[i] > 40) raw[i / 8] |= 1;
    }

    uint8_t sum = raw[0] + raw[1] + raw[2] + raw[3];
    if (sum != raw[4]) {
        ESP_LOGW(TAG, "checksum fail %02x%02x%02x%02x sum=%02x != %02x",
                 raw[0], raw[1], raw[2], raw[3], sum, raw[4]);
        return false;
    }
    return true;
}

static bool dht_sample(float *t_c, float *hum) {
    uint8_t raw[5];
    if (!dht_read_raw(raw)) return false;

#if DHT_TYPE == 22
    /* DHT22/AM2302: 16-bit tenths, big-endian; temp sign in MSB of byte[2]. */
    uint16_t rh = ((uint16_t)raw[0] << 8) | raw[1];
    uint16_t tt = ((uint16_t)(raw[2] & 0x7F) << 8) | raw[3];
    *hum = rh / 10.0f;
    *t_c = tt / 10.0f;
    if (raw[2] & 0x80) *t_c = -*t_c;
#else
    /* DHT11: byte[0]=RH integer, byte[1]=RH decimal, byte[2]=T integer,
     * byte[3]=T decimal (newer DHT11 use the decimal bytes). */
    *hum = raw[0] + raw[1] * 0.1f;
    *t_c = raw[2] + (raw[3] & 0x7F) * 0.1f;
    if (raw[3] & 0x80) *t_c = -*t_c;
#endif

    /* Sanity clamp against obviously bad frames. */
    if (*hum < 0 || *hum > 100 || *t_c < -40 || *t_c > 80) return false;
    return true;
}

static void dht_task(void *arg) {
    (void)arg;
    /* Idle-high line (external pull-up present). */
    gpio_set_direction(DHT_GPIO, GPIO_MODE_INPUT);
    /* DHT needs ~1 s to stabilise after power-on. */
    vTaskDelay(pdMS_TO_TICKS(1500));

    int fails = 0;
    for (;;) {
        float t, h;
        if (dht_sample(&t, &h)) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_temp_c = t; s_hum = h; s_valid = true;
            xSemaphoreGive(s_lock);
            fails = 0;
            ESP_LOGI(TAG, "%.1f C, %.1f %%RH", t, h);
        } else if (++fails % 5 == 0) {
            ESP_LOGW(TAG, "%d consecutive read failures (wiring/pull-up?)", fails);
        }
        vTaskDelay(pdMS_TO_TICKS(DHT_READ_INTERVAL_MS));
    }
}

void dht_start(void) {
    s_lock = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "DHT%d on GPIO%d, interval %d ms", DHT_TYPE, DHT_GPIO, DHT_READ_INTERVAL_MS);
    /* Higher priority so the timing-critical read isn't preempted mid-frame,
     * but it sleeps almost all the time. */
    xTaskCreate(dht_task, "dht", 3072, NULL, 6, NULL);
}

bool dht_get(float *temperature_c, float *humidity_pct) {
    bool v;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (temperature_c) *temperature_c = s_temp_c;
    if (humidity_pct)  *humidity_pct  = s_hum;
    v = s_valid;
    xSemaphoreGive(s_lock);
    return v;
}

bool dht_get_display(int *temp_c_int, int *humidity_int) {
    float t, h;
    if (!dht_get(&t, &h)) return false;
    if (temp_c_int)  *temp_c_int  = (int)(t + (t >= 0 ? 0.5f : -0.5f));
    if (humidity_int) *humidity_int = (int)(h + 0.5f);
    return true;
}

bool dht_valid(void) {
    bool v;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    v = s_valid;
    xSemaphoreGive(s_lock);
    return v;
}
