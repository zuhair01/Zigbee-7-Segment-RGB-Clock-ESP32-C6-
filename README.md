# Zigbee 7‑Segment RGB Clock (ESP32‑C6)

A customizable, **Zigbee‑connected 7‑segment style clock** built on the **ESP32‑C6**, using
**addressable RGB LEDs** (WS2812B / SK6812) to form four digits (**HH:MM**) plus a colon.

It joins any Zigbee 3.0 network and appears in **Zigbee2MQTT** and **Home Assistant (ZHA)** as a
controllable light with extra clock‑specific controls (12/24h, brightness, per‑display color,
effects, colon blink). Time is synchronized over the Zigbee **Time cluster** from the
coordinator — no Wi‑Fi, no NTP, no RTC battery required.

```
   ┌──────┐   ┌──────┐        ┌──────┐   ┌──────┐
   │  ──  │   │  ──  │        │  ──  │   │  ──  │
   │ |  | │   │ |  | │   ●    │ |  | │   │ |  | │
   │  ──  │   │  ──  │   ●    │  ──  │   │  ──  │
   │ |  | │   │ |  | │        │ |  | │   │ |  | │
   │  ──  │   │  ──  │        │  ──  │   │  ──  │
   └──────┘   └──────┘        └──────┘   └──────┘
     H tens     H ones  colon   M tens     M ones
```

## What's in this package

| Path | Description |
|------|-------------|
| `firmware/` | Complete **ESP‑IDF** project (ESP32‑C6, esp‑zigbee‑sdk + led_strip) |
| `docs/BOM.md` | Bill of materials with part choices and quantities |
| `docs/WIRING.md` | Wiring diagram, GPIO map, power budget, LED data chain |
| `docs/BUILD_GUIDE.md` | Step‑by‑step: build, flash, join network, troubleshoot |
| `docs/ENCLOSURE.md` | 3D‑print / diffuser layout, segment geometry, LED mapping |
| `docs/HOME_ASSISTANT.md` | ZHA quirk + Zigbee2MQTT converter usage |
| `tools/zigbee2mqtt/seg_clock.js` | Zigbee2MQTT external converter |
| `tools/zha/seg_clock_quirk.py` | Home Assistant ZHA custom quirk |
| `tools/led_map_preview.html` | Interactive previewer: LED index / segment layout + time, temperature & humidity screens |

## Feature summary

- **4 digits (HH:MM)** in 7‑segment style, configurable LEDs‑per‑segment.
- **Fully addressable RGB** — set a global color, or a distinct color for hours vs minutes.
- **Zigbee 3.0 end device** — pairs with any coordinator (SkyConnect, Sonoff, ConBee, etc.).
- **Standard clusters** so it "just works": On/Off, Level (brightness), Color Control (XY/HSV),
  Illuminance, Temperature and Humidity Measurement (sensors show up automatically).
- **Custom cluster `0xFC00`** for clock behavior: 12/24h, leading‑zero blanking, colon mode,
  effect selection, per‑half color, auto‑brightness enable + min/max bounds, and the
  temp/humidity display‑cycle toggle + dwell times.
- **Time sync over Zigbee Time cluster (0x000A)** — HA/Z2M push the time automatically.
- **Automatic brightness** — an ambient light sensor (LDR) dims the display at night and
  brightens it in daylight, with a smoothed anti‑flicker curve and configurable min/max.
  The light level is also exposed as a standard **Illuminance sensor** in HA.
- **Temperature & humidity (DHT11)** — the display cycles time → temperature (°C) →
  humidity, and both readings are exposed as standard **Temperature** and **Humidity**
  sensors in HA. The on‑display cycling is toggleable and its timing is configurable.
- **Effects**: solid, breathing, rainbow sweep, per‑digit gradient.
- **Persistent settings** in NVS — survives reboots and re‑pairing.

See `docs/BUILD_GUIDE.md` to get going. A quick design overview is below.

## Quick architecture

```
              Zigbee network (802.15.4)
        ┌──────────────────────────────────┐
        │  Coordinator (SkyConnect/Sonoff)  │
        │  + Zigbee2MQTT / ZHA + Home Asst. │
        └───────────────┬──────────────────┘
                        │ ZCL
        ┌───────────────▼──────────────────┐
        │            ESP32‑C6               │
        │  ┌────────────┐   ┌────────────┐  │
        │  │ Zigbee task│   │ Render task│  │
        │  │ clusters   │──▶│ 50 FPS     │  │
        │  │ + Time     │   │ effects    │  │
        │  └────────────┘   └─────┬──────┘  │
        │  ┌────────────┐  auto   │ RMT     │
        │  │ Ambient    │─brightness        │
        │  │ task (LDR) │◀── ADC1 (GPIO3)   │
        │  ├────────────┤  temp/  │         │
        │  │ DHT task   │─humidity          │
        │  │ (DHT11)    │◀── 1-wire (GPIO10)│
        │  └────────────┘         │         │
        │        NVS settings     │         │
        └─────────────────────────┼─────────┘
                                  ▼
                     WS2812B chain (28 seg + colon)
```
