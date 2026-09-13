/*
 * zb_device.c — Zigbee 3.0 device definition + attribute callbacks.
 *
 * Endpoint 10 is a Color Dimmable Light with these clusters:
 *   - Basic        (0x0000)  identity / manufacturer / model
 *   - Identify     (0x0003)
 *   - Groups       (0x0004)
 *   - Scenes       (0x0005)
 *   - On/Off       (0x0006)  power
 *   - Level        (0x0008)  brightness -> LED brightness
 *   - Color Ctrl   (0x0300)  XY + Hue/Sat -> main_color
 *   - Time         (0x000A)  time sync FROM coordinator (client-side read)
 *   - Illuminance  (0x0400)  ambient light reading -> HA lux sensor
 *   - Temperature  (0x0402)  DHT11 temperature -> HA sensor
 *   - Humidity     (0x0405)  DHT11 humidity    -> HA sensor
 *   - Custom       (0xFC00)  clock behaviour + auto-brightness + env-cycle attrs
 *
 * Because On/Off + Level + Color are standard, ZHA and Zigbee2MQTT show a full
 * RGB light with zero configuration. The custom cluster adds clock controls,
 * surfaced via the external converter / quirk in tools/. The Illuminance,
 * Temperature and Humidity Measurement clusters expose the sensors as standard
 * HA entities; the auto-brightness engine (ambient.c) drives Level Control and
 * the render layer cycles temp/humidity screens onto the display.
 */
#include "zb_device.h"
#include "clock_config.h"
#include "clock_render.h"
#include "settings.h"
#include "ambient.h"
#include "dht.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_zigbee_core.h"
#include "nwk/esp_zigbee_nwk.h"

static const char *TAG = "zb";

/* Persisted settings mirror; saved on any custom-attr / level / color change. */
static clk_settings_t s_cfg;

static void persist(void) { settings_save(&s_cfg); }

/* ---- HS/XY -> RGB conversion for Color Control ---- */
static rgb_t xy_to_rgb(uint16_t x16, uint16_t y16, uint8_t level) {
    /* CIE xyY -> sRGB (Y fixed to 1.0; brightness applied separately). */
    float x = x16 / 65535.0f;
    float y = y16 / 65535.0f;
    if (y <= 0.0f) return (rgb_t){0, 0, 0};
    float Y = 1.0f;
    float X = (Y / y) * x;
    float Z = (Y / y) * (1.0f - x - y);
    float r =  3.2406f * X - 1.5372f * Y - 0.4986f * Z;
    float g = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
    float b =  0.0557f * X - 0.2040f * Y + 1.0570f * Z;
    /* gamma + clamp */
    float m = fmaxf(r, fmaxf(g, b));
    if (m > 1.0f) { r /= m; g /= m; b /= m; }
    #define G(c) (c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1/2.4f) - 0.055f)
    r = G(r < 0 ? 0 : r); g = G(g < 0 ? 0 : g); b = G(b < 0 ? 0 : b);
    #undef G
    rgb_t out = {
        (uint8_t)(fmaxf(0, fminf(1, r)) * 255),
        (uint8_t)(fmaxf(0, fminf(1, g)) * 255),
        (uint8_t)(fmaxf(0, fminf(1, b)) * 255),
    };
    return out;
}

static rgb_t hs_to_rgb(uint8_t hue8, uint8_t sat8) {
    float h = hue8 / 255.0f * 360.0f;
    float s = sat8 / 255.0f;
    float c = s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = 1 - c;
    float r, g, b;
    if      (h <  60) { r=c; g=x; b=0; }
    else if (h < 120) { r=x; g=c; b=0; }
    else if (h < 180) { r=0; g=c; b=x; }
    else if (h < 240) { r=0; g=x; b=c; }
    else if (h < 300) { r=x; g=0; b=c; }
    else              { r=c; g=0; b=x; }
    return (rgb_t){(uint8_t)((r+m)*255), (uint8_t)((g+m)*255), (uint8_t)((b+m)*255)};
}

/* ---------------- attribute write handler ---------------- */
static esp_err_t attr_cb(const esp_zb_zcl_set_attr_value_message_t *m) {
    ESP_RETURN_ON_FALSE(m, ESP_FAIL, TAG, "empty attr msg");
    ESP_RETURN_ON_FALSE(m->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_FAIL, TAG,
                        "attr status 0x%x", m->info.status);

    const uint16_t cluster = m->info.cluster;
    const uint16_t attr    = m->attribute.id;
    const void    *val     = m->attribute.data.value;

    switch (cluster) {
    case ESP_ZB_ZCL_CLUSTER_ID_ON_OFF:
        if (attr == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
            s_cfg.power = *(bool *)val;
            render_set_power(s_cfg.power);
            persist();
        }
        break;

    case ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL:
        if (attr == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID) {
            s_cfg.brightness = *(uint8_t *)val;
            /* When auto-brightness is on, ambient owns the display level; the
             * manual value is remembered but not applied until auto is disabled. */
            if (!s_cfg.auto_bright) render_set_brightness(s_cfg.brightness);
            persist();
        }
        break;

    case ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL: {
        /* We handle either XY or Hue/Sat depending on what the controller sends. */
        static uint16_t cx = 0, cy = 0;
        static uint8_t  hue = 0, sat = 0;
        if (attr == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_X_ID) cx = *(uint16_t *)val;
        else if (attr == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_Y_ID) cy = *(uint16_t *)val;
        else if (attr == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID) hue = *(uint8_t *)val;
        else if (attr == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID) sat = *(uint8_t *)val;

        rgb_t c;
        if (attr == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_HUE_ID ||
            attr == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_CURRENT_SATURATION_ID) {
            c = hs_to_rgb(hue, sat);
        } else {
            c = xy_to_rgb(cx, cy, s_cfg.brightness);
        }
        s_cfg.main_color = c;
        render_set_main_color(c);
        persist();
        break;
    }

    case CLK_CUSTOM_CLUSTER_ID:
        switch (attr) {
        case CLK_ATTR_MODE_24H:
            s_cfg.mode_24h = *(bool *)val; render_set_mode_24h(s_cfg.mode_24h); break;
        case CLK_ATTR_LEADING_ZERO:
            s_cfg.leading_zero = *(bool *)val; render_set_leading_zero(s_cfg.leading_zero); break;
        case CLK_ATTR_COLON_MODE:
            s_cfg.colon_mode = *(uint8_t *)val; render_set_colon_mode(s_cfg.colon_mode); break;
        case CLK_ATTR_EFFECT:
            s_cfg.effect = *(uint8_t *)val; render_set_effect(s_cfg.effect); break;
        case CLK_ATTR_HOUR_COLOR: {
            uint32_t v = 0; memcpy(&v, val, 3);
            rgb_t c = {(v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF};
            s_cfg.hour_color = c; render_set_hour_color(c); break;
        }
        case CLK_ATTR_MIN_COLOR: {
            uint32_t v = 0; memcpy(&v, val, 3);
            rgb_t c = {(v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF};
            s_cfg.min_color = c; render_set_min_color(c); break;
        }
        case CLK_ATTR_AUTO_BRIGHT:
            s_cfg.auto_bright = *(bool *)val;
            ambient_set_enabled(s_cfg.auto_bright);
            /* Leaving auto: restore the last manual brightness immediately. */
            if (!s_cfg.auto_bright) render_set_brightness(s_cfg.brightness);
            break;
        case CLK_ATTR_MIN_BRIGHT:
            s_cfg.min_bright = *(uint8_t *)val;
            ambient_set_bounds(s_cfg.min_bright, s_cfg.max_bright);
            break;
        case CLK_ATTR_MAX_BRIGHT:
            s_cfg.max_bright = *(uint8_t *)val;
            ambient_set_bounds(s_cfg.min_bright, s_cfg.max_bright);
            break;
        case CLK_ATTR_SHOW_ENV:
            s_cfg.show_env = *(bool *)val;
            render_set_show_env(s_cfg.show_env);
            break;
        case CLK_ATTR_TIME_DWELL:
            s_cfg.time_dwell_s = *(uint8_t *)val;
            render_set_env_dwell(s_cfg.time_dwell_s, s_cfg.env_dwell_s);
            break;
        case CLK_ATTR_ENV_DWELL:
            s_cfg.env_dwell_s = *(uint8_t *)val;
            render_set_env_dwell(s_cfg.time_dwell_s, s_cfg.env_dwell_s);
            break;
        default: break;
        }
        persist();
        break;

    default: break;
    }
    return ESP_OK;
}

/* ---------------- Time cluster sync ---------------- */
/* The coordinator's Time cluster stores UTC seconds since 2000-01-01. We read it
 * and (optionally) a timezone attribute, then convert to local wall-clock. */
static void apply_zb_time(uint32_t zb_utc, int32_t tz_offset_s) {
    /* Zigbee epoch = 2000-01-01 00:00:00 UTC. */
    long local = (long)zb_utc + tz_offset_s;
    if (local < 0) local = 0;
    long day_seconds = local % 86400;
    uint8_t h = day_seconds / 3600;
    uint8_t m = (day_seconds % 3600) / 60;
    uint8_t s = day_seconds % 60;
    render_set_time(h, m, s, true);
    ESP_LOGI(TAG, "time synced -> %02u:%02u:%02u (tz %+d s)", h, m, s, (int)tz_offset_s);
}

static void read_time_cb(esp_zb_zcl_cmd_read_attr_resp_message_t *msg) {
    if (!msg || msg->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_TIME) return;
    static uint32_t utc = 0;
    static int32_t  tz  = 0;
    esp_zb_zcl_read_attr_resp_variable_t *v = msg->variables;
    while (v) {
        if (v->status == ESP_ZB_ZCL_STATUS_SUCCESS && v->attribute.data.value) {
            if (v->attribute.id == ESP_ZB_ZCL_ATTR_TIME_TIME_ID)
                utc = *(uint32_t *)v->attribute.data.value;
            else if (v->attribute.id == ESP_ZB_ZCL_ATTR_TIME_TIME_ZONE_ID)
                tz = *(int32_t *)v->attribute.data.value;
        }
        v = v->next;
    }
    if (utc) apply_zb_time(utc, tz);
}

/* Poll the coordinator's Time cluster. Bound to the coordinator (addr 0x0000). */
static void request_time_sync(void) {
    uint16_t attrs[] = { ESP_ZB_ZCL_ATTR_TIME_TIME_ID, ESP_ZB_ZCL_ATTR_TIME_TIME_ZONE_ID };
    esp_zb_zcl_read_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = 0x0000,      /* coordinator */
            .dst_endpoint = 1,
            .src_endpoint = ZB_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_TIME,
        .attr_number = sizeof(attrs) / sizeof(attrs[0]),
        .attr_field = attrs,
    };
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_read_attr_cmd_req(&cmd);
    esp_zb_lock_release();
}

/* 1 Hz software clock; every 10 min we re-sync from the coordinator. */
static void time_task(void *arg) {
    (void)arg;
    uint32_t ticks = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        render_tick_second();
        if (++ticks % 600 == 0) request_time_sync();
    }
}

/* ---------------- ambient reporting ----------------
 * Push the LDR-derived illuminance to the standard Illuminance Measurement
 * cluster and mirror the auto-computed level into Level Control's CurrentLevel
 * so Home Assistant's brightness slider tracks reality. The ZCL MeasuredValue
 * is stored as 10000*log10(lux)+1 (0xFFFF = unknown / too dark). */
static uint16_t lux_to_measured(uint16_t lux) {
    if (lux == 0) return 0;                 /* "too dark to measure" floor */
    double v = 10000.0 * log10((double)lux) + 1.0;
    if (v < 1) v = 1;
    if (v > 0xFFFE) v = 0xFFFE;
    return (uint16_t)v;
}

static void report_ambient(void) {
    uint16_t measured = lux_to_measured(ambient_get_lux());
    uint8_t  level    = ambient_get_auto_level();

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_set_attribute_val(
        ZB_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID,
        &measured, false);

    /* Reflect the applied brightness while auto-brightness is active. */
    if (ambient_is_enabled()) {
        esp_zb_zcl_set_attribute_val(
            ZB_ENDPOINT,
            ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID,
            &level, false);
    }
    esp_zb_lock_release();
}

static void ambient_report_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(AMBIENT_REPORT_MS));
        report_ambient();
    }
}

/* ---------------- environment (DHT11) reporting ----------------
 * Pushes DHT readings to (a) the render layer for on-display screens, and
 * (b) the standard Temperature (0x0402, value in 0.01 C) and Humidity
 * (0x0405, value in 0.01 %RH) measurement clusters for Home Assistant. */
static void report_environment(void) {
    float t, h;
    if (!dht_get(&t, &h)) return;   /* no valid reading yet */

    int temp_i, hum_i;
    dht_get_display(&temp_i, &hum_i);
    render_set_environment(temp_i, hum_i, true);

    int16_t  zt = (int16_t)lroundf(t * 100.0f);   /* 0.01 C  */
    uint16_t zh = (uint16_t)lroundf(h * 100.0f);  /* 0.01 %  */

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_set_attribute_val(
        ZB_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
        &zt, false);
    esp_zb_zcl_set_attribute_val(
        ZB_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID,
        &zh, false);
    esp_zb_lock_release();
}

static void env_report_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(ENV_REPORT_MS));
        report_environment();
    }
}

/* ---------------- action dispatcher ---------------- */
static esp_err_t action_handler(esp_zb_core_action_callback_id_t id, const void *data) {
    switch (id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        return attr_cb((const esp_zb_zcl_set_attr_value_message_t *)data);
    case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID:
        read_time_cb((esp_zb_zcl_cmd_read_attr_resp_message_t *)data);
        return ESP_OK;
    default:
        ESP_LOGD(TAG, "unhandled action 0x%x", id);
        return ESP_OK;
    }
}

/* ---------------- commissioning / signals ---------------- */
static void bdb_start(void) {
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION));
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig = *p_sg_p;

    switch (sig) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "stack initialised, starting commissioning");
        bdb_start();
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "not joined; searching for a network");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "rejoined network; requesting time");
                request_time_sync();
            }
        } else {
            ESP_LOGW(TAG, "init failed (%s), retrying", esp_err_to_name(err));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start, 0, 2000);
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err == ESP_OK) {
            esp_zb_ieee_addr_t ext;
            esp_zb_get_extended_pan_id(ext);
            ESP_LOGI(TAG, "joined! PAN 0x%04hx ch %d",
                     esp_zb_get_pan_id(), esp_zb_get_current_channel());
            request_time_sync();
        } else {
            ESP_LOGI(TAG, "steering failed (%s), retrying", esp_err_to_name(err));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start, 0, 2000);
        }
        break;
    default:
        ESP_LOGD(TAG, "ZDO signal 0x%x status %s", sig, esp_err_to_name(err));
        break;
    }
}

/* ---------------- endpoint construction ---------------- */
static esp_zb_cluster_list_t *build_clusters(void) {
    esp_zb_color_dimmable_light_cfg_t light_cfg = ESP_ZB_DEFAULT_COLOR_DIMMABLE_LIGHT_CONFIG();
    esp_zb_cluster_list_t *cl = esp_zb_color_dimmable_light_clusters_create(&light_cfg);

    /* Overwrite Basic manufacturer/model strings. */
    esp_zb_attribute_list_t *basic = esp_zb_cluster_list_get_cluster(
        cl, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)ZB_MANUF_NAME);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)ZB_MODEL_ID);

    /* Add a Time cluster CLIENT so we can read the coordinator's time. */
    esp_zb_attribute_list_t *time_cli =
        esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_TIME);
    esp_zb_cluster_list_add_time_cluster(cl, time_cli, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    /* Illuminance Measurement (server) — exposes the LDR reading as a HA sensor. */
    esp_zb_illuminance_meas_cluster_cfg_t illum_cfg = {
        .measured_value   = 0,        /* updated at runtime */
        .min_value        = 0,
        .max_value        = 0xFFFE,
    };
    esp_zb_attribute_list_t *illum = esp_zb_illuminance_meas_cluster_create(&illum_cfg);
    esp_zb_cluster_list_add_illuminance_meas_cluster(cl, illum, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Temperature Measurement (server) — DHT11 temperature, value in 0.01 C.
     * DHT11 range 0..50 C => min 0, max 5000. */
    esp_zb_temperature_meas_cluster_cfg_t temp_cfg = {
        .measured_value = 0x8000,     /* 0x8000 = "invalid/unknown" until first read */
        .min_value      = 0,
        .max_value      = 5000,
    };
    esp_zb_attribute_list_t *temp = esp_zb_temperature_meas_cluster_create(&temp_cfg);
    esp_zb_cluster_list_add_temperature_meas_cluster(cl, temp, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Relative Humidity Measurement (server) — DHT11 humidity, value in 0.01 %.
     * DHT11 range 20..90 %RH => 2000..9000. */
    esp_zb_humidity_meas_cluster_cfg_t hum_cfg = {
        .measured_value = 0xFFFF,     /* 0xFFFF = "invalid/unknown" until first read */
        .min_value      = 0,
        .max_value      = 10000,
    };
    esp_zb_attribute_list_t *hum = esp_zb_humidity_meas_cluster_create(&hum_cfg);
    esp_zb_cluster_list_add_humidity_meas_cluster(cl, hum, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Custom manufacturer-specific cluster for clock behaviour. */
    esp_zb_attribute_list_t *custom =
        esp_zb_zcl_attr_list_create(CLK_CUSTOM_CLUSTER_ID);
    bool    b_true = true, b_false = false;
    uint8_t u0 = 0, u1 = 1;
    uint8_t u_min = 12, u_max = 230;
    uint8_t u_time_dwell = DEFAULT_TIME_DWELL_S, u_env_dwell = DEFAULT_ENV_DWELL_S;
    uint32_t rgb0 = 0;
    #define CATTR(id, type, access, val) \
        esp_zb_custom_cluster_add_custom_attr(custom, (id), (type), (access), (void*)(val))
    CATTR(CLK_ATTR_MODE_24H,     ESP_ZB_ZCL_ATTR_TYPE_BOOL, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &b_true);
    CATTR(CLK_ATTR_LEADING_ZERO, ESP_ZB_ZCL_ATTR_TYPE_BOOL, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &b_false);
    CATTR(CLK_ATTR_COLON_MODE,   ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &u1);
    CATTR(CLK_ATTR_EFFECT,       ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &u0);
    CATTR(CLK_ATTR_HOUR_COLOR,   ESP_ZB_ZCL_ATTR_TYPE_U32, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &rgb0);
    CATTR(CLK_ATTR_MIN_COLOR,    ESP_ZB_ZCL_ATTR_TYPE_U32, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &rgb0);
    CATTR(CLK_ATTR_AUTO_BRIGHT,  ESP_ZB_ZCL_ATTR_TYPE_BOOL, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &b_true);
    CATTR(CLK_ATTR_MIN_BRIGHT,   ESP_ZB_ZCL_ATTR_TYPE_U8, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &u_min);
    CATTR(CLK_ATTR_MAX_BRIGHT,   ESP_ZB_ZCL_ATTR_TYPE_U8, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &u_max);
    CATTR(CLK_ATTR_SHOW_ENV,     ESP_ZB_ZCL_ATTR_TYPE_BOOL, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &b_true);
    CATTR(CLK_ATTR_TIME_DWELL,   ESP_ZB_ZCL_ATTR_TYPE_U8, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &u_time_dwell);
    CATTR(CLK_ATTR_ENV_DWELL,    ESP_ZB_ZCL_ATTR_TYPE_U8, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &u_env_dwell);
    #undef CATTR
    esp_zb_cluster_list_add_custom_cluster(cl, custom, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    return cl;
}

static void zb_task(void *arg) {
    (void)arg;
    esp_zb_cfg_t zb_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,
        .install_code_policy = false,
        .nwk_cfg.zed_cfg = {
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,
            .keep_alive = 3000,
        },
    };
    esp_zb_init(&zb_cfg);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = ZB_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_COLOR_DIMMABLE_LIGHT_DEVICE_ID,
        .app_device_version = 1,
    };
    esp_zb_ep_list_add_ep(ep_list, build_clusters(), ep_cfg);
    esp_zb_device_register(ep_list);

    esp_zb_core_action_handler_register(action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));

    /* Push persisted settings into the running attributes/render state. */
    render_set_power(s_cfg.power);
    render_set_brightness(s_cfg.brightness);
    render_set_main_color(s_cfg.main_color);
    render_set_hour_color(s_cfg.hour_color);
    render_set_min_color(s_cfg.min_color);
    render_set_mode_24h(s_cfg.mode_24h);
    render_set_leading_zero(s_cfg.leading_zero);
    render_set_colon_mode(s_cfg.colon_mode);
    render_set_effect(s_cfg.effect);

    /* Apply persisted auto-brightness config to the ambient engine. */
    ambient_set_bounds(s_cfg.min_bright, s_cfg.max_bright);
    ambient_set_enabled(s_cfg.auto_bright);
    /* If auto is off, honour the stored manual brightness right away. */
    if (!s_cfg.auto_bright) render_set_brightness(s_cfg.brightness);

    /* Apply persisted environment-display config. */
    render_set_show_env(s_cfg.show_env);
    render_set_env_dwell(s_cfg.time_dwell_s, s_cfg.env_dwell_s);

    xTaskCreate(time_task, "zb_time", 3072, NULL, 4, NULL);
    xTaskCreate(ambient_report_task, "zb_illum", 3072, NULL, 3, NULL);
    xTaskCreate(env_report_task, "zb_env", 3072, NULL, 3, NULL);

    esp_zb_stack_main_loop();
}

void zb_device_start(void) {
    settings_load(&s_cfg);

    esp_zb_platform_config_t platform = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config  = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform));

    xTaskCreate(zb_task, "zigbee", 6144, NULL, 5, NULL);
}
