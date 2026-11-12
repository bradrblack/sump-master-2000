// Sump Master 2000 case: tray + lid for the carrier in ../sump_carrier.kicad_pcb
//
// Style follows the fan dongle case (github.com/bradrblack/rf-ceiling-fan-remote,
// hw/case/fan_dongle_case.scad): no screws through the carrier, printed pins
// through its holes, hook posts from the lid that clamp it, detent clicks, USB-C
// held in the end wall. Differences: the AJ-SR04M ultrasonic board is screwed
// under the lid at the far end with its probe socket through a slot, and the
// tray has strap slots low in its side walls so the case can be cable-tied to
// the pump's discharge pipe (the accelerometer needs to feel the pump). The
// ties cross the floor under the carrier; the tray's underside is flat, so it
// prints straight on the bed.
//
// Closure (as on the fan dongle case): the tray's walls run the full height and
// the lid is a flat ceiling with two 2 mm skirts that slide down inside the long
// walls, plus an end wall that drops onto the tray's low stub wall at the USB
// end. The skirts are one piece with the ceiling and run unbroken along the
// case: no slits, no free tabs. At three places per side the skirt is thinned
// to 1 mm from the inside (a pocket framed by the full 2 mm skirt, as on the fan
// dongle case), and a rounded bump on the outside of each pocket, near the
// skirt's lower edge, clicks into a matching rounded seat in the tray wall.
// Closing, the bump rides down the tray wall with the pocket flexed 0.4 mm and
// drops into its seat; closed, nothing is under load (0.1 mm clearance), so the
// fit doesn't creep loose. The 35 deg flanks let a firm pull open it again.
// (History: a separate "lip" was removed 2026-11-11 because it was not attached
// to the lid; slit snap tabs with windows were removed 2026-11-12 because they
// were fragile; a plain friction fit, wedge ridges pressing on the skirt, was
// replaced the same day by this detent.)
//
// Render:
//   openscad -o tray.stl -D 'part="tray"' sump_case.scad
//   openscad -o lid.stl  -D 'part="lid"'  sump_case.scad   (printed upside down, ceiling on the bed)
//   part="both" (default) shows an assembled preview with dummy modules.
//
// Values marked MEASURE are placeholders until checked with calipers.
// Coordinates: x along the case from the USB-C end, y across, z up from the
// tray floor's top surface. "Inner" = the cavity.

part = "both";
$fn = 40;

// ---------------- lid skirts and detents ----------------
// Pockets as on the fan dongle case (8 mm wide, 1 mm thick); each carries a
// rounded detent bump that clicks into a seat in the tray wall.
skirt_t    = 2;      // skirt thickness
skirt_gap  = 0.2;    // skirt to the tray wall
skirt_z0   = 9;      // skirt's lower edge above the floor
pad_w      = 8;      // thinned pocket width (fan case: snap_tab_w)
pad_t      = 1;      // skirt thickness left at the pocket (fan case: snap_tab_thickness)
detent     = 0.4;    // TUNE: how far the bump reaches past the tray wall's face (= the click's overlap)
detent_clr = 0.1;    // seat clearance around the bump when closed (no preload)
bump_p     = skirt_gap + detent;   // bump height off the skirt's outer face
bump_ang   = 35;     // flank angle from the wall: lower = easier on and off
bump_flat  = 0.4;    // crest length (rounded)
bump_round = 0.2;    // crest rounding radius
bump_w     = pad_w - 2;            // along the case, on the flexible pocket only
bump_zc    = skirt_z0 + 1.6;       // bump centre, near the skirt's free lower edge
seat_w     = bump_w + 1;
skirt_snap_x = [18, 48, 98];       // clear of the AHT20 vents (56-71)

// ---------------- case shell ----------------
wall      = 2;
floor_t   = 2;
lid_t     = 2;
in_l      = 110;     // cavity length: far clip + 0.6 mm to the end wall
in_w      = 43 + 2 * (skirt_t + skirt_gap);   // cavity width: the SR04's 42 mm length runs across the case; the skirts need their own room
in_h      = 24;      // cavity height, floor top to lid underside
corner_r  = 2;
fit       = 0.2;     // general FDM clearance

// ---------------- carrier (from the KiCad board) ----------------
pcb_l = 75.5; pcb_w = 24; pcb_t = 1.6;
pcb_x0 = 0.3;                      // gap between the USB wall and the board's edge
pcb_y0 = (in_w - pcb_w) / 2;
standoff_h = 3;                    // floor to carrier underside (solder tails)
pcb_top = standoff_h + pcb_t;
// KiCad board coords (x right, y down) -> case coords
function bxy(p) = [pcb_x0 + p[0], pcb_y0 + (pcb_w - p[1])];
// Holes: [kicad x, kicad y, hook-post OD (0 = floor pin only)]
holes = [
  [24.4, 12.0, 3.4],   // H1, between ESP and ADXL (tight: ESP edge and ADXL body)
  [51.0, 12.6, 4.5],   // H2, between the caps and the AHT20
  [73.2,  3.0, 0],     // H3, under the SR04 board: floor pin only
  [73.2, 21.0, 0],     // H4
  [3.75,  8.0, 0],     // H5, under the ESP32 at the USB end: standoff + pin; the lid's
  [3.75, 16.0, 0],     // H6  USB tongue clamps the connector right above them
];
pin_d   = 2.2;          // through the 2.5 mm holes
pin_d_snug = 2.4;       // holes without a hook post (H3-H6) are press-fit
boss_d  = 5;
hook_gap = 0.3;          // hook post stops this far above the carrier
// A hook post normally has a hollow tip that fits over the tray pin. Where the post
// is too thin for that (H1: 3.4 mm post, 2.8 mm bore = a 0.3 mm wall that doesn't
// print), the post is solid instead and the tray pin stops just below the carrier's
// top face, so the post presses on the board around the hole.
hook_bore_d = pin_d + 0.6;
min_tip_wall = 0.8;
function solid_post(h) = h[2] > 0 && (h[2] - hook_bore_d) / 2 < min_tip_wall;

// ---------------- ESP32-C3 SuperMini USB-C ----------------
esp_top   = pcb_top + 3.74;  // module PCB top (2.54 header spacer + 1.2 PCB), from the fan case
usb_h     = 3.2;
usb_w     = 9;
usb_margin = 0.4;
usb_y     = bxy([0, 12])[1];
usb_z0    = esp_top - usb_margin;
usb_z1    = esp_top + usb_h + usb_margin;
usb_port_w = usb_w + 1;      // opening width (bare connector + 0.5 each side), as on the fan case
usb_zc    = (usb_z0 + usb_z1) / 2;
// USB-C opening: a stadium (rounded ends, the connector's own outline), as on the
// fan case. Built in the end wall's frame: x through the wall, centred on the port.
// The tray holds the lower half and an open notch above it; the lid's tongue fills
// the notch and carries the upper half, so together they make the full stadium.
module usb_port_cutter(depth) {
  r = (usb_z1 - usb_z0) / 2;
  translate([-1, wall + usb_y, floor_t + usb_zc]) hull()
    for (s = [-1, 1]) translate([0, s * (usb_port_w / 2 - r), 0]) rotate([0, 90, 0]) cylinder(r = r, h = depth);
}
// Button guide tunnel over RESET: user-measured 9 mm from the module's USB end and
// 5 mm from its side: the right side with the USB port up, parts side up (board y 8).
btn_xy    = bxy([9.0, 8.0]);
btn_hole_d = 3;              // fits a toothpick / paperclip
btn_tube_wall = 1;
btn_top_above_esp = 2;       // tact switch height above the module PCB (guess, as in the fan case)
btn_gap = 1;                 // tube stops this far above the button

// ---------------- lid holes ----------------
// A symmetric 5 mm grid centred on the lid. The middle strip along the length holds
// the button tube, the two clamping posts and the JST slot, so holes stay out of it.
//   "rows2": one straight row of holes along each long side, full length.
//   "rows4": two rows of holes along each long side, plus shallow dimples on the
//            middle two grid rows (fan-case look). Holes that would land on an SR04
//            screw boss become dimples.
hole_pattern = "rows4";
dot_spacing = 5;
dot_r = 1.8;
dot_depth = 0.35;
vent_hole_d = 3;
rows2_offset = 7;           // rows2: hole rows this far either side of the centre line
texture_margin = 6;

// ---------------- AJ-SR04M (under the lid, parts facing down) ----------------
sr_l = 42; sr_w = 28.5; sr_t = 1.6;   // user-measured
// The SR04 is turned so its length runs across the case and its I/O header edge faces
// the carrier: the straight right-angle pins point back toward the carrier, female
// Dupont ends plug straight on, and the wires drop to J3. SR04-local coordinates:
// u along its length from the crystal end, v across its width from the header edge.
sr_plug_len = 11;           // board edge to the end of the Dupont housings: pins reach 5 mm past the edge,
                            // the header body is 1 mm in, so the 12 mm housings (user-measured) end ~11 mm out
sr_x0 = (pcb_x0 + 67.9) + 0.5 + sr_plug_len;   // header edge (plug ends ~4 mm short of J3: the wires make a C-bend down to it)
sr_y0 = (in_w - sr_l) / 2;  // centred across the case
function srp(uv) = [sr_x0 + uv[1], sr_y0 + uv[0]];
sr_hdr_cu = sr_l / 2;       // header centred on the long edge (user-measured; 10 mm wide, 1 mm in from the edge)
sr_plug_w = 10.2;           // 4 Dupont housings side by side
sr_pin_z = 1.27;            // pin axis below the board's parts-side face
sr_parts_h = 9;             // tallest part on its parts side (trimmer pot, user-measured)
sr_standoff = 4;            // clears its back-face solder tails and makes the clip arms longer
sr_hole_u = 1.5 + 1.25;     // hole centres from the short edges (user: 2.5 mm holes, 1.5 mm from the edge)
sr_hole_v = 1.0 + 1.25;     // ...and from the long edges (1 mm from the edge)
sr_pin_d = 2.2;             // locating pins on two diagonal pads, through the SR04's 2.5 mm holes
// Snap clips: arms hang from the lid beside the SR04's long edges and hook under its
// parts-side face. Two per edge, near the ends, clear of the bent header pins.
sr_clip_u = [8, sr_l - 8];  // clip centres along both long edges, clear of the header (keep clear of edge parts: MEASURE)
clip_w = 6;                 // arm width along the edge
clip_t = 0.8;               // arm thickness (flexes outward): two 0.4 mm perimeters
clip_gap = 0.2;             // arm to board edge
clip_hook = 0.4;            // how far the hook reaches under the board
clip_relief = 1.4;          // blind groove into the lid's underside around each arm (0.6 mm skin left on top)
clip_relief_gap = 0.6;      // groove width outside the arm and past its ends
// Arm length from the groove bottom to the hook: 1.4 + 4 + 1.6 = ~7 mm; bending 0.4 mm strains a
// 0.8 mm arm about 1%, fine for PLA or PETG.
clip_lead = 1.8;            // height of the hook's lead-in ramp
jst_cu = sr_l / 2;          // probe socket 17.5 mm from each short edge (user-measured)
jst_cv = sr_w / 2;          // 12 mm from each long edge (user-measured)
jst_body = [7, 4.5, 9];     // along u, along v, height above the board (user-measured)
jst_slot = [8.5, 6.6];      // socket 7 x 4.5 + clearance; the plug must also pass (check it)

// ---------------- AHT20 ventilation ----------------
aht_x = [pcb_x0 + 55.9, pcb_x0 + 70.9];   // AHT20 board span: 15 x 15, lying flat away from J2 (user-measured)
aht_z = 2.5;                               // carrier top to the AHT20 board's underside (user-measured)
cap_d = 7; cap_h = 12;                     // 470 uF caps (user-measured), 2.5 mm lead pitch
gy_w = 9; gy_l = 20;                       // GY-346 (user-measured), lying flat from J1 toward the caps
gy_z = 2.5;                                // soldered on its own header like the AHT20 (user, after bench testing)
vent_w = 1.6; vent_pitch = 3.2;

// ---------------- strap slots through the side walls ----------------
// Each zip tie goes in through one side wall at floor level, across the floor
// under the carrier (3 mm gap) and out the other side, then round the pipe.
// The slots sit where the carrier has no solder tails underneath (board x
// 29.5-43.5 and 58.5-71.5 are clear), so a tie threads with the carrier in.
// The underside of the tray is flat: no rails, no pipe channel.
strap_x = [36.5, 65];       // cavity x: under the GY-346, and under the AHT20
strap_w = 5.5; strap_h = 2; // largest tie
strap_slot_w = strap_w + 1;
strap_slot_h = strap_h + 0.6;   // stays under the carrier's underside (standoff_h)
pipe_d = 48.3;              // 1-1/2" ABS outlet (preview only)

out_l = in_l + 2 * wall; out_w = in_w + 2 * wall;
tray_h = in_h;       // tray walls run the full height

module rbox(l, w, h, r) {
  translate([r, r, 0]) linear_extrude(h) offset(r) square([l - 2 * r, w - 2 * r]);
}
module at_inner() { translate([wall, wall, floor_t]) children(); }

// =============== TRAY ===============
module tray() {
  difference() {
    rbox(out_l, out_w, floor_t + tray_h, corner_r);
    at_inner() translate([0, 0, 0]) cube([in_l, in_w, tray_h + 1]);
    usb_port_cutter(wall + 2);
    // the USB end is a low stub wall up to the port's centre line; the lid's
    // end wall drops onto it, across the full width
    translate([-1, -1, floor_t + usb_zc]) cube([wall + fit + 1, out_w + 2, 50]);
    // label on the long side wall (the y = 0 side), above the vents
    translate([out_l / 2, 0.5, floor_t + 17.5])
      rotate([90, 0, 0]) linear_extrude(1)
        text("SUMP MASTER 2000", size = 5, halign = "center", valign = "center");
    // AHT20 side vents (no floor vents: the underside is solid)
    for (x = [aht_x[0] + 1 : vent_pitch : aht_x[1] - 1])
      translate([wall + x, -1, floor_t + 4]) cube([vent_w, out_w + 2, 7]);
    // strap slots through both side walls, at floor level
    for (x = strap_x) translate([wall + x - strap_slot_w / 2, -1, floor_t]) cube([strap_slot_w, out_w + 2, strap_slot_h]);
    // rounded seats that the lid skirts' detent bumps click into
    at_inner() for (x = skirt_snap_x, side = [0, 1]) detent_seat(x, side);
  }
  // (detent seats are cut into the long walls in the difference above)
  // standoffs + pins
  at_inner() for (h = holes) {
    p = bxy(h);
    translate([p[0], p[1], 0]) {
      cylinder(d = boss_d, h = standoff_h);
      pin_top = solid_post(h) ? pcb_top - 0.9 : pcb_top + 1.2;   // short pin under a solid post
      cylinder(d = h[2] > 0 ? pin_d : pin_d_snug, h = pin_top);
      translate([0, 0, pin_top]) cylinder(d1 = h[2] > 0 ? pin_d : pin_d_snug, d2 = 1.4, h = 0.6);
    }
  }
}

// Detent bump profile, in the plane across a long wall: q = distance from the
// skirt's outer face toward the tray wall, z = height. Trapezoid with bump_ang
// flanks and a flat crest, its convex corners rounded. grow > 0 makes the seat.
module bump_profile(grow = 0) {
  run = bump_p / tan(bump_ang);
  offset(delta = grow) offset(r = bump_round) offset(delta = -bump_round)
    polygon([[-0.4, bump_zc - bump_flat / 2 - run - 0.4], [0, bump_zc - bump_flat / 2 - run],
             [bump_p, bump_zc - bump_flat / 2], [bump_p, bump_zc + bump_flat / 2],
             [0, bump_zc + bump_flat / 2 + run], [-0.4, bump_zc + bump_flat / 2 + run + 0.4]]);   // base sunk into the skirt: one solid
}
// Place a q-z profile on one long side, extruded w along the case, in cavity coordinates.
module on_side(x, side, w) {
  translate([x - w / 2, 0, 0]) rotate([90, 0, 90]) linear_extrude(w)
    if (side) translate([in_w - skirt_gap, 0]) children(); else translate([skirt_gap, 0]) mirror([1, 0]) children();
}
module detent_bump(x, side) on_side(x, side, bump_w) intersection() {
  bump_profile();
  translate([-0.3, 0]) square([10, 100]);   // keep the base inside the 1 mm pocket skin
}
module detent_seat(x, side) on_side(x, side, seat_w) bump_profile(detent_clr);

// One lid skirt, in cavity coordinates: a 2 mm wall hanging from the ceiling
// just inside a long tray wall, unbroken along its length. At each detent it is
// thinned from the inside to a 1 mm pocket (open at the bottom edge, as on the
// fan case) so it flexes as the bump rides over the tray wall; the outside face is
// flat apart from the bump, and the full-thickness skirt frames the pocket on both
// sides and at the ceiling, so there is nothing free-standing to snap off.
// A rounded detent bump sits on the pocket's outer face near the lower edge.
module skirt(side) {
  y0 = side ? in_w - skirt_gap - skirt_t : skirt_gap;
  difference() {
    translate([1, y0, skirt_z0]) cube([in_l - 1.4, skirt_t, in_h - skirt_z0 + 0.01]);
    for (x = skirt_snap_x)
      translate([x - pad_w / 2, side ? y0 - 1 : y0 + pad_t, skirt_z0 - 1])
        cube([pad_w, skirt_t - pad_t + 1, in_h - skirt_z0 + 1]);
  }
  for (x = skirt_snap_x) intersection() {
    detent_bump(x, side);
    translate([0, -5, skirt_z0]) cube([in_l, in_w + 10, in_h]);   // nothing below the skirt's edge
  }
}

// The lid's shell: flat ceiling, the two skirts, and an end
// wall at the USB end that sits on the tray's stub wall and carries the top half
// of the USB-C opening.
module lid_shell_skirt() {
  translate([0, 0, floor_t + in_h]) rbox(out_l, out_w, lid_t, corner_r);
  at_inner() for (side = [0, 1]) skirt(side);
  difference() {
    intersection() {
      rbox(out_l, out_w, floor_t + in_h + 0.01, corner_r);
      translate([-1, -1, floor_t + usb_zc + 0.1]) cube([wall + 1, out_w + 2, 100]);
    }
    usb_port_cutter(wall + 2);
  }
}

// =============== LID (modelled in place; flip to print) ===============
module lid() {
  difference() {
    union() {
      lid_shell_skirt();
      // hook posts that clamp the carrier
      at_inner() for (h = holes) if (h[2] > 0) {
        p = bxy(h);
        translate([p[0], p[1], pcb_top + hook_gap]) difference() {
          cylinder(d = h[2], h = in_h - pcb_top - hook_gap + 0.01);   // 0.01 into the ceiling: one solid
          if (!solid_post(h)) translate([0, 0, -1]) cylinder(d = hook_bore_d, h = 3.5);   // 1 mm clear above the pin tip
        }
      }
      // button guide tube, hanging from the ceiling to just above the ESP's button
      at_inner() translate([btn_xy[0], btn_xy[1], esp_top + btn_top_above_esp + btn_gap])
        cylinder(d = btn_hole_d + 2 * btn_tube_wall, h = in_h - (esp_top + btn_top_above_esp + btn_gap) + 0.01);
      // SR04 stand-off pads (its back face rests on these), locating pins on two diagonal corners
      at_inner() for (i = [0, 1], j = [0, 1])
        translate(concat(srp([i ? sr_l - sr_hole_u : sr_hole_u, j ? sr_w - sr_hole_v : sr_hole_v]),
                         [in_h - sr_standoff])) {
          cylinder(d = 5, h = sr_standoff + 0.01);
          if (i == j) {
            translate([0, 0, -sr_t - 0.6]) cylinder(d = sr_pin_d, h = sr_t + 0.6 + 0.01);
            translate([0, 0, -sr_t - 1.0]) cylinder(d1 = sr_pin_d - 0.6, d2 = sr_pin_d, h = 0.4);
          }
        }
      // SR04 snap clips
      at_inner() for (cu = sr_clip_u, side = [0, 1]) sr_clip(cu, side);
    }
    // probe socket slot
    // (the JST's two pins sit along the SR04's length, which now runs across the case)
    at_inner() translate(concat(srp([jst_cu, jst_cv]) - [jst_slot[1] / 2, jst_slot[0] / 2], [in_h - 1]))
      cube([jst_slot[1], jst_slot[0], lid_t + 2]);
    // button tunnel bore
    at_inner() translate([btn_xy[0], btn_xy[1], esp_top + btn_top_above_esp + btn_gap - 0.5])
      cylinder(d = btn_hole_d, h = in_h + lid_t);
    // relief grooves around the clip arms
    at_inner() for (cu = sr_clip_u, side = [0, 1]) sr_clip_relief(cu, side);
    // dimples and vent holes in the top
    lid_texture();
    // the tray's AHT20 side vents carry on through the skirts
    for (x = [aht_x[0] + 1 : vent_pitch : aht_x[1] - 1])
      translate([wall + x, -1, floor_t + 4]) cube([vent_w, out_w + 2, 7]);
  }
}

// One SR04 snap clip, in cavity coordinates, at position u along one of the SR04's long
// edges: side 0 = the header edge (facing the carrier), 1 = the far edge. The arm hangs
// from the ceiling just outside the board edge; its hook reaches under the board's
// parts-side face, with a ramp below so the board pushes it aside on the way in.
module sr_clip(u, side) {
  board_bot = in_h - sr_standoff - sr_t;
  edge = sr_x0 + (side ? sr_w : 0);
  dir = side ? 1 : -1;                         // outward, along x
  arm_in = edge + dir * clip_gap;
  bot = board_bot - 0.05 - clip_lead;
  y = sr_y0 + u;
  translate([side ? arm_in : arm_in - clip_t, y - clip_w / 2, bot])
    cube([clip_t, clip_w, in_h + clip_relief - bot]);
  translate([0, y + clip_w / 2, 0]) rotate([90, 0, 0]) linear_extrude(clip_w)
    polygon([[arm_in, board_bot - 0.05], [arm_in - dir * (clip_gap + clip_hook), board_bot - 0.05], [arm_in, bot]]);
}

// Blind relief groove around a clip arm, open on its outer side and ends, so the arm's
// root is up inside the lid (longer arm, gentler bend) while the lid top stays closed.
module sr_clip_relief(u, side) {
  edge = sr_x0 + (side ? sr_w : 0);
  dir = side ? 1 : -1;
  arm_in = edge + dir * clip_gap;
  arm_out = arm_in + dir * clip_t;
  g = clip_relief_gap;
  y = sr_y0 + u;
  x0 = min(arm_in, arm_out + dir * g); x1 = max(arm_in, arm_out + dir * g);
  difference() {
    translate([x0, y - clip_w / 2 - g, in_h - 0.01]) cube([x1 - x0, clip_w + 2 * g, clip_relief + 0.01]);
    translate([min(arm_in, arm_out), y - clip_w / 2, in_h - 1]) cube([clip_t, clip_w, clip_relief + 2]);
  }
}
function clip_centres() = [for (cu = sr_clip_u, side = [0, 1])
  [sr_x0 + (side ? sr_w + clip_gap + clip_t / 2 : -clip_gap - clip_t / 2), sr_y0 + cu]];

// Holes / dimples on the lid top (outer-shell coordinates), symmetric about both centre lines.
module lid_texture() {
  top = floor_t + in_h + lid_t;
  n_cols = floor((out_l - 2 * texture_margin) / dot_spacing) + 1;
  u0 = (out_l - (n_cols - 1) * dot_spacing) / 2;
  yc = out_w / 2;
  o = [wall, wall];
  bosses = [for (i = [0, 1], j = [0, 1])
            o + srp([i ? sr_l - sr_hole_u : sr_hole_u, j ? sr_w - sr_hole_v : sr_hole_v])];
  btn = o + btn_xy;
  jst = o + srp([jst_cu, jst_cv]);
  function near(p, list, r) = len([for (q = list) if (norm(p - q) < r) 1]) > 0;
  clips = [for (c = clip_centres()) o + c];
  function over_clip(p) = len([for (c = clips) if (abs(p[0] - c[0]) < 3.5 && abs(p[1] - c[1]) < clip_w / 2 + 3) 1]) > 0;
  module hole(p) if (!over_clip(p)) translate([p[0], p[1], top - lid_t - 1]) cylinder(d = vent_hole_d, h = lid_t + 2, $fn = 24);
  module dimple(p) if (!over_clip(p)) translate([p[0], p[1], top + dot_r - dot_depth]) sphere(r = dot_r, $fn = 16);
  for (i = [0 : n_cols - 1]) {
    x = u0 + i * dot_spacing;
    if (hole_pattern == "rows2") {
      for (s = [-1, 1]) hole([x, yc + s * rows2_offset]);
    } else {
      for (k = [-2.5, -1.5, -0.5, 0.5, 1.5, 2.5]) {
        p = [x, yc + k * dot_spacing];
        if (abs(k) > 1) {
          if (near(p, bosses, 4.5)) dimple(p); else hole(p);
        } else if (norm(p - btn) > 3 && !(abs(p[0] - jst[0]) < jst_slot[1] / 2 + 2 && abs(p[1] - jst[1]) < jst_slot[0] / 2 + 2))
          dimple(p);
      }
    }
  }
}

// =============== preview dummies ===============
module dummies() {
  at_inner() {
    color("green") translate([pcb_x0, pcb_y0, standoff_h]) cube([pcb_l, pcb_w, pcb_t]);
    color("royalblue") translate([pcb_x0, pcb_y0 + 3, pcb_top + 2.54]) cube([22.5, 18, 1.2]);
    color("silver") translate([pcb_x0 - 1, usb_y - usb_w / 2, esp_top]) cube([7.5, usb_w, usb_h]);
    color("orange") translate([pcb_x0 + 28.1 - 1.27, pcb_y0 + 12 - gy_l / 2, pcb_top + gy_z]) cube([gy_w, gy_l, 1.6]);
    // what each module sits on (from the KiCad footprint positions)
    color("black") {
      // J1: the GY-346's own 8-pin header, soldered in (2.5 mm plastic spacer)
      translate(concat(bxy([28.1 - 1.27, 12 + 10.16]), [pcb_top])) cube([2.54, 20.32, gy_z]);
      // J2: the AHT20's own header, soldered in (2.5 mm plastic spacer)
      translate(concat(bxy([57.2 - 1.27, 12 + 5.08]), [pcb_top])) cube([2.54, 10.16, aht_z]);
      // J3: the SR04 Dupont wires' male pins, soldered in, wires rising
      translate(concat(bxy([72.8 - 1.27, 12 + 5.08]), [pcb_top])) cube([2.54, 10.16, 8]);
      // ESP32: two 1x8 male header strips (2.54 mm plastic) under the module
      for (ky = [4.38, 19.62]) translate(concat(bxy([11.25 - 10.16, ky + 1.27]), [pcb_top])) cube([20.32, 2.54, 2.54]);
    }
    color("pink") translate([aht_x[0], pcb_y0 + 12 - 7.5, pcb_top + aht_z]) cube([aht_x[1] - aht_x[0], 15, 1.6]);
    for (y = [6.9, 17.1]) color("steelblue") translate(concat(bxy([45.8, y]), [pcb_top])) cylinder(d = cap_d, h = cap_h);
    color("skyblue") translate([sr_x0, sr_y0, in_h - sr_standoff - sr_t]) cube([sr_w, sr_l, sr_t]);
    // tallest part: a trimmer pot, 9 mm (user-measured; position not measured, drawn mid-board)
    color("dimgray") translate(concat(srp([sr_l / 2 + 8, sr_w / 2]) - [3, 3], [in_h - sr_standoff - sr_t - sr_parts_h])) cube([6, 6, sr_parts_h]);
    color("white") translate(concat(srp([jst_cu, jst_cv]) - [jst_body[1] / 2, jst_body[0] / 2], [in_h - sr_standoff])) cube([jst_body[1], jst_body[0], jst_body[2]]);
    // its straight right-angle header + the female Dupont housings on it, pointing at the carrier
    color("black") translate(concat(srp([sr_hdr_cu, 0]) - [sr_plug_len, sr_plug_w / 2],
                                    [in_h - sr_standoff - sr_t - sr_pin_z - 1.27]))
      cube([sr_plug_len, sr_plug_w, 2.54]);
  }
}

// the pump outlet pipe under the flat floor, and the two zip ties (preview only)
module pipe_dummy() {
  translate([-20, out_w / 2, -pipe_d / 2]) rotate([0, 90, 0]) cylinder(d = pipe_d, h = out_l + 40, $fn = 96);
}
module tie_dummies() {
  for (x = strap_x) color("black") {
    translate([wall + x - strap_w / 2, -0.5, floor_t + 0.2]) cube([strap_w, out_w + 1, 1.3]);
    for (y = [-1.8, out_w + 0.5]) translate([wall + x - strap_w / 2, y, -pipe_d / 2]) cube([strap_w, 1.3, pipe_d / 2 + floor_t + 1.5]);
  }
}

if (part == "none") { }
else if (part == "tray") tray();
else if (part == "lid") translate([0, out_w, floor_t + in_h + lid_t]) rotate([180, 0, 0]) lid();
else if (part == "clash") intersection() { tray(); lid(); }   // must be empty (the detents have clearance when closed)
else if (part == "section") intersection() { union() { tray(); lid(); dummies(); } translate([-5, wall + in_w / 2, -10]) cube([200, 50, 60]); }
else { tray(); color("white", 0.35) lid(); dummies(); tie_dummies(); color("lightgray", 0.5) pipe_dummy(); }
