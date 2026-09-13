# Enclosure, Segment Geometry & Diffusion

This describes the physical display so you can 3D‑print or laser‑cut a clean
7‑segment look with even light diffusion.

## Concept

Three stacked layers:

```
   ┌─────────────────────────────┐  front
   │   Diffuser (white acrylic)  │  2–3 mm milky acrylic
   ├─────────────────────────────┤
   │  Segment mask (black)       │  opaque walls between segments
   │  ┌──┐ light wells per seg   │  prevents bleed digit↔digit
   ├─────────────────────────────┤
   │  LED carrier / PCB / strip  │  WS2812 pixels seated in wells
   └─────────────────────────────┘  back plate holds ESP32‑C6
```

The **segment mask** is the key part: each of the 7 segments per digit is a walled
cavity ("light well") so adjacent segments and digits don't bleed into each other. The
white diffuser on top spreads each segment's LEDs into an even bar.

## Standard 7‑segment proportions

Use a classic ratio so digits read cleanly. For a segment **length `L`**:

| Feature | Value |
|---------|-------|
| Segment length (bar) | `L` |
| Segment thickness | `L / 5` to `L / 6` |
| Digit width | ≈ `L + 2 × thickness` |
| Digit height | ≈ `2L + 3 × thickness` |
| Slant (optional) | 5–8° for a classic clock look |
| Well depth | 10–15 mm (more depth = smoother diffusion) |

Example for `L = 40 mm`: segment ~7 mm thick, digit ~54 mm wide × ~101 mm tall.
Four digits + colon ≈ **250 mm** wide overall with gaps.

## LED placement per segment

With `LEDS_PER_SEGMENT = 2` (default), space the two pixels evenly along each bar:

```
  segment bar (length L):
  ┌───────────────────────┐
  │   ●            ●       │   pixels at ~25% and ~75% of L
  └───────────────────────┘
```

For longer segments use 3–4 LEDs per segment (set `LEDS_PER_SEGMENT`) for uniform
brightness. More pixels + deeper wells + thicker diffuser = smoother bars.

## Colon

Two round wells between digit 1 and digit 2, vertically centered, one pixel each
(`COLON_LEDS = 2`). Diameter ≈ segment thickness.

## Chain routing between segments/digits

Route the data line so the physical order matches the firmware chain:

```
a→b→c→d→e→f→g  (within a digit; a=bottom/input … g=lower-right/last)
digits left→right, colon after digit 1
```

Leave small pass‑through notches in the mask walls for the DIN/5V/GND jumpers between
segments. Keep 5 V and GND as a continuous rail; only DIN daisy‑chains pixel‑to‑pixel.

## Printing tips

- **Mask:** black PLA/PETG, 0 % light transmission. Print walls ≥ 1.2 mm.
- **Diffuser:** either 2–3 mm milky acrylic sheet cut to the digit windows, or print a
  0.8–1.2 mm layer in **white/natural** PLA at low speed — thin white PLA diffuses well.
- **Back plate:** pocket for the ESP32‑C6, cable strain relief, and a hole aligned with
  the BOOT button for factory reset.
- Increase well depth before adding more LEDs if you see hotspots.

## Optional: bill of print parts

| Part | Material | Notes |
|------|----------|-------|
| Segment mask | Black PLA/PETG | The light‑well grid |
| Diffuser | White acrylic or thin white PLA | Even glow |
| Back plate | Any | Holds board + connectors |
| Feet / stand | Any | Slight rear tilt improves viewing |

> This package doesn't ship STLs, but the proportions above drop directly into
> Fusion/FreeCAD/OpenSCAD. If you'd like, I can generate a parametric OpenSCAD file
> that produces the mask + diffuser for any `L`, digit count, and LEDs‑per‑segment.
