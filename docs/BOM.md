# Bill of Materials

Quantities assume the default layout: **4 digits × 7 segments × 2 LEDs + 2 colon LEDs = 58 addressable LEDs**.
Scale LED counts if you change `LEDS_PER_SEGMENT` in `firmware/main/clock_config.h`.

## Core electronics

| # | Part | Qty | Notes |
|---|------|-----|-------|
| 1 | **ESP32‑C6** dev board | 1 | Must be genuine C6 (802.15.4 radio). ESP32‑C6‑DevKitC‑1, Seeed XIAO ESP32‑C6, or a bare ESP32‑C6‑WROOM‑1 module. **Not** C3/S3 — those have no Zigbee radio. |
| 2 | **WS2812B** or **SK6812** addressable LEDs | 58 | Individually addressable 5 V RGB. SK6812 has better color mixing; RGBW needs a firmware tweak. Use LED strip, PCB, or discrete pixels. |
| 3 | 5 V power supply | 1 | See power budget below. 5 V / 2 A (10 W) is comfortable for the default build. |
| 4 | 74AHCT125 or 74AHCT1G125 level shifter | 1 | Shifts C6's 3.3 V data to 5 V for reliable WS2812 timing. Optional for short runs but recommended. |
| 5 | Electrolytic capacitor 1000 µF / 10 V | 1 | Across LED 5 V/GND to absorb inrush. |
| 6 | Resistor 330–470 Ω | 1 | In series with the LED data line (near the first pixel). |
| 7 | Resistor 10 kΩ | 1 | Pull‑up if you add an external reset button (BOOT works otherwise). |

## Ambient light sensing (auto‑brightness)

| # | Part | Qty | Notes |
|---|------|-----|-------|
| A1 | **LDR / photoresistor** (e.g. GL5528) | 1 | Cheap analog light sensor. GL5528 ≈ 8–20 kΩ in light, ~1 MΩ in dark. |
| A2 | Resistor **10 kΩ** (divider) | 1 | Fixed leg of the LDR voltage divider into the ADC pin. See `docs/WIRING.md`. |
| A3 | Capacitor 100 nF (optional) | 1 | Across the ADC node to GND to steady the reading. |

> The LDR is wired as a divider into an **ADC1** pin (default GPIO3). It gives a
> *relative* light level, not calibrated lux — perfect for auto‑dimming. Tune
> `LDR_RAW_DARK` / `LDR_RAW_BRIGHT` in `clock_config.h` to your room (watch the
> `ambient raw=…` serial log). Mount the LDR facing the room, away from the LEDs'
> own glow to avoid a feedback loop.

## Temperature & humidity sensing

| # | Part | Qty | Notes |
|---|------|-----|-------|
| T1 | **DHT11** temperature/humidity sensor | 1 | 3‑ or 4‑pin module. Range 0–50 °C (±2 °C), 20–90 %RH (±5 %), ~1 Hz. |
| T2 | Resistor **4.7 kΩ–10 kΩ** | 1 | Pull‑up on the DATA line to 3V3 (many DHT11 breakout boards include this). |

> The DHT11 uses a single data GPIO (default **GPIO10**) with a pull‑up. The
> firmware also supports the more accurate **DHT22/AM2302** (0.1° resolution) —
> same wiring, just set `DHT_TYPE 22` in `clock_config.h`. Temperature is shown
> in °C on the display and reported to Home Assistant via the standard clusters.

## Power & connectors

| # | Part | Qty | Notes |
|---|------|-----|-------|
| 8 | USB‑C breakout **or** 5 V barrel jack | 1 | Powering the strip and board. |
| 9 | Dupont / JST‑SM 3‑pin connectors | as needed | For 5 V / DIN / GND between board and LED array. |
| 10 | Hookup wire 22 AWG | — | 20 AWG for the main 5 V/GND rails if driving many LEDs. |

## Enclosure / optics

| # | Part | Qty | Notes |
|---|------|-----|-------|
| 11 | 3D‑printed segment mask + diffuser | 1 set | See `docs/ENCLOSURE.md`. Black PLA/PETG mask + white/natural diffuser. |
| 12 | White diffusion acrylic or paper | 1 | 2–3 mm milky acrylic gives the cleanest segment glow. |
| 13 | M2/M3 screws + standoffs | few | Mount the board behind the display. |

## Power budget

Each WS2812B pixel draws up to **~60 mA** at full‑white, full‑brightness.

| Scenario | Current | Supply |
|----------|---------|--------|
| 58 LEDs, worst case (all white, 100%) | 58 × 60 mA ≈ **3.5 A** | 5 V / 4 A if you ever go full white |
| Typical clock (single color, ~60% brightness) | ≈ **0.8–1.2 A** | 5 V / 2 A is plenty |

The firmware brightness (Level Control) scales current linearly, so a 5 V / 2 A supply
is fine for normal use; size up to 4 A only if you want guaranteed full‑white headroom.

## Chip note

The **ESP32‑C6** is required specifically because it integrates an IEEE 802.15.4 radio
(the physical layer Zigbee runs on) alongside Wi‑Fi 6 and BLE. The ESP32‑H2 is an
alternative (802.15.4 only, no Wi‑Fi) and the firmware builds for it with a one‑line
target change.
