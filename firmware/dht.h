/*
 * dht.h — minimal DHT11/DHT22 (single-wire) temperature + humidity driver.
 *
 * Runs a background task that samples the sensor every DHT_READ_INTERVAL_MS and
 * publishes the latest reading via thread-safe getters. Values are also handed
 * to the render layer (for on-display temp/humidity screens) and to the Zigbee
 * layer (standard Temperature/Humidity Measurement clusters).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Start the DHT sampling task. */
void dht_start(void);

/* Latest reading (thread-safe). Returns false if no valid reading yet. */
bool dht_get(float *temperature_c, float *humidity_pct);

/* Convenience: rounded integers for the 7-seg display; return false if stale. */
bool dht_get_display(int *temp_c_int, int *humidity_int);

/* True once at least one valid reading has been captured. */
bool dht_valid(void);
