# Wiring

## GPIO map (defaults in `firmware/main/clock_config.h`)

| Signal | ESP32‑C6 GPIO | Notes |
|--------|---------------|-------|
| LED data (DIN) | **GPIO8** | Via 330–470 Ω series resistor; through level shifter if used |
| LDR ambient sensor | **GPIO3** | ADC1_CH3; LDR voltage divider (see below) |
| DHT11 data | **GPIO10** | Single‑wire; 4.7k–10k pull‑up to 3V3 (see below) |
| Factory reset button | **GPIO9** | BOOT button on most C6 devkits; hold 5 s to leave network |
| Status LED (optional) | disabled (`-1`) | Set to a free GPIO to enable |
| 5 V | 5V / VBUS | Powers LEDs (and board if USB not connected) |
| GND | GND | **Common ground between board, LEDs, and PSU is mandatory** |

Any free GPIO works for the data line; avoid strapping pins (GPIO4/5/8/9/15) if you can,
though GPIO8 is used here for convenience since it's commonly broken out. Change
`LED_DATA_GPIO` if your board reserves it.

## LED data chain order

A **single** data line snakes through the whole display in this order:

```
DIN ─▶ Digit0 (H tens) ─▶ Digit1 (H ones) ─▶ Colon ─▶ Digit2 (M tens) ─▶ Digit3 (M ones)
```

Within each digit, the 7 segments are wired in this order (each segment =
`LEDS_PER_SEGMENT` pixels):

Segments are named in **chain order** (the letters follow the data wire from the
input), matching the CAD drawing:

```
   ddd          a = bottom       (input, 1st in chain)
  c   e         b = lower-left
  c   e         c = upper-left
   fff          d = top
  b   g         e = upper-right
  b   g         f = middle
   aaa          g = lower-right  (last in chain)
```

Chain path per digit: `a → b → c → d → e → f → g` — enter at the bottom bar, up
the left side, across the top, down the right, across the middle, then the
diagonal to the lower-right bar.

Because the letters already are the chain order, the firmware default is the
identity map `SEGMENT_ORDER = {0,1,2,3,4,5,6}` in `clock_render.c`. If you ever
rewire, edit only that array — the font table stays the same.

Use `tools/led_map_preview.html` to visualize exactly which chain index lights each
segment for your configuration.

## Power wiring

```
        5V PSU (+)
           │
     ┌─────┴──────┐
     │  1000µF    │   (across 5V/GND, close to first LED)
     │  ═══╪═══   │
     └─────┬──────┘
           │ 5V rail ───────────────▶ LED +5V (inject at both ends if long)
           │
   ESP32‑C6 5V pin (if powering board from same PSU)

  ESP32‑C6 GPIO8 ─[330Ω]─▶ [74AHCT125] ─▶ LED DIN
  ESP32‑C6 GND ──────────────┬──────────▶ LED GND
                             │
                         5V PSU (−)   (single common ground)
```

### Level shifter (74AHCT125) — recommended

```
   3V3 data (GPIO8) ─▶ 1A     1Y ─▶ [330Ω] ─▶ LED DIN
                       1OE ─ GND (enable)
                       VCC ─ 5V
                       GND ─ GND
```

WS2812 expects a logic‑high around 0.7 × VDD ≈ 3.5 V at 5 V supply; the C6's 3.3 V
output is marginal. Short runs (a few cm to the first pixel) often work directly, but
the AHCT buffer makes it rock‑solid. Alternatively power the LEDs at ~4.3 V (one diode
drop) so 3.3 V logic is comfortably valid.

## Ambient light sensor (LDR) — auto‑brightness

The LDR forms a voltage divider into an ADC1 pin (default **GPIO3**):

```
     3V3 ──[ LDR ]──┬───────────── ADC pin (GPIO3 / ADC1_CH3)
                    │
                  [ 10kΩ ]         (optional 100nF from this node to GND)
                    │
                   GND
```

With this orientation **more light → lower LDR resistance → higher voltage → higher ADC**.
If you wire the LDR and resistor swapped (or the reading is inverted), set
`LDR_INVERT 1` in `clock_config.h`.

**Notes**
- Must be an **ADC1** pin on the ESP32‑C6 (GPIO0–GPIO6). Don't use ADC2 pins — ADC2 is
  unavailable while the radio is active.
- The firmware uses 12 dB attenuation (~0–3.3 V range) and 12‑bit resolution (0–4095).
- **Calibrate:** watch the serial line `ambient raw=… norm=… lux~… auto_level=…`. Note the
  raw value in a dark room and in bright light, then set `LDR_RAW_DARK` / `LDR_RAW_BRIGHT`.
- **Placement matters:** point the LDR at the room, shielded from the display's own LEDs,
  or bright settings can create a feedback loop (LEDs light the sensor → dims → brightens…).
  The EMA smoothing + slew limiting damp this, but physical shielding is best.

## Temperature/humidity sensor (DHT11)

Single‑wire sensor on **GPIO10** with a pull‑up to 3V3:

```
     3V3 ──[ 4.7k–10k ]──┬── DATA ── GPIO10
     3V3 ────────────────┤ VCC
     GND ────────────────┤ GND
```

**Notes**
- Many DHT11 breakout boards already include the pull‑up resistor — if so, wire
  VCC/DATA/GND straight through (no extra resistor needed).
- Power the DHT11 from **3V3** (the C6 is a 3.3 V part; DATA must not exceed 3.3 V).
- Keep the data wire short‑ish (< ~20 cm) for reliable single‑wire timing.
- DHT11 samples at ~1 Hz; the firmware reads it every `DHT_READ_INTERVAL_MS`
  (default 5 s). Mount it away from the LEDs and the PSU so their heat doesn't
  bias the reading.
- For better accuracy, a **DHT22/AM2302** is a drop‑in (same 3 pins) — set
  `DHT_TYPE 22` in `clock_config.h`.

## Grounding & injection tips

- Tie **all grounds together** (board, PSU, LEDs) — the data signal is referenced to GND.
- For the default 58 LEDs a single 5 V feed at the head is fine.
- If you expand to hundreds of LEDs, inject 5 V at both ends to avoid brown‑out/color
  shift on the far digits.
