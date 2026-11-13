# Sump Master 2000 PCB and case: design handoff

Status (2026-11-08): **rev A generated: schematic, placed and routed board, and case.
Not yet measured against the real modules, not ordered, not printed.** See section 8 for
what was built and how it differs from the v3 sketch below. Sections 1-7 are the original brief.
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
| GY-346 accelerometer module (ADXL346, I2C 0x53) | 8-pin header: VIN, GND, SCL, SDA, SDO, CS, INT2, INT1. Lies flat on its own soldered header. Board 14 x 21 mm, header along a 21 mm side (user-measured 2026-11-13). |
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

## 8. Rev A as built (2026-11-08)

Files (all generated, re-run the scripts after editing their tables):
- `tools/gen_carrier.py` -> `sump_carrier.kicad_sch` + `sump_carrier.kicad_pcb` (run with KiCad's Python,
  command at the top of the script). `sump_carrier.kicad_pro` is the fan project's settings (0.2 mm track,
  0.5 mm copper-to-edge). New footprint `Custom_Library:CasePin_Hole_2.5mm` (NPTH for the case pins).
- `case/sump_case.scad` -> `case/tray.stl`, `case/lid.stl` (lid exported flipped, ceiling on the bed).
- Renders: `carrier-top.png`, `case-preview.png`.

Checks: ERC 0 violations; DRC 0 violations, 0 unconnected, 0 schematic-parity issues.
Case (overlap volumes, OpenSCAD intersections): lid x tray empty; the only overlaps with the stand-in
parts are the pins passing through the hole-less dummy boards (six carrier pins, two SR04 locating pins).

**Carrier, 75.5 x 24 mm, 2 layers, GND pour both sides, fully routed:**
| Ref | Part | Notes |
|---|---|---|
| U1 | SuperMini, soldered on 2.54 headers, USB-C overhangs the left edge | GPIO5 SDA, 6 SCL, 3 Trig, 4 Echo, 3V3, GND |
| J1 | 1x8, GY-346 soldered on its own header (2.5 mm up; bench-test it first on Dupont wires) | pin 1 = VIN at the top edge; labels VIN GND SCL SDA SDO CS IT2 IT1 on silk |
| J2 | 1x4, AHT20 soldered on its own header (board 2.5 mm up, lying flat toward the far end) | VDD SDA GND SCL from the top edge down (user-verified) |
| J3 | 4 plain pads (1x4, 2.54 mm) at x 72.8 | VCC TRG ECH GND. Male Dupont wires soldered in; their female ends go on the SR04's own pins. Echo goes straight to GPIO 4 (3.3 V board only, no divider) |
| C1, C2 | 470 uF upright radial, 7 x 12 mm, D7.5 P2.5 footprint | ADXL and AHT20 caps; side by side between the two sockets |
| H1-H6 | 2.5 mm holes for case pins | H5/H6 under the ESP at the USB end (standoffs take the USB plug's force; the lid's end wall closes over the USB connector from above); H1/H2 flank the ADXL and get lid hook posts; H3/H4 under the SR04 end, pins only |

No SMD parts, no jumpers, no milled slot: a basic 2-layer order (user's choice, 2026-11-08). The ADXL's
CS, SDO and INT pins are unconnected (the module's own resistors set I2C mode, as on the bench).

Changes from the v3 sketch, and why:
- **Board is 75.5 mm, not 100.** It stops before the SR04's tall parts (transformer, big electrolytic), so
  nothing on the carrier has to fit under them. The case length is about the same (120 mm outside).
- **No milled slots** (basic PCB order). The AHT20 is about 45 mm from the ESP, with vents beside and above
  it in the case (the floor vents were removed 2026-11-10).
- **Two caps, not four** (user's choice 2026-11-08; the bench only ever had caps at the two I2C modules).
- **SR04 turned 90 deg under the lid** (user's choice 2026-11-08): its length runs across the case and its
  header edge faces the carrier, so its straight right-angle pins point back at the carrier. Female Dupont
  ends plug straight onto them (about 11 mm past the board edge: pins 5 mm out, 12 mm housings) and the wires make a
  C-bend down to J3. The header edge sits 79.7 mm from the USB wall. Cavity
  **110 x 47.4 x 24 mm** (case **114 x 51.4 x 28 mm** outside, flat underneath). The SR04 is
  centred across the case, so its JST slot is on the lid's centre line; the socket pokes ~5 mm above the lid.
- **SR04 held by 4 snap clips** (two on each long edge, 8 mm from the ends, clear of the header), resting
  on 4 stand-off pads (4 mm) with locating pins (2.2 mm) on two diagonal pads through its corner holes. No
  screws. Arms are 0.8 mm thick, rooted 1.4 mm up in blind grooves in the lid (top stays closed, no vent
  holes over them), ~7 mm long with a 0.4 mm hook: ~1% strain, so PLA or PETG. Flex the arms outward to
  remove it.
- **Holes without a hook post are press-fit (2.4 mm pins in 2.5 mm holes):** H3/H4 (a hook post there would
  have to pass through the SR04 board) and H5/H6 (the ESP module is above them). H1 and H2 are clamped by
  lid hook posts.
- **Strap slots through the tray's side walls** (two ties up to 5.5 x 2 mm; slots 6.5 x 2.6 mm at floor
  level, cavity x 36.5 and 65) so the case can be cable-tied to the discharge pipe; the ADXL needs to feel
  the pump. Each tie crosses the floor under the carrier (3 mm gap) where the board has no solder tails
  (board x 29.5-43.5 and 58.5-71.5 are clear), so it threads with the carrier fitted. **Changed 2026-11-10:**
  the earlier rails with tunnels and a pipe channel under the floor are gone, because the tray couldn't
  sit flat on the print bed. The underside is now flat (it rests on the pipe along one line; the two ties
  pull down on both side walls and keep it centred). The floor vent slots under the AHT20 were removed too (2026-11-10): the floor is solid.
- Lid top: a symmetric 5 mm hole grid centred on the lid (`hole_pattern` in the SCAD): **"rows4" (chosen
  2026-11-08)** = two rows of 3 mm holes along each long side plus dimples down the middle (fan-case look;
  holes that would land on an SR04 stand-off pad become dimples); "rows2" = one row each side. The middle strip stays solid for the button tube, the clamping
  posts and the JST slot. A button guide tunnel sits at the fan case's spot (board x 9.75, 9.5 mm from the
  lower long edge; check whether that's BOOT or RESET on your module). "SUMP MASTER 2000" is engraved on
  the tray's long side wall. The tray keeps its side vent slots beside the AHT20 (its floor is solid).
- **Closure: lid skirts inside full-height tray walls** (the fan dongle case's arrangement; printed and
  confirmed by the user 2026-11-11).
  - The tray's walls run the full height (24 mm inside). Its USB end is only a low stub wall, up to the
    centre line of the USB-C opening.
  - The lid is a flat ceiling with two 2 mm skirts that slide down inside the long walls (0.2 mm gap,
    15 mm tall), and an end wall across the full width that drops onto the stub wall and carries the top
    half of the USB-C opening.
  - Three snap tabs per skirt (cavity x 18, 48, 98): the skirt is thinned to 1 mm over 10 mm, with a slit
    either side and a window that clicks over a 0.7 mm ridge on the tray wall (0.5 mm engagement, under 1%
    strain). The ridge's underside is 45 deg, so a firm pull opens the case. Unlike the fan case's
    friction-only tabs, these have a real catch.
  - The SR04 (42 mm across) needs the skirts beside it, so the cavity is 47.4 mm wide; everything inside
    is centred. The AHT20 side vents carry on through the skirts.
  - If the snap is too tight or loose, change `ridge_p` (ridge height) or `skirt_gap`.
  - **Removed 2026-11-11: the original "lip" closure** (tray and lid walls meeting half way up, a 1.2 mm
    lip under the lid's walls snapping into grooves; case 47 mm wide). Its lip was modelled 0.2 mm beside
    the lid wall with no overlap, so it was a separate body and did not print. It is in git history
    before this date if ever wanted.
- **H1 clamp post made solid (2026-11-10).** Its hollow tip had a 0.3 mm wall (3.4 mm post,
  2.8 mm bore), which doesn't print with a 0.4 mm nozzle, so the first printed lid's post came out about
  2.5 mm short and didn't clamp the board beside the ADXL. The post is now solid and the tray pin at H1
  stops 0.3 mm below the carrier's top face (it still locates the board); the post presses on the board
  around the hole. H2's post (4.5 mm, 0.85 mm wall) keeps its hollow tip. Both tray and lid changed.
- **One-solid check (2026-11-10):** posts and snap ridges overlap the part they sit on, so `tray.stl` and
  `lid.stl` are one body each (count the connected bodies after any change), and lid x tray only touch.

**Assembly:** solder the 4 Dupont wires into J3; carrier into the tray (the USB-C drops into the rounded
notch in the end wall, pins through the holes); with the lid upside down, push the SR04 onto the pads
(back face down, JST up through the slot) until the 4 clips snap over it; plug the wires' female ends onto
the SR04's pins; press the lid on (its skirts slide inside the tray's long walls until the tabs click; its end wall completes the USB opening).

**Measured by the user (2026-11-08):** caps 7 mm dia x 12 mm tall (footprint now D7.5 P2.5, assumed
2.5 mm leads); GY-346 20 x 9 mm (superseded 2026-11-13: re-measured **14 x 21 mm**, header along the 21 mm side), orientation confirmed; AHT20 15 x 15 mm, its board 2.5 mm above the
carrier (soldered on its own header, lying flat away from J2 toward the far end; J3 moved to x 72.8 to clear
it); SuperMini USB-C bottom flush with the module PCB top (as modelled); RESET 9 mm from the USB end and
5 mm from the right side with the USB port up (tunnel at board (9.0, 8.0)).

**Silkscreen:** top "SUMP MASTER 2000" and "v1.0"; bottom (mirrored, reads from underneath) the BB logo
(`tools/bb_logo.png` -> `tools/bb_logo.txt`, 0.05 mm pixels, 11.5 x 7.1 mm), the project URL
github.com/bradrblack/sump-master-2000 and "SUMP MASTER 2000  PCB v1.0".

**Still needed before ordering / printing:**
1. AHT20 direction confirmed by the user (lies toward the far end). (Caps: 7 x 12 mm used; the 2.5 mm lead
   pitch is the standard for that size, a 3.5 mm part would need its leads bent in.)
2. AJ-SR04M (measured: 42 x 28.5 mm; JST centred, 12 mm from the long edges and 17.5 mm from the short
   edges, top 9 mm above the board; header centred on a long edge, 10 mm wide, 1 mm in, pins 5 mm past the
   edge). Holes 2.5 mm, edges 1 mm from the long edges and 1.5 mm from the short ones (centres 2.25 / 2.75 mm in;
   2.2 mm locating pins). Nothing but the header is near the long edges (clips clear); the tallest part is a 9 mm
   trimmer pot (the old 16 mm transformer guess is gone).
3. Dupont housings measured at 12 mm: the plug ends sit ~11 mm past the SR04 edge, ~4 mm short of J3, so the
   wires make a C-bend down to it.
4. PLA or PETG both OK: the clips are 0.8 mm arms rooted 1.4 mm up in blind grooves in the lid (top stays
   closed), on 4 mm stand-off pads, ~7 mm long with a 0.4 mm hook (~1% strain). The case straps to the pump's
   1-1/2" ABS outlet (48.3 mm OD) with two zip ties (up to 5.5 x 2 mm) through the side-wall slots and
   under the carrier; the tray's underside is flat.
5. Then print a test tray to check the pin fits (2.2 mm clearance, 2.4 mm press) and the USB opening height.
