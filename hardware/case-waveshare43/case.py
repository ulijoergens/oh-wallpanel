#!/usr/bin/env python3
"""Wall case for the Waveshare ESP32-S3-Touch-LCD-4.3 (touch version), centred over a round
60 mm flush wall box, powered by a side-angled (left/right) 90 degree USB-C plug.

Three parts, all printable without supports:
  wall_plate.stl  screws to the wall box (60 mm screw pitch, horizontal or vertical, arc slots
                  allow levelling +-12 deg); two hooks at the top, two screw tabs at the bottom;
                  print back face down
  frame.stl       front: face with the display window (45 deg chamfer), locating ribs around the
                  glass, side pockets (the right one takes the USB plug), four corner blocks for
                  the cover screws, vent slots in the top and bottom walls; its walls reach the
                  wall and hide everything; print face down (as exported)
  back_cover.stl  screwed into the frame with 4 screws; four posts press the PCB at its mounting
                  holes (pins locate it) against the face; central opening for the cable and the
                  box screws, windows for the plate hooks, notches for the plate tabs;
                  print back face down (as exported), posts up

Assembly: panel face down into the frame (optional 0.5 mm felt/foam strip on the face around the
window), plug the USB-C cable in, lead it through the cover's opening, screw the cover on
(4 x M3x8 pan head for plastic). Screw the plate to the box. Hold the unit SLIDE mm above its
final place, push it against the wall (the hooks go through the cover windows), slide it down,
lock it with 2 x M3x10 screws from below.

Coordinates: front view, x to the right, y up, z out of the wall (z = 0 on the wall), centre of
the panel and the wall box at x = y = 0. All sizes in mm. Data: Waveshare dimension drawing
"Esp32-s3-touch-lcd-4.3-003.jpg" (touch version). Run:
    python3 case.py            (needs manifold3d, numpy, trimesh)
"""
import os
import sys

import numpy as np
from manifold3d import CrossSection, JoinType, Manifold
import trimesh

from step_export import write_step

# ---------------------------------------------------------------- the board (Waveshare drawing)
GLASS_W, GLASS_H = 106.10, 67.80   # touch glass, the front of the module
PCB_W, PCB_H = 106.0, 68.0         # PCB, same centre as the glass
STACK = 8.8                        # glass front to PCB back face (side view "8.80")
PCB_T = 1.6
ACT_W, ACT_H = 95.54, 54.36        # active area
ACT_RIGHT = 5.33                   # glass right edge (front view) to the active area
ACT_BOTTOM = 9.09                  # glass bottom edge to the active area (the wide bezel)
HOLE_DX, HOLE_DY = 49.0, 30.0      # mounting holes 98 x 60, 4 mm from the PCB edges
HOLE_D = 3.0                       # NOT on the drawing: measure (M2.5 boards: 2.7 .. 3.2)
USB_Y = PCB_H / 2 - 32.90          # "USB" port (native USB) centre; right edge in the front view
UART_Y = PCB_H / 2 - 12.93         # "UART" port centre (not used, same edge)
TAILS = ((-36.0, -18.0), (21.0, 44.0))   # x ranges where the display flex cables wrap around
TAIL_OUT = 2.2                     # how far they stick out below the bottom glass edge
BACK_CLEAR = 9.5                   # free space behind the PCB (jumpers on the CAN/RS485 headers
                                   # ~8.5 mm, PH connectors ~6 mm): measure the tallest part

# ---------------------------------------------------------------- the plug (side-angled USB-C)
PLUG_X = 11.7                      # head: from the PCB edge outwards
PLUG_UP = 6.5                      # head reaches this far above the port centre
PLUG_DOWN = 22.0                   # head + strain relief below the port centre (cable goes down)
PLUG_T = 6.8                       # head thickness (front to back)
PORT_Z = 1.7                       # port centre behind the PCB back face

# ---------------------------------------------------------------- case
OUT_W, OUT_H = 136.0, 80.0         # outside of the frame
R_OUT = 5.0                        # corner radius
WALL = 2.4                         # frame walls
FACE_T = 1.6                       # face in front of the glass
FOAM = 0.5                         # gap glass <-> face for a felt / foam strip (0 = none)
FIT = 0.3                          # clearance around the glass, cover in frame
WIN_MARGIN = 0.6                   # window larger than the active area, each side
WIN_CHAMFER = 1.0                  # 45 deg chamfer on the front edge of the window
RIB_T, RIB_DEPTH = 1.2, 3.5        # ribs around the glass (locate it)
BOTTOM_RIBS = ((-52.0, -40.0), (-12.0, 12.0), (48.0, 52.0))   # between the flex cables

PLATE_T = 2.5
PLATE_W = 80.0
COVER_GAP = 0.5                    # plate front to cover back (cover screw heads live beside the plate)
COVER_T = 2.4
SLIDE = 2.6                        # the unit hooks on by sliding down this far
BOX_PITCH = 60.0                   # wall box screw pitch (French / EU round boxes)
BOX_SLOT_W = 3.8                   # for the box's M3 / M3.5 screws
BOX_SLOT_ARC = 12.0                # +- degrees of rotation for levelling
BOX_HEAD_D = 7.5                   # box screw head (clearance in the cover)
BOX_HOLE_D = 46.0                  # cable hole into the box
HOOK_X, HOOK_W = 25.0, 10.0        # two hooks at x = +-HOOK_X
HOOK_Y = 31.0                      # hook post (2 mm thick) starts here
HOOK_REACH = 2.4                   # head reaches over the cover this far (45 deg underside)
HOOK_PLAY = 0.1
TAB_X, TAB_W, TAB_H, TAB_Z = 20.0, 12.0, 5.0, 10.0   # bottom screw tabs on the plate
SCREW_Z = 6.7                      # bottom screws (M3x10) axis height
PILOT_D = 2.5                      # pilot holes for M3 screws for plastic
CLEAR_D = 3.4
CORNER = (8.0, 7.6)                # frame corner blocks (x, y) for the cover screws
POST_D = 6.0
PIN_D, PIN_H = 2.2, 1.2            # locating pins, go into the standoffs' bores
STANDOFF_H = 4.0                   # standoffs fitted in the PCB holes, sticking out behind the PCB (measured ~4)
STANDOFF_D = 5.8                   # across the corners of the hex standoff (dummy only)
VENT_W, VENT_Z0, VENT_Z1 = 1.6, 6.5, 13.5
VENT_X = (-50, -40, -30, -10, 0, 10, 30, 40, 50)

SEG = 64

# ---------------------------------------------------------------- derived
IN_W, IN_H = OUT_W - 2 * WALL, OUT_H - 2 * WALL
R_IN = R_OUT - WALL
Z_PLATE1 = PLATE_T
Z_COVER0 = PLATE_T + COVER_GAP
Z_COVER1 = Z_COVER0 + COVER_T
Z_PCB = Z_COVER1 + BACK_CLEAR          # PCB back face
Z_GLASS = Z_PCB + STACK                # glass front
Z_FACE0 = Z_GLASS + FOAM               # back of the face
DEPTH = Z_FACE0 + FACE_T
Z_PORT = Z_PCB - PORT_Z
GX, GY = GLASS_W / 2, GLASS_H / 2
ACT_X1 = GX - ACT_RIGHT
ACT_X0 = ACT_X1 - ACT_W
ACT_Y0 = -GY + ACT_BOTTOM
ACT_Y1 = ACT_Y0 + ACT_H
# plate: snug under the top wall, room for the slide at the bottom
PLATE_Y1 = IN_H / 2 - 0.4
PLATE_Y0 = -IN_H / 2 + SLIDE + 0.4
COVER_SCREWS = [(sx * (IN_W / 2 - CORNER[0] / 2), sy * (IN_H / 2 - CORNER[1] / 2 + 0.3))
                for sx in (-1, 1) for sy in (-1, 1)]


def box(x0, y0, z0, x1, y1, z1):
    x0, x1 = sorted((x0, x1))
    y0, y1 = sorted((y0, y1))
    z0, z1 = sorted((z0, z1))
    return Manifold.cube([x1 - x0, y1 - y0, z1 - z0]).translate([x0, y0, z0])


def cyl(x, y, z0, z1, d, seg=SEG):
    return Manifold.cylinder(z1 - z0, d / 2, d / 2, seg).translate([x, y, z0])


def cyl_y(x, z, y0, y1, d):
    """Cylinder along y."""
    return Manifold.cylinder(y1 - y0, d / 2, d / 2, SEG).rotate([-90, 0, 0]).translate([x, y0, z])


def rrect(x0, y0, x1, y1, r):
    return CrossSection.square([x1 - x0 - 2 * r, y1 - y0 - 2 * r]).translate([x0 + r, y0 + r]).offset(
        r, JoinType.Round, 2, SEG) if r > 0 else CrossSection.square([x1 - x0, y1 - y0]).translate([x0, y0])


def rbox(x0, y0, z0, x1, y1, z1, r):
    return Manifold.extrude(rrect(x0, y0, x1, y1, r), z1 - z0).translate([0, 0, z0])


def poly(pts):
    """CrossSection from a polygon in any orientation."""
    a = sum(pts[i][0] * pts[i - 1][1] - pts[i - 1][0] * pts[i][1] for i in range(len(pts)))
    return CrossSection([pts if a < 0 else pts[::-1]])


def prism_yz(pts, x0, x1):
    """Prism with the cross-section pts (list of (y, z)) from x0 to x1."""
    return Manifold.extrude(poly(pts), x1 - x0).rotate([90, 0, 90]).translate([x0, 0, 0])


def window_cut(x0, y0, x1, y1, z0, z1, chamfer):
    """Window through z0..z1 with a 45 deg chamfer that opens towards z1."""
    straight = box(x0, y0, z0 - 1, x1, y1, z1 + 1)
    zc = z1 - chamfer
    frustum = Manifold.hull(box(x0, y0, zc, x1, y1, zc + 0.01) +
                            box(x0 - chamfer - 0.5, y0 - chamfer - 0.5, z1 + 0.5, x1 + chamfer + 0.5, y1 + chamfer + 0.5, z1 + 0.51))
    return straight + frustum


# ---------------------------------------------------------------- frame
def make_frame():
    shell = rbox(-OUT_W / 2, -OUT_H / 2, 0, OUT_W / 2, OUT_H / 2, DEPTH, R_OUT)
    shell -= rbox(-IN_W / 2, -IN_H / 2, -1, IN_W / 2, IN_H / 2, Z_FACE0, R_IN)
    # display window, chamfered towards the front
    m = WIN_MARGIN
    shell -= window_cut(ACT_X0 - m, ACT_Y0 - m, ACT_X1 + m, ACT_Y1 + m, Z_FACE0, DEPTH, WIN_CHAMFER)

    # ribs around the glass
    z0, z1 = Z_FACE0 - RIB_DEPTH, Z_FACE0 + 0.01
    gx, gy = GX + FIT, GY + FIT
    ribs = box(-gx - RIB_T, -gy - RIB_T, z0, -gx, gy + RIB_T, z1)            # left
    ribs += box(gx, -gy - RIB_T, z0, gx + RIB_T, gy + RIB_T, z1)             # right
    ribs += box(-gx, gy, z0, gx, gy + RIB_T, z1)                             # top
    for x0, x1 in BOTTOM_RIBS:                                               # bottom, between the flex cables
        ribs += box(x0, -gy - RIB_T, z0, x1, -gy, z1)
    shell += ribs

    # corner blocks for the cover screws (from the cover's front face to the face)
    cx, cy = CORNER
    for sx in (-1, 1):
        for sy in (-1, 1):
            xa, xb = sorted((sx * (IN_W / 2 + 0.5), sx * (IN_W / 2 - cx)))
            ya, yb = sorted((sy * (IN_H / 2 + 0.5), sy * (IN_H / 2 - cy)))
            shell += box(xa, ya, Z_COVER1, xb, yb, Z_FACE0 + 0.01)
    for x, y in COVER_SCREWS:
        shell -= cyl(x, y, Z_COVER1 - 0.01, Z_COVER1 + 12, PILOT_D)

    # bottom screws (into the plate tabs)
    for sx in (-1, 1):
        shell -= cyl_y(sx * TAB_X, SCREW_Z, -OUT_H / 2 - 1, -IN_H / 2 + 0.5, CLEAR_D)

    # vent slots in the top and bottom walls (along z: no bridges when printed face down)
    for x in VENT_X:
        if any(abs(x - sx * TAB_X) < 6 for sx in (-1, 1)):
            continue
        for ys in (-1, 1):
            shell -= box(x - VENT_W / 2, ys * (OUT_H / 2 + 1), VENT_Z0, x + VENT_W / 2, ys * (IN_H / 2 - 0.5), VENT_Z1)
    return shell


# ---------------------------------------------------------------- back cover (modelled in place)
def make_cover():
    f = FIT
    cover = rbox(-IN_W / 2 + f, -IN_H / 2 + f, Z_COVER0, IN_W / 2 - f, IN_H / 2 - f, Z_COVER1, max(R_IN - f, 0.5))
    z0, z1 = Z_COVER0 - 1, Z_COVER1 + 1
    # central opening: cable, box screws (heads), plug passes through
    cover -= rbox(-42, -20, z0, 42, 27, z1, 6)     # bottom edge stays: the notches below are open at the edge
    hd = BOX_HEAD_D / 2 + 0.5
    r = BOX_PITCH / 2
    cover -= box(-hd, r - hd - SLIDE, z0, hd, r + hd, z1)                  # top box screw (vertical boxes)
    cover -= box(-hd, -IN_H, z0, hd, -r + hd, z1)                          # bottom box screw
    # windows for the hooks: the hook passes through with the unit SLIDE higher, then the cover
    # rim above the window slides under the hook head
    for sx in (-1, 1):
        x = sx * HOOK_X
        cover -= box(x - HOOK_W / 2 - 0.3, HOOK_Y - HOOK_PLAY - SLIDE - 0.3, z0,
                     x + HOOK_W / 2 + 0.3, HOOK_Y + 2 + HOOK_PLAY, z1)
    # notches for the plate's screw tabs (open at the bottom edge)
    for sx in (-1, 1):
        x = sx * TAB_X
        cover -= box(x - TAB_W / 2 - 0.3, -IN_H, z0, x + TAB_W / 2 + 0.3, PLATE_Y0 + TAB_H + SLIDE + 0.3, z1)
    # screw holes for the cover screws (heads on the wall side, beside the plate)
    for x, y in COVER_SCREWS:
        cover -= cyl(x, y, z0, z1, CLEAR_D)
    # posts with locating pins at the PCB holes
    for sx in (-1, 1):
        for sy in (-1, 1):
            x, y = sx * HOLE_DX, sy * HOLE_DY
            zt = Z_PCB - STANDOFF_H                     # posts press on the standoffs' ends
            cover += cyl(x, y, Z_COVER1 - 0.01, zt, POST_D)
            pin = cyl(x, y, zt - 0.01, zt + PIN_H - 0.3, PIN_D)
            pin += Manifold.cylinder(0.3, PIN_D / 2, PIN_D / 2 - 0.3, SEG).translate([x, y, zt + PIN_H - 0.3])
            cover += pin
    return cover


# ---------------------------------------------------------------- wall plate
def arc_slot(r, a_mid, half_deg, w, z0, z1):
    """Slot along an arc of radius r around the box centre."""
    pts_out, pts_in = [], []
    for a in np.linspace(np.radians(a_mid - half_deg), np.radians(a_mid + half_deg), 24):
        pts_out.append(((r + w / 2) * np.cos(a), (r + w / 2) * np.sin(a)))
        pts_in.append(((r - w / 2) * np.cos(a), (r - w / 2) * np.sin(a)))
    cs = poly(pts_out + pts_in[::-1])
    m = Manifold.extrude(cs, z1 - z0).translate([0, 0, z0])
    for a in (a_mid - half_deg, a_mid + half_deg):
        m += cyl(r * np.cos(np.radians(a)), r * np.sin(np.radians(a)), z0, z1, w)
    return m


def make_plate():
    plate = rbox(-PLATE_W / 2, PLATE_Y0, 0, PLATE_W / 2, PLATE_Y1, PLATE_T, 3.0)
    plate -= cyl(0, 0, -1, PLATE_T + 1, BOX_HOLE_D)
    for a in (0, 90, 180, 270):
        plate -= arc_slot(BOX_PITCH / 2, a, BOX_SLOT_ARC, BOX_SLOT_W, -1, PLATE_T + 1)

    # hooks: post + head with a 45 deg underside (no overhang), the head reaches over the cover
    zu = Z_COVER1 + HOOK_PLAY                     # underside of the head at the post
    zt = zu + HOOK_REACH + 0.6
    prof = [(HOOK_Y, PLATE_T - 0.01), (HOOK_Y + 2, PLATE_T - 0.01), (HOOK_Y + 2, zu),
            (HOOK_Y + 2 + HOOK_REACH, zu + HOOK_REACH), (HOOK_Y + 2 + HOOK_REACH, zt), (HOOK_Y, zt)]
    for sx in (-1, 1):
        x = sx * HOOK_X
        plate += prism_yz(prof, x - HOOK_W / 2, x + HOOK_W / 2)

    # screw tabs at the bottom edge
    for sx in (-1, 1):
        x = sx * TAB_X
        plate += box(x - TAB_W / 2, PLATE_Y0, PLATE_T - 0.01, x + TAB_W / 2, PLATE_Y0 + TAB_H, TAB_Z)
        plate -= cyl_y(x, SCREW_Z, PLATE_Y0 - 1, PLATE_Y0 + TAB_H - 0.8, PILOT_D)
    return plate


# ---------------------------------------------------------------- board dummy (clash check, preview)
def board_dummy():
    b = box(-GX, -GY, Z_PCB + PCB_T, GX, GY, Z_GLASS)                       # LCD + glass
    pcb = box(-PCB_W / 2, -PCB_H / 2, Z_PCB, PCB_W / 2, PCB_H / 2, Z_PCB + PCB_T)
    for sx in (-1, 1):
        for sy in (-1, 1):
            pcb -= cyl(sx * HOLE_DX, sy * HOLE_DY, Z_PCB - 1, Z_PCB + PCB_T + 1, HOLE_D)
            so = cyl(sx * HOLE_DX, sy * HOLE_DY, Z_PCB - STANDOFF_H, Z_PCB + 0.01, STANDOFF_D)
            pcb += so - cyl(sx * HOLE_DX, sy * HOLE_DY, Z_PCB - STANDOFF_H - 1, Z_PCB + 1, PIN_D + 0.2)
    b += pcb
    for x0, x1 in TAILS:                                                      # flex cables round the bottom edge
        b += box(x0, -GY - TAIL_OUT, Z_PCB - 1.0, x1, -GY + 0.01, Z_GLASS - 1.5)
    # parts on the back: low parts everywhere (not at the holes), the tall ones where they are
    low = box(-PCB_W / 2 + 1, -PCB_H / 2 + 1, Z_PCB - 3.5, PCB_W / 2 - 1, PCB_H / 2 - 1, Z_PCB + 0.01)
    for sx in (-1, 1):
        for sy in (-1, 1):
            low -= cyl(sx * HOLE_DX, sy * HOLE_DY, Z_PCB - 5, Z_PCB + 1, POST_D + 1.0)
    b += low
    for x in (53 - 19.30, 53 - 46.33):                                          # jumpers on CAN / RS485 headers
        b += box(x - 2.6, -PCB_H / 2 + 10.39 - 1.5, Z_PCB - 8.5, x + 2.6, -PCB_H / 2 + 10.39 + 1.5, Z_PCB)
    b += box(-PCB_W / 2, -10, Z_PCB - 6.0, -PCB_W / 2 + 7.5, PCB_H / 2 - 12, Z_PCB)   # PH connectors, left edge
    b += box(-PCB_W / 2, -PCB_H / 2 + 14, Z_PCB - 6.0, -PCB_W / 2 + 7.5, -PCB_H / 2 + 24, Z_PCB)  # battery
    for y in (USB_Y, UART_Y):                                                 # USB-C sockets
        b += box(PCB_W / 2 - 7.4, y - 4.5, Z_PCB - 3.3, PCB_W / 2 + 0.3, y + 4.5, Z_PCB)
    return b


def plug_dummy():
    x0 = PCB_W / 2 + 0.3
    return box(x0, USB_Y - PLUG_DOWN, Z_PORT - PLUG_T / 2, x0 + PLUG_X, USB_Y + PLUG_UP, Z_PORT + PLUG_T / 2)


# ---------------------------------------------------------------- output
def to_trimesh(m):
    mesh = m.to_mesh()
    return trimesh.Trimesh(vertices=np.asarray(mesh.vert_properties)[:, :3], faces=np.asarray(mesh.tri_verts))


def main():
    out = os.path.dirname(os.path.abspath(__file__))
    frame, cover, plate = make_frame(), make_cover(), make_plate()
    board, plug = board_dummy(), plug_dummy()

    checks = {
        "board/frame": (board ^ frame).volume(),
        "board/cover": (board ^ cover).volume(),
        "board/plate": (board ^ plate).volume(),
        "plug/frame": (plug ^ frame).volume(),
        "plug/cover": (plug ^ cover).volume(),
        "plug/board": (plug ^ board).volume(),
        "cover/frame": (cover ^ frame).volume(),
        "plate/frame": (plate ^ frame).volume(),
        "plate/cover": (plate ^ cover).volume(),
    }
    # installing: the unit (frame + cover + board) SLIDE mm higher must pass the plate's hooks
    up = [0, SLIDE, 0]
    checks["install: plate/cover"] = (plate ^ cover.translate(up)).volume()
    checks["install: plate/frame"] = (plate ^ frame.translate(up)).volume()
    checks["install: plate/board"] = (plate ^ board.translate(up)).volume()

    parts = {
        "frame": frame.rotate([180, 0, 0]).translate([0, 0, DEPTH]),           # face down
        "back_cover": cover.translate([0, 0, -Z_COVER0]),                       # back down, posts up
        "wall_plate": plate,                                                    # back down
    }
    for name, m in parts.items():
        t = to_trimesh(m)
        t.export(os.path.join(out, name + ".stl"))
        ext = t.bounds[1] - t.bounds[0]
        print(f"{name}.stl: {ext[0]:.1f} x {ext[1]:.1f} x {ext[2]:.1f} mm, volume {t.volume / 1000:.1f} cm3, "
              f"watertight {t.is_watertight}, {len(t.faces)} triangles")
    to_trimesh(board).export(os.path.join(out, "preview_board.stl"))
    to_trimesh(plug).export(os.path.join(out, "preview_plug.stl"))

    asm = [(n, np.asarray(m.to_mesh().vert_properties)[:, :3], np.asarray(m.to_mesh().tri_verts), rgb)
           for n, m, rgb in (("frame", frame, (0.92, 0.92, 0.90)), ("back_cover", cover, (0.20, 0.45, 0.80)),
                             ("wall_plate", plate, (0.95, 0.55, 0.15)), ("board_dummy", board, (0.10, 0.45, 0.25)),
                             ("usb_plug_dummy", plug, (0.15, 0.15, 0.15)))]
    faces = write_step(os.path.join(out, "case_assembly.step"), asm, "wallpanel_case")
    print("case_assembly.step: " + ", ".join(f"{k} {v} faces" for k, v in faces.items()))
    print(f"outside {OUT_W:.0f} x {OUT_H:.0f} x {DEPTH:.1f} mm; PCB back at z {Z_PCB:.1f}, glass front at {Z_GLASS:.1f}")
    bad = {k: v for k, v in checks.items() if v > 1e-6}
    for name, m in (("frame", frame), ("back_cover", cover), ("wall_plate", plate)):
        n = len(m.decompose())
        print(f"  {name}: {n} piece(s)")
        if n != 1:
            bad[name + " not one piece"] = n
    for k, v in checks.items():
        print(f"  clash {k}: {v:.3f} mm3")
    print("clash check: " + ("OK" if not bad else "FAILED " + ", ".join(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
