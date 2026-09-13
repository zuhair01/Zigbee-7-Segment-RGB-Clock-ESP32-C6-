/*
 * main.c — entry point for the Zigbee 7-segment RGB clock.
 *
 * Boot sequence:
 *   1. init NVS (settings persistence)
 *   2. start the LED render task (shows "--:--" until time syncs)
 *   3. start the ambient light sensor + auto-brightness engine
 *   4. start the DHT11 temperature/humidity sampler
 *   5. start the Zigbee stack (joins network, syncs time, handles commands)
 *   6. watch the factory-reset button
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "clock_config.h"
#include "clock_render.h"
#include "zb_device.h"
#include "settings.h"
#include "ambient.h"
#include "dht.h"
#include "esp_zigbee_core.h"

static const char *TAG = "app";

/* Hold the BOOT button ~5 s to leave the network and factory-reset Zigbee. */
static void factory_reset_task(void *arg) {
    (void)arg;
    if (FACTORY_RESET_GPIO < 0) { vTaskDelete(NULL); return; }
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << FACTORY_RESET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int held = 0;
    for (;;) {
        if (gpio_get_level(FACTORY_RESET_GPIO) == 0) {
            if (++held >= 50) {   /* 50 * 100 ms = 5 s */
                ESP_LOGW(TAG, "factory reset: leaving network");
                esp_zb_factory_reset();   /* erases Zigbee NVRAM and reboots */
            }
        } else {
            held = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Zigbee 7-segment RGB clock starting");

    settings_init();
    clock_render_start();
    ambient_start();
    dht_start();
    zb_device_start();

    xTaskCreate(factory_reset_task, "factory_rst", 2560, NULL, 3, NULL);
}
