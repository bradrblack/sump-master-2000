# Sump Master 2000 PCB and case: design handoff

Status: **concept agreed, nothing laid out yet.** No KiCad schematic/board has been
generated for this project. This file is the full brief for whoever does it.
The current sketch is `layout-sketch-v3.png` (to scale; script in `ref/make_sketch_v3.py`).
`layout-sketch-v1-/v2-superseded.png` are earlier rejected ideas, kept only for history.

## 1. Goal
A carrier PCB plus 3D-printed case for the sump monitor (firmware in `../src/`,
PlatformIO env `sump`), replacing the bench wiring. Start from the author's other
project: https://github.com/bradrblack/rf-ceiling-fan-remote/tree/main/hw
(KiCad 10 project `esp32c3_cc1101_dongle`, SuperMini footprint, OpenSCAD case
`hw/case/fan_dongle_case.scad`). Reuse its style: USB-C overhanging the board edge,
no screws holding the PCB, alignment pins through mounting holes, hook posts from the
lid, snap-fit latch tabs on the long walls. Copied here: `Custom_Library.pretty/`
(SuperMini and CC1101 footprints), `Custom_Library.kicad_sym`, `fp-lib-table`, `sym-lib-table`.
Replace the CC1101 with the sensor sockets below.

## 2. Parts
| Part | Notes |
|---|---|
| ESP32-C3 SuperMini, "v2" with external antenna | 16 through-hole pads, 2.54 mm pitch, rows 15.24 mm apart (footprint `ESP32-C3_SuperMini_FaceUp`). USB-C on the board edge. Antenna stays inside the case. 3.3 V rail comes from this board. |
| GY-346 accelerometer module (ADXL346, I2C 0x53) | 8-pin header: VIN, GND, SCL, SDA, SDO, CS, INT2, INT1. Lies flat in a female header. Size and pin-row length still to be measured. |
| AHT20 + BMP280 module (AHT20 at 0x38; BMP280 unused) | 4-pin I2C header, presumably VIN, GND, SCL, SDA (verify). Lies flat in a female header. |
| AJ-SR04M ultrasonic control board (JSN-SR04T-style) | See section 4. Mounted to the lid, not the PCB. |
| 470 uF electrolytic caps | One by each module header, one at the ESP32, one at the SR04 lead header (4 total minimum). **They were the likely fix for I2C failures on the bench.** |

## 3. Electrical (must match the firmware, `../src/sm2k.ino`)
| Net | ESP32-C3 GPIO | Notes |
|---|---|---|
| SDA | 5 | to ADXL346 and AHT20 headers (the modules have their own pull-ups) |
| SCL | 6 | same. Firmware runs I2C at 100 kHz. |
| JSN Trig | 3 | to the SR04 lead header |
| JSN Echo | 4 | straight to the pin at 3.3 V, no divider. Firmware enables an internal pull-down. Add an unpopulated 2k/1k divider footprint if cheap. |
| NeoPixel | 8 | the SuperMini's onboard LED, nothing to route |
| 3V3, GND | | JSN-SR04T board is powered from 3V3 (newer boards accept 3.3 V) |
Keep GPIO 8/9 clear of other loads (boot strapping). Everything is 3.3 V logic.
Optional: pads to tie the ADXL's CS to 3V3 and SDO to GND; INT1/INT2 to an unpopulated header.
SR04 lead header (1x4): VCC (3V3), Trig, Echo, GND.
Bench result: with direct short wires, 470 uF caps per sensor, and 100 kHz I2C all three sensors
ran stably for 14 h and 5/5 cold power-ups. Long jumpers and no caps were unreliable.

## 4. AJ-SR04M board (photos in `ref/`: `sr04-top.png`, `sr04-bottom.png`)
Verified by the user: board **41.5 x 29 mm**; the white **JST-XH 2-pin probe socket is on the BOTTOM face,
12 mm from each long edge** (centred across the 29 mm width), and the **probe plug goes in straight, perpendicular
to the board (top-entry)**. (Earlier I thought it was side-entry; the user says straight.)
From the photos: a **4-pin right-angle I/O header on one long edge** (pins stick out about 11 mm in the board plane;
order 5V, Trig, Echo, GND), four corner mounting holes (about 2.3 mm), a large silver inductor/tuning can and an
electrolytic cap on the top face (tall parts about 14-16 mm). Keep walls 1.5 mm clear of the can.
**Unverified** (from a web summary, do not trust without calipers): hole centres (2.0, 2.0)/(39.3, 26.5) from a corner,
I/O header pin 1 about 2.5 mm from the edge, JST position along the 41.5 mm length. Measure the JST's position along
the length and the exact hole positions before cutting the lid.

## 5. Agreed layout (v3)
Long and slim, **about 24 x 100 mm PCB, about 33 x 108 mm case, about 22 mm tall inside**.
Left to right: ESP32 (USB-C end), ADXL346, AHT20, then the SR04 at the far end.
- The ADXL346 and AHT20 lie flat (parallel to the PCB) in female headers whose pin rows run across the board
  width, like the CC1101 on the fan board. They rely on the sockets; no separate mounting holes on the modules.
- The SR04 board is fixed under the lid at the far end, bottom face against the lid, parts facing down (tall parts hang
  over the empty far end of the PCB). Its JST socket passes through a rectangular slot in the lid; the probe cable
  plugs in from outside. A short 4-wire lead joins its I/O header to the 1x4 header on the carrier.
- AHT20 as far from the ESP32 as practical (about 45 mm), a milled slot between ESP32 and ADXL346 and another near
  the SR04 zone, case vents beside the AHT20, no warm parts upwind. (Option: order ESP32, AHT20, ADXL346 if it reads
  warm.)
- The ADXL346 should couple to the case, so give it a hook post/pin through the carrier hole next to it.
- Rejected: SR04 flat on the carrier; modules standing on edge; SR04 outside the lid.
- Shorter alternative if 108 mm is ever too long: overlap the SR04 over the AHT20 (about 90 mm, about 30 mm tall).

## 6. Tools on the author's Mac
KiCad 10.0.5: `/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli` (ERC, DRC, SVG/PNG/STEP export) and the
`pcbnew` Python module (use KiCad's bundled Python). OpenSCAD: `/opt/homebrew/bin/openscad`. Footprint libraries:
`/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints`. matplotlib is not installed.

## 7. Suggested next steps
1. Measure with calipers: GY-346 and AHT20 board outlines and pin pitch/row length, SR04 JST position along its length,
   SR04 hole positions, probe plug body and cable diameter.
2. Generate the schematic and board (Python with pcbnew, or hand-written s-expressions based on the fan project's files);
   run ERC/DRC with kicad-cli; render for review; the user finishes routing in KiCad.
3. Adapt the OpenSCAD case from `fan_dongle_case.scad` (lid with SR04 standoffs/pins and a JST slot, end-wall gland if
   needed, vents, latch tabs, USB-C opening).
4. Update `../README.md` and `../PLAN.md` for the new wiring (they still describe a 5 V JSN with an Echo divider and
   breadboard wiring).
