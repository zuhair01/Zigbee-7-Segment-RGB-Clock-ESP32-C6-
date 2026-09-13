# Home Assistant Integration (ZHA & Zigbee2MQTT)

The clock uses **standard Zigbee clusters** for the light itself, so basic control works
with no configuration on both ZHA and Zigbee2MQTT. The **custom clock controls** (12/24h,
colon mode, effects, per‑half color) need a small converter/quirk, included in `tools/`.

## What works with zero config

Once joined, the device shows up as a **Color Dimmable Light** plus **Illuminance**,
**Temperature** and **Humidity** sensors:

- **On/Off** → turns the display on/off
- **Brightness** → overall LED brightness (auto‑driven when auto‑brightness is on)
- **Color (RGB / XY / Hue‑Sat)** → the main digit color
- **Illuminance** → the ambient light reading from the LDR (relative lux estimate)
- **Temperature** → DHT11 temperature (°C, standard cluster)
- **Humidity** → DHT11 relative humidity (%RH, standard cluster)

## Automatic brightness

By default the clock **automatically sets its brightness from ambient light** (the LDR).
As the room gets darker the display dims to the configured floor; in daylight it rises to
the ceiling. The mapping is smoothed (EMA + slew limiting) so it never flickers or steps
visibly, and shaped by a gamma curve so it brightens promptly yet settles low at night.

While auto‑brightness is active, the HA **brightness slider reflects** the value the clock
chose (it's reported back every couple of seconds). Moving the slider manually is
remembered but only takes effect once you turn auto‑brightness **off**.

Controls (via the converter/quirk):

| Entity | Type | Meaning |
|--------|------|---------|
| `auto_brightness` | switch | On = ambient controls brightness; Off = manual slider |
| `min_brightness` | number | Floor level at night (0–254) |
| `max_brightness` | number | Ceiling level in daylight (0–254) |
| `illuminance` | sensor | Ambient light (relative lux) |

> The LDR is not a calibrated lux meter, so the illuminance value is an estimate for
> trends/automations, not a photometric reading. Tune `LDR_RAW_DARK`/`LDR_RAW_BRIGHT`
> in firmware to match your room (see `docs/WIRING.md`).

## Temperature & humidity on the display

By default the display **cycles**: it shows the clock for `time_dwell` seconds
(default 30), then temperature (`23°C` style) for `env_dwell` seconds (default 4),
then humidity (`48H` style), then repeats. Both readings are always sent to Home
Assistant regardless of what's on the LEDs.

Controls (via the converter/quirk):

| Entity | Type | Meaning |
|--------|------|---------|
| `show_environment` | switch | Turn the temp/humidity screens on the display on/off |
| `time_dwell` | number (s) | How long the clock is shown each cycle |
| `env_dwell` | number (s) | How long each temp/humidity screen is shown |
| `temperature` | sensor | DHT11 temperature (°C) |
| `humidity` | sensor | DHT11 relative humidity (%RH) |

> Turning `show_environment` **off** makes the display a pure clock while the
> sensors keep reporting to HA. On‑display temperature is Celsius; Home Assistant
> always receives °C via the standard Temperature cluster (convert to °F with a
> template sensor if you prefer).

## Time synchronization

The clock is a Time‑cluster **client**: it reads time from the coordinator.

- **ZHA** runs a Time server automatically — nothing to do.
- **Zigbee2MQTT** answers Time reads by default.

The firmware reads `Time` (0x0000) and `TimeZone` (0x0002) from the coordinator on join
and every 10 minutes, and free‑runs a 1 Hz software clock in between. Time is displayed
as **local wall‑clock** using the coordinator's timezone offset.

> If your coordinator doesn't populate `TimeZone`, the display shows UTC. In Zigbee2MQTT
> you can confirm/set the time attributes via the device's *Dev console → genTime*. In
> ZHA the Time server follows the HA server timezone.

---

## Zigbee2MQTT: install the external converter

1. **Stop** Zigbee2MQTT.
2. Locate the folder that contains `database.db` (e.g. `/opt/zigbee2mqtt/data/`, or the
   add‑on's `zigbee2mqtt/` share). Create an `external_converters/` folder there if it
   doesn't exist.
   > On recent Z2M the path is `<data>/external_converters/`. It must sit next to
   > `database.db`. Remove any old `external_converters:` lines from `configuration.yaml`.
3. Copy **`tools/zigbee2mqtt/seg_clock.js`** into that folder.
4. **Start** Zigbee2MQTT. Open the device page; it should now be "Supported" and expose:

| Entity | Type | Meaning |
|--------|------|---------|
| State / brightness / color | light | standard |
| `mode_24h` | switch | 24‑hour vs 12‑hour |
| `leading_zero` | switch | show leading hour zero |
| `colon_mode` | select | solid / blink / off |
| `effect` | select | solid / breathe / rainbow / gradient |
| `hour_color` | number | 0xRRGGBB integer (0 = use main color) |
| `minute_color` | number | 0xRRGGBB integer (0 = use main color) |
| `auto_brightness` | switch | ambient light controls brightness |
| `min_brightness` | number | auto floor (night level, 0–254) |
| `max_brightness` | number | auto ceiling (daytime level, 0–254) |
| `show_environment` | switch | cycle temp/humidity screens on the display |
| `time_dwell` | number | seconds the clock is shown each cycle |
| `env_dwell` | number | seconds each temp/humidity screen is shown |
| `illuminance` | sensor | ambient light reading (relative lux) |
| `temperature` | sensor | DHT11 temperature (°C) |
| `humidity` | sensor | DHT11 relative humidity (%RH) |

> The per‑half colors are exposed as 24‑bit integers. E.g. pure red = `16711680`
> (`0xFF0000`), green = `65280` (`0x00FF00`), blue = `255` (`0x0000FF`). Set to `0`
> to fall back to the main light color.

---

## ZHA: install the custom quirk

1. Create a quirks folder, e.g. `/config/zha_quirks/`.
2. Copy **`tools/zha/seg_clock_quirk.py`** into it.
3. Add to `configuration.yaml`:
   ```yaml
   zha:
     custom_quirks_path: /config/zha_quirks/
   ```
4. Restart Home Assistant, then re‑interview the device (or remove/re‑add it) so ZHA
   applies the quirk. The custom cluster attributes become entities you can control
   from the device page and use in automations.

---

## Example automations

**Dim at night, bright by day** (Z2M entities):

```yaml
alias: Clock brightness by time of day
trigger:
  - platform: sun
    event: sunset
  - platform: sun
    event: sunrise
action:
  - service: light.turn_on
    target: { entity_id: light.segclock_c6 }
    data:
      brightness_pct: "{{ 20 if trigger.event == 'sunset' else 90 }}"
```

**Rainbow effect on the weekend:**

```yaml
alias: Weekend rainbow clock
trigger:
  - platform: time
    at: "00:00:00"
action:
  - service: select.select_option
    target: { entity_id: select.segclock_c6_effect }
    data:
      option: "{{ 'rainbow' if now().weekday() >= 5 else 'solid' }}"
```

**Different colors for hours and minutes** (Z2M, via MQTT set):

```yaml
service: mqtt.publish
data:
  topic: zigbee2mqtt/SegClock-C6/set
  payload: '{"hour_color": 16711680, "minute_color": 255}'   # red hours, blue minutes
```
