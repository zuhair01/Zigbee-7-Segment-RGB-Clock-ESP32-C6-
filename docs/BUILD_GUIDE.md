# Build & Flash Guide

## 0. Prerequisites

- **ESP‑IDF v5.3 or newer** installed ([official guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/get-started/)).
  The Zigbee (`esp-zigbee-lib`, `esp-zboss-lib`) and `led_strip` components are pulled
  automatically from the ESP Component Registry on first build via `main/idf_component.yml`.
- A **Zigbee coordinator** already running: Home Assistant with **ZHA** or **Zigbee2MQTT**
  (SkyConnect / Sonoff Zigbee 3.0 Dongle / ConBee II, etc.).
- USB cable and the ESP32‑C6 board.

## 1. Get the ESP‑IDF environment

```bash
# once per shell
. $HOME/esp/esp-idf/export.sh        # adjust to your IDF path
cd firmware
idf.py set-target esp32c6            # already the default, but explicit is good
```

## 2. Configure your build (optional)

Edit `firmware/main/clock_config.h` for your hardware:

- `LED_DATA_GPIO` — the pin driving the LEDs (default 8)
- `LEDS_PER_SEGMENT` — pixels per segment (default 2)
- `COLON_LEDS` — colon dot pixels (default 2)
- `SEGMENT_ORDER` in `clock_render.c` — if you wired segments in a custom order
- **Ambient light sensor**: `LDR_ADC_GPIO` / `LDR_ADC_CHANNEL` (default GPIO3 / ADC1_CH3),
  `LDR_INVERT` if your divider is wired the other way, and the calibration endpoints
  `LDR_RAW_DARK` / `LDR_RAW_BRIGHT`. Tune the curve with `AMBIENT_CURVE_GAMMA` and the
  responsiveness with `AMBIENT_EMA_ALPHA` / `AMBIENT_SLEW_PER_STEP`.
- **DHT11 sensor**: `DHT_GPIO` (default 10), `DHT_TYPE` (11, or 22 for DHT22/AM2302),
  and `DHT_READ_INTERVAL_MS`. Display cycling: `DEFAULT_SHOW_ENV`, `DEFAULT_TIME_DWELL_S`,
  `DEFAULT_ENV_DWELL_S`.

Run `idf.py menuconfig` only if you need to change stack options; the provided
`sdkconfig.defaults` already sets target, Zigbee end‑device mode, partition table, etc.

## 3. Build

```bash
idf.py build
```

First build downloads the managed components and compiles the Zigbee stack — it takes
a few minutes. Subsequent builds are fast.

## 4. Flash & monitor

```bash
idf.py -p /dev/ttyACM0 flash monitor   # Linux; use the right port for your OS
# Windows: -p COM5   |   macOS: -p /dev/cu.usbmodemXXXX
```

Exit the monitor with `Ctrl‑]`.

On first boot the display shows a gently pulsing **`--:--`** — this means "powered and
rendering, but not yet time‑synced." Watch for the `ambient raw=… auto_level=…` log line
to confirm the light sensor is being read (and to calibrate it, see step 8).

## 5. Join the Zigbee network

1. Put your coordinator into **permit join / pairing** mode:
   - **ZHA:** Settings → Devices & Services → ZHA → *Add device*.
   - **Zigbee2MQTT:** enable *Permit join (all)* in the frontend.
2. The clock searches for and joins automatically on boot (network steering). Watch the
   serial log for `joined! PAN 0x… ch …`.
3. It appears as a **Color Dimmable Light** named after model `SegClock-C6`.

If it doesn't join within ~40 s, it retries. To force a fresh join, **factory reset**:
hold the BOOT button (GPIO9) for 5 seconds — the log prints `factory reset: leaving
network` and the device reboots to search again.

## 6. Get the time showing

The clock reads the coordinator's **Time cluster** right after joining and every 10
minutes after. Make sure the coordinator serves time:

- **Zigbee2MQTT:** time server is on by default (the coordinator answers Time reads).
- **ZHA:** ZHA provides a Time server automatically.

Within a few seconds of joining, `--:--` is replaced by the current local time. See
`docs/HOME_ASSISTANT.md` for the timezone attribute and the custom controls.

## 7. Add the custom controls (optional but recommended)

The on/off, brightness, and RGB color work out of the box. For 12/24h, colon mode,
effects, and per‑half colors:

- **Zigbee2MQTT:** copy `tools/zigbee2mqtt/seg_clock.js` into your `external_converters/`
  folder and restart Z2M. See `docs/HOME_ASSISTANT.md`.
- **ZHA:** copy `tools/zha/seg_clock_quirk.py` into your `custom_quirks_path` and restart HA.

## 8. Calibrate auto‑brightness

Auto‑brightness is **on by default**. To tune it to your room:

1. In the serial monitor, note the `ambient raw=…` value:
   - cover the LDR / darken the room → record the low number → set `LDR_RAW_DARK`.
   - shine light on it / daylight → record the high number → set `LDR_RAW_BRIGHT`.
2. Set the comfortable **min/max brightness** either in firmware (`min_bright`/`max_bright`
   defaults) or live from Home Assistant (`min_brightness` / `max_brightness`).
3. Rebuild/flash if you changed firmware constants.

You can turn auto off from HA (`auto_brightness` switch) to control the level manually.

## Troubleshooting

| Symptom | Likely cause / fix |
|---------|--------------------|
| Build fails downloading components | Check internet; `idf.py fullclean` then rebuild. Ensure IDF ≥ 5.3. |
| `--:--` never fills in | Coordinator isn't serving Time cluster, or device on a different network. Check log for time sync line. |
| Brightness stuck at floor/ceiling | LDR not calibrated — set `LDR_RAW_DARK`/`LDR_RAW_BRIGHT` from the `ambient raw=…` log. |
| Brightness inverted (bright in dark) | LDR divider wired the other way — set `LDR_INVERT 1` or swap LDR/resistor. |
| Display "hunts"/oscillates | LDR sees its own LEDs — shield/reposition it; lower `AMBIENT_EMA_ALPHA` or `AMBIENT_SLEW_PER_STEP`. |
| HA slider snaps back when moved | Expected while auto‑brightness is ON; turn `auto_brightness` off for manual control. |
| No illuminance sensor in HA | Install the converter/quirk from `tools/` (see HOME_ASSISTANT.md) and re‑interview. |
| Temp/humidity never show / `checksum fail` in log | DHT11 wiring or missing pull‑up; check 3V3 power, DATA on GPIO10, and a 4.7k–10k pull‑up. |
| Temp/humidity screens never appear on LEDs | `show_environment` is off, or no valid DHT reading yet (needs a few seconds after boot). |
| Wrong temperature unit wanted | Display is °C; HA gets °C — add a °F template sensor in HA if desired. |
| No LEDs at all | Check 5 V, common ground, data GPIO, and the series resistor. Confirm `TOTAL_LEDS` matches reality. |
| Flickery / wrong first pixels | Add the 74AHCT125 level shifter or power LEDs at ~4.3 V; add the 330 Ω series resistor and 1000 µF cap. |
| Colors swapped (R/G/B) | Change `color_component_format` in `clock_render.c` (WS2812=GRB, some SK6812=RGB). |
| Wrong segments light | Fix `SEGMENT_ORDER[]` to match your wiring; verify with `tools/led_map_preview.html`. |
| Won't re‑pair after moving networks | Factory reset: hold BOOT (GPIO9) 5 s. |

## Building for ESP32‑H2 instead

The H2 has 802.15.4 but no Wi‑Fi; the firmware is otherwise identical:

```bash
idf.py set-target esp32h2
idf.py build flash monitor
```
