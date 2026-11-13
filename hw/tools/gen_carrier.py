#!/usr/bin/env python3
"""Generate the Sump Master 2000 carrier: schematic + placed and routed PCB.

Run with KiCad's bundled Python (it needs the pcbnew module):

  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 \
      hw/tools/gen_carrier.py

Writes hw/sump_carrier.kicad_sch and hw/sump_carrier.kicad_pcb (overwrites both).
Everything (parts, nets, positions, tracks) is in the tables below; edit them and
re-run rather than hand-editing the outputs, or switch to hand-editing in KiCad
once you start tweaking the layout there (then don't re-run this).

Board coordinates are mm in board-local space: origin at the board's top-left
corner (USB-C end), +x toward the far (SR04) end, +y down (KiCad convention).
The board sits at (100, 100) on the KiCad page.
"""
import os
import re
import uuid

import pcbnew

HW = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = "sump_carrier"
KICAD = "/Applications/KiCad/KiCad.app/Contents/SharedSupport"
SYMDIR = KICAD + "/symbols"
FPDIR = KICAD + "/footprints"
OX, OY = 100.0, 100.0          # board origin on the page

# ---------------------------------------------------------------------------
# Board outline
# ---------------------------------------------------------------------------
BOARD_W, BOARD_H = 75.5, 24.0

# ---------------------------------------------------------------------------
# Parts: ref -> (lib_symbol, footprint, value, (x, y) of footprint origin, rot,
#                {pin: net}, schematic (x, y))
# Footprint origin is pin 1 for headers/caps/sockets and the centre for U1/H.
# ---------------------------------------------------------------------------
NC = None
PARTS = {
    "U1": ("Custom_Library:ESP32-C3_SuperMini", "Custom_Library:ESP32-C3_SuperMini_FaceUp",
           "ESP32-C3_SuperMini", (11.25, 12.0), 0,
           {"1": NC, "2": "GND", "3": "+3V3", "4": "ECHO", "5": "TRIG", "6": NC, "7": NC, "8": NC,
            "9": "SDA", "10": "SCL", "11": NC, "12": NC, "13": NC, "14": NC, "15": NC, "16": NC},
           (63.5, 88.9)),
    # GY-346 (ADXL346) socket. Pin order from hw/DESIGN.md -- VERIFY against the module.
    # CS/SDO/INTs unconnected (the module's own pull resistors set I2C mode, address 0x53).
    "J1": ("Connector_Generic:Conn_01x08", "Connector_PinHeader_2.54mm:PinHeader_1x08_P2.54mm_Vertical",
           "GY-346 ADXL346", (28.1, 3.11), 0,
           {"1": "+3V3", "2": "GND", "3": "SCL", "4": "SDA", "5": NC, "6": NC, "7": NC, "8": NC},
           (139.7, 63.5)),
    # AHT20(+BMP280) socket, pin 1 at the top edge: VDD, SDA, GND, SCL (user-verified 2026-11-08).
    "J2": ("Connector_Generic:Conn_01x04", "Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical",
           "AHT20", (57.2, 8.19), 0,
           {"1": "+3V3", "2": "SDA", "3": "GND", "4": "SCL"},
           (139.7, 101.6)),
    # Wires to the AJ-SR04M (its I/O header order is 5V, Trig, Echo, GND): Dupont wires
    # soldered straight into these pads, female ends onto the SR04's own pins.
    # 3.3 V board: Echo goes straight to GPIO 4.
    "J3": ("Connector_Generic:Conn_01x04", "Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical",
           "SR04 wires", (72.8, 8.19), 0,
           {"1": "+3V3", "2": "TRIG", "3": "ECHO", "4": "GND"},
           (139.7, 127.0)),
    "C1": ("Device:C_Polarized", "Capacitor_THT:CP_Radial_D7.5mm_P2.50mm",
           "470uF (ADXL)", (44.55, 6.9), 0, {"1": "+3V3", "2": "GND"}, (88.9, 132.08)),
    "C2": ("Device:C_Polarized", "Capacitor_THT:CP_Radial_D7.5mm_P2.50mm",
           "470uF (AHT20)", (44.55, 17.1), 0, {"1": "+3V3", "2": "GND"}, (104.14, 132.08)),
    # Case alignment-pin holes (no screws). H5/H6 sit under the ESP32 near the USB end
    # (same spots as the fan board): standoffs there take the USB plug's push and pull.
    # H1/H2 flank the ADXL and get lid hook posts; H3/H4 are under the SR04 (pins only).
    "H1": ("Mechanical:MountingHole", "Custom_Library:CasePin_Hole_2.5mm", "pin", (24.4, 12.0), 0, {}, (215.9, 154.94)),
    "H2": ("Mechanical:MountingHole", "Custom_Library:CasePin_Hole_2.5mm", "pin", (51.0, 12.6), 0, {}, (226.06, 154.94)),
    "H3": ("Mechanical:MountingHole", "Custom_Library:CasePin_Hole_2.5mm", "pin", (73.2, 3.0), 0, {}, (236.22, 154.94)),
    "H4": ("Mechanical:MountingHole", "Custom_Library:CasePin_Hole_2.5mm", "pin", (73.2, 21.0), 0, {}, (246.38, 154.94)),
    "H5": ("Mechanical:MountingHole", "Custom_Library:CasePin_Hole_2.5mm", "pin", (3.75, 8.0), 0, {}, (256.54, 154.94)),
    "H6": ("Mechanical:MountingHole", "Custom_Library:CasePin_Hole_2.5mm", "pin", (3.75, 16.0), 0, {}, (266.7, 154.94)),
    "#FLG1": ("power:PWR_FLAG", None, "PWR_FLAG", None, 0, {"1": "GND"}, (88.9, 154.94)),
}

# ---------------------------------------------------------------------------
# Tracks: (net, layer, width, [points]). A point is (x, y) or "REF.pad".
# ---------------------------------------------------------------------------
F, B = "F.Cu", "B.Cu"
PWR, SIG = 0.5, 0.25
TRACKS = [
    # 3V3: ESP -> ADXL, then along the top edge to the AHT20, the SR04 lead and C1;
    # C2 via the bottom layer (the SCL lane is in the way on top).
    ("+3V3", F, PWR, ["U1.3", (7.44, 6.6), (21.0, 6.6), (24.5, 3.11), "J1.1"]),
    ("+3V3", F, PWR, ["J1.1", (29.91, 1.3), (69.0, 1.3), (71.0, 3.3), (71.0, 6.39), "J3.1"]),
    ("+3V3", F, PWR, [(55.5, 1.3), (57.2, 3.0), "J2.1"]),
    ("+3V3", F, PWR, [(43.2, 1.3), (44.55, 2.65), "C1.1", (44.55, 12.5)]),
    ("+3V3", B, PWR, [(44.55, 12.5), "C2.1"]),
    # I2C under the ESP: SCL on top, SDA on the bottom then up a via (they cross here
    # because the ESP's pin order is SDA, SCL and the ADXL's is SCL, SDA).
    ("SCL", F, SIG, ["U1.10", (6.17, 18.35), (6.17, 9.46), (7.44, 8.19), "J1.3"]),
    ("SDA", B, SIG, ["U1.9", (3.36, 18.62), (3.98, 18.0), (9.5, 18.0), (9.5, 10.0)]),
    ("SDA", F, SIG, [(9.5, 10.0), (26.0, 10.0), (26.73, 10.73), "J1.4"]),
    # ...and they cross again between the ADXL (SCL, SDA) and the AHT20 (SDA, SCL):
    # SDA straight across on the bottom, SCL down and across on top.
    ("SDA", B, SIG, ["J1.4", "J2.2"]),
    ("SCL", F, SIG, ["J1.3", (30.0, 8.19), (31.5, 9.69), (31.5, 14.3), (55.0, 14.3), (56.51, 15.81), "J2.4"]),
    # SR04 Trig: B.Cu between the ESP's pins, then along the bottom edge.
    ("TRIG", B, SIG, ["U1.5", (12.52, 6.0), (13.79, 7.27), (13.79, 22.0), (14.49, 22.7),
                      (67.0, 22.7), (69.2, 20.5), (69.2, 12.0), "J3.2"]),
    # SR04 Echo: B.Cu under the ESP, via to F.Cu, along the bottom edge to J3.
    ("ECHO", B, SIG, ["U1.4", (9.98, 6.0), (11.25, 7.27), (11.25, 23.1)]),
    ("ECHO", F, SIG, [(11.25, 23.1), (66.5, 23.1), (68.5, 21.1), (68.5, 15.27), "J3.3"]),
]
VIAS = [
    ("SDA", (9.5, 10.0)), ("ECHO", (11.25, 23.1)), ("+3V3", (44.55, 12.5)),
    # GND stitching between the two pours
    ("GND", (18.0, 13.5)), ("GND", (38.0, 12.3)), ("GND", (38.0, 20.5)),
    ("GND", (49.0, 20.5)), ("GND", (53.5, 4.0)), ("GND", (65.5, 5.0)), ("GND", (2.0, 22.8)),
]

# Silkscreen text: (text, (x, y), size, layer, rotation)
SILK = [
    ("SUMP MASTER 2000", (37.5, 22.9), 0.9, "F.SilkS", 0),
    ("v1.0", (62.0, 22.0), 0.8, "F.SilkS", 0),
    # Bottom side (mirrored so it reads correctly from underneath): project URL, name/version, logo
    ("github.com/bradrblack/sump-master-2000", (37.75, 1.2), 1.0, "B.SilkS", 0),
    ("SUMP MASTER 2000  PCB v1.0", (37.75, 22.75), 0.9, "B.SilkS", 0),
    ("USB", (1.2, 12.0), 0.8, "F.SilkS", 90),
]
PIN_LABELS = {  # labels beside each socket pin, to check module orientation before soldering
    "J1": ["VIN", "GND", "SCL", "SDA", "SDO", "CS", "IT2", "IT1"],
    "J2": ["VDD", "SDA", "GND", "SCL"],
    "J3": ["VCC", "TRG", "ECH", "GND"],
}
# Module outlines on F.Fab (user-measured 2026-11-13; header pin row assumed 1.27 mm in from
# the module's edge, each module lying flat away from its header): (label, x0, y0, x1, y1)
FAB = [
    ("ESP32-C3 SuperMini", 0.0, 3.0, 22.5, 21.0),
    ("GY-346 14x21", 26.83, 1.5, 40.83, 22.5),
    ("AHT20 15x15", 55.93, 4.5, 70.93, 19.5),
]


# ===========================================================================
# Schematic
# ===========================================================================
def U():
    return str(uuid.uuid4())


def find_block(text, start):
    """Return the s-expression starting at text[start] == '('."""
    depth, i, instr = 0, start, False
    while True:
        c = text[i]
        if instr:
            if c == "\\":
                i += 1
            elif c == '"':
                instr = False
        elif c == '"':
            instr = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1


_libcache = {}


def lib_symbol(lib_id):
    lib, name = lib_id.split(":")
    path = os.path.join(HW, "Custom_Library.kicad_sym") if lib == "Custom_Library" \
        else os.path.join(SYMDIR, lib + ".kicad_sym")
    text = _libcache.setdefault(path, open(path).read())
    m = re.search(r'\n\t+(\(symbol "%s"\s)' % re.escape(name), text)
    if not m:
        raise SystemExit("symbol %s not found in %s" % (name, path))
    block = find_block(text, m.start(1))
    if "(extends" in block:
        raise SystemExit("symbol %s uses extends; flatten it first" % lib_id)
    block = block.replace('(symbol "%s"' % name, '(symbol "%s"' % lib_id, 1)
    pins = {}
    for pm in re.finditer(r'\(pin \w+ \w+\s*\(at ([-\d.]+) ([-\d.]+) ([-\d.]+)\)', block):
        rest = block[pm.end():]
        num = re.search(r'\(number "([^"]+)"', rest).group(1)
        pname = re.search(r'\(name "([^"]*)"', rest).group(1)
        pins[num] = (float(pm.group(1)), float(pm.group(2)), int(float(pm.group(3))), pname)
    return block, pins


def prop(name, value, x, y, hide=False):
    h = "\n\t\t\t(hide yes)" if hide else ""
    return ('\t\t(property "%s" "%s"\n\t\t\t(at %.2f %.2f 0)%s\n'
            '\t\t\t(effects (font (size 1.27 1.27)))\n\t\t)\n' % (name, value, x, y, h))


def build_schematic():
    root = U()
    libs, body = {}, []
    for ref, (lib_id, fp, value, _pos, _rot, nets, (sx, sy)) in PARTS.items():
        if lib_id not in libs:
            libs[lib_id] = lib_symbol(lib_id)
        _blk, pins = libs[lib_id]
        su = PARTS_UUID[ref]
        s = ['\t(symbol\n\t\t(lib_id "%s")\n\t\t(at %.2f %.2f 0)\n\t\t(unit 1)\n'
             '\t\t(exclude_from_sim no)\n\t\t(in_bom %s)\n\t\t(on_board %s)\n\t\t(dnp %s)\n'
             '\t\t(uuid "%s")\n' % (lib_id, sx, sy, "no" if ref.startswith(("#", "H", "JP")) else "yes",
                                    "no" if ref.startswith("#") else "yes",
                                    "yes" if value.startswith("DNP") else "no", su)]
        s.append(prop("Reference", ref, sx, sy - 7.62, hide=ref.startswith("#")))
        s.append(prop("Value", value, sx, sy + 7.62))
        s.append(prop("Footprint", fp or "", sx, sy, hide=True))
        s.append(prop("Datasheet", "", sx, sy, hide=True))
        for num in pins:
            s.append('\t\t(pin "%s" (uuid "%s"))\n' % (num, U()))
        s.append('\t\t(instances (project "%s" (path "/%s" (reference "%s") (unit 1))))\n\t)\n'
                 % (PROJECT, root, ref))
        body.append("".join(s))
        # A label or no-connect at every pin end
        for num, (px, py, ang, _n) in pins.items():
            x, y = sx + px, sy - py
            net = nets.get(num)
            if net is None:
                body.append('\t(no_connect (at %.2f %.2f) (uuid "%s"))\n' % (x, y, U()))
                continue
            # Pin angle points from the connection end toward the body; put the
            # label on the far side.
            lang, just = {0: (180, "right"), 180: (0, "left"), 90: (270, "right"),
                          270: (90, "left")}[ang]
            body.append('\t(label "%s"\n\t\t(at %.2f %.2f %d)\n\t\t(effects (font (size 1.27 1.27)) '
                        '(justify %s bottom))\n\t\t(uuid "%s")\n\t)\n' % (net, x, y, lang, just, U()))
    text = ['(kicad_sch\n\t(version 20260306)\n\t(generator "gen_carrier.py")\n'
            '\t(generator_version "10.0")\n\t(uuid "%s")\n\t(paper "A4")\n' % root,
            '\t(title_block\n\t\t(title "Sump Master 2000 carrier")\n\t\t(rev "A")\n'
            '\t\t(comment 1 "Generated by hw/tools/gen_carrier.py")\n\t)\n',
            "\t(lib_symbols\n"]
    for blk, _p in libs.values():
        text.append("\t\t" + blk.replace("\n", "\n\t") + "\n")
    text.append("\t)\n")
    text.extend(body)
    text.append('\t(sheet_instances (path "/" (page "1")))\n\t(embedded_fonts no)\n)\n')
    open(os.path.join(HW, PROJECT + ".kicad_sch"), "w").write("".join(text))


# ===========================================================================
# PCB
# ===========================================================================
def mm(v):
    return pcbnew.FromMM(v)


def pt(x, y):
    return pcbnew.VECTOR2I(mm(OX + x), mm(OY + y))


def load_fp(fpid):
    lib, name = fpid.split(":")
    path = os.path.join(HW, "Custom_Library.pretty") if lib == "Custom_Library" \
        else os.path.join(FPDIR, lib + ".pretty")
    fp = pcbnew.FootprintLoad(path, name)
    if fp is None:
        raise SystemExit("footprint %s not found" % fpid)
    fp.SetFPIDAsString(fpid)
    return fp


def build_pcb():
    board = pcbnew.BOARD()
    ds = board.GetDesignSettings()
    ds.SetCopperLayerCount(2)

    nets = {}

    def net(name):
        if name not in nets:
            ni = pcbnew.NETINFO_ITEM(board, name if name.startswith("unconnected") else "/" + name)
            board.Add(ni)
            nets[name] = ni
        return nets[name]
    for ref, part in PARTS.items():
        if part[1] is None:
            continue
        _blk, pins = lib_symbol(part[0])
        for num, (_x, _y, _a, pname) in pins.items():
            n = part[5].get(num)
            net(n if n else "unconnected-(%s-%s-Pad%s)" % (ref, pname, num))

    # Outline + slot
    def rect(x0, y0, x1, y1, layer, width=0.1):
        r = pcbnew.PCB_SHAPE(board)
        r.SetShape(pcbnew.SHAPE_T_RECT)
        r.SetStart(pt(x0, y0))
        r.SetEnd(pt(x1, y1))
        r.SetLayer(layer)
        r.SetWidth(mm(width))
        board.Add(r)
    rect(0, 0, BOARD_W, BOARD_H, pcbnew.Edge_Cuts)

    fps = {}
    for ref, (lib_id, fpid, value, pos, rot, pnets, _s) in PARTS.items():
        if fpid is None:
            continue
        fp = load_fp(fpid)
        fp.SetReference(ref)
        fp.SetValue(value)
        fp.SetPosition(pt(*pos))
        fp.SetOrientationDegrees(rot)
        fp.SetPath(pcbnew.KIID_PATH("/" + PARTS_UUID[ref]))
        if value.startswith("DNP"):
            fp.SetDNP(True)
        _blk, spins = lib_symbol(lib_id)
        for pad in fp.Pads():
            num = pad.GetNumber()
            n = pnets.get(num)
            if not n and num in spins:
                n = "unconnected-(%s-%s-Pad%s)" % (ref, spins[num][3], num)
            if n:
                pad.SetNet(nets[n])
        fp.Reference().SetVisible(ref in ("C1", "C2"))
        fp.Value().SetLayer(pcbnew.F_Fab)
        board.Add(fp)
        fps[ref] = fp

    def P(p):
        if isinstance(p, tuple):
            return pt(*p)
        ref, num = p.split(".")
        return fps[ref].FindPadByNumber(num).GetPosition()

    layer_id = {F: pcbnew.F_Cu, B: pcbnew.B_Cu}
    for net, layer, width, pts in TRACKS:
        for a, b in zip(pts, pts[1:]):
            t = pcbnew.PCB_TRACK(board)
            t.SetStart(P(a))
            t.SetEnd(P(b))
            t.SetWidth(mm(width))
            t.SetLayer(layer_id[layer])
            t.SetNet(nets[net])
            board.Add(t)
    for net, xy in VIAS:
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pt(*xy))
        v.SetDrill(mm(0.3))
        v.SetWidth(mm(0.6))
        v.SetNet(nets[net])
        board.Add(v)

    def text(s, xy, size, layer, rot=0):
        t = pcbnew.PCB_TEXT(board)
        t.SetText(s)
        t.SetPosition(pt(*xy))
        t.SetLayer({"F.SilkS": pcbnew.F_SilkS, "F.Fab": pcbnew.F_Fab, "B.SilkS": pcbnew.B_SilkS}[layer])
        t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
        t.SetTextThickness(mm(max(0.12, size * 0.15)))
        t.SetTextAngleDegrees(rot)
        if layer.startswith("B."):
            t.SetMirrored(True)
        board.Add(t)
        return t
    for s, xy, size, layer, rot in SILK:
        text(s, xy, size, layer, rot)
    for ref, labels in PIN_LABELS.items():
        side = -1 if ref == "J3" else 1          # J3: left of the pins, others: right
        for i, lab in enumerate(labels):
            p = fps[ref].FindPadByNumber(str(i + 1)).GetPosition()
            x = pcbnew.ToMM(p.x) - OX + side * 1.7
            y = pcbnew.ToMM(p.y) - OY
            t = text(lab, (x, y), 0.8, "F.SilkS")
            t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT if side > 0 else pcbnew.GR_TEXT_H_ALIGN_RIGHT)
    add_logo(board)
    for lab, x0, y0, x1, y1 in FAB:
        rect(x0, y0, x1, y1, pcbnew.F_Fab, 0.1)
        text(lab, ((x0 + x1) / 2, y0 + 1.0), 0.8, "F.Fab")

    # GND pours, both layers
    for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
        z = pcbnew.ZONE(board)
        z.SetLayer(layer)
        z.SetNet(nets["GND"])
        z.SetLocalClearance(mm(0.3))
        z.SetMinThickness(mm(0.25))
        z.SetThermalReliefGap(mm(0.4))
        z.SetThermalReliefSpokeWidth(mm(0.5))
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        ol = z.Outline()
        ol.NewOutline()
        for x, y in ((0, 0), (BOARD_W, 0), (BOARD_W, BOARD_H), (0, BOARD_H)):
            ol.Append(mm(OX + x), mm(OY + y))
        board.Add(z)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())

    out = os.path.join(HW, PROJECT + ".kicad_pcb")
    board.Save(out)


# BB logo on the bottom silkscreen, from tools/bb_logo.txt (1 = silk; made from
# tools/bb_logo.png at 0.05 mm per pixel). Pixel runs are merged into polygons.
LOGO = {"file": "bb_logo.txt", "center": (65.2, 12.0), "pixel": 0.05, "layer": "B.SilkS"}


def add_logo(board):
    rows = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), LOGO["file"])).read().split()
    h, w, p = len(rows), len(rows[0]), LOGO["pixel"]
    cx, cy = LOGO["center"]
    cx, cy = cx + OX, cy + OY
    mirror = LOGO["layer"].startswith("B.")
    ps = pcbnew.SHAPE_POLY_SET()
    for j, row in enumerate(rows):
        for m in re.finditer("1+", row):
            a, b = m.start(), m.end()
            if mirror:
                a, b = w - b, w - a
            x0, x1 = cx + (a - w / 2) * p, cx + (b - w / 2) * p
            y0, y1 = cy + (j - h / 2) * p, cy + (j + 1 - h / 2) * p
            ps.NewOutline()
            for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
                ps.Append(mm(x), mm(y))
    ps.Simplify()
    ps.Fracture()
    layer = {"B.SilkS": pcbnew.B_SilkS, "F.SilkS": pcbnew.F_SilkS}[LOGO["layer"]]
    for i in range(ps.OutlineCount()):
        sh = pcbnew.PCB_SHAPE(board)
        sh.SetShape(pcbnew.SHAPE_T_POLY)
        sh.SetPolyShape(pcbnew.SHAPE_POLY_SET(ps.COutline(i)))
        sh.SetFilled(True)
        sh.SetWidth(0)
        sh.SetLayer(layer)
        board.Add(sh)


# Stable symbol UUIDs (so re-running keeps the PCB <-> schematic link).
PARTS_UUID = {ref: str(uuid.uuid5(uuid.NAMESPACE_URL, "sm2k-carrier/" + ref)) for ref in PARTS}

if __name__ == "__main__":
    build_schematic()
    build_pcb()
    print("wrote", PROJECT + ".kicad_sch and .kicad_pcb in", HW)
