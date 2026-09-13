/*
 * ambient.h — ambient light sensing + automatic brightness computation.
 *
 * Samples an analog LDR on ADC1, smooths it, and maps it through a gamma curve
 * between a configurable [min,max] brightness window. The result is applied to
 * the render brightness when auto-brightness is enabled.
 *
 * It also exposes an estimated illuminance (lux-ish) and the current auto
 * brightness so the Zigbee layer can report them to Home Assistant.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Start the ADC sampler + auto-brightness task. */
void ambient_start(void);

/* Runtime configuration (driven by Zigbee custom attributes / NVS). */
void ambient_set_enabled(bool en);
void ambient_set_bounds(uint8_t min_level, uint8_t max_level);

/* Latest computed values (thread-safe snapshots). */
uint8_t  ambient_get_auto_level(void);   /* 0..254 brightness auto wants       */
uint16_t ambient_get_lux(void);          /* estimated illuminance (relative)   */
uint16_t ambient_get_raw(void);          /* smoothed raw ADC (for calibration) */
bool     ambient_is_enabled(void);
