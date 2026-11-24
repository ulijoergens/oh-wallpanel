#!/usr/bin/env python3
"""Desk stand for the wall case (case.py): the same frame, back cover and wall plate hook onto a
tilted board that stands in for the wall.

  desk_stand.stl  tilted board (the frame's rim rests on it like on a wall), base ring and two
                  rear legs; the wall plate is bolted to the board with 2 x M3x10 + nuts through
                  its horizontal arc slots; the cable runs through the board, drops into the
                  hollow stand and leaves through a gap in the rear rail. Two holes in the
                  base's front strip give a screwdriver access to the frame's bottom screws
                  (optional on a desk: gravity keeps the unit on the hooks). Print base down,
                  as exported (no supports: the board leans back TILT deg, the legs ~53 deg,
                  the cable hole has a pointed top).

Coordinates: panel coordinates as in case.py (x right, y up the panel, z out of the panel, the
plate's back = the board's front at z = 0); desk coordinates X = x, Y backwards, Z up.
"""
import os
import sys

import numpy as np
from manifold3d import CrossSection, Manifold

import case as c
from step_export import write_step

TILT = 20.0             # panel leans back this far from vertical
BOARD_T = 4.0
LIFT = 1.5              # gap between the frame's lowest edge and the base
BASE_T = 3.0
BASE_HALF_W = 50.0      # base ring and legs: X = +-BASE_HALF_W
RAIL = 10.0             # base ring rails
FRONT_TOE = 7.0         # base reaches this far in front of the frame's front edge
REAR = 65.0             # base reaches back to Y = REAR (stability when the screen is touched)
LEG_W = 12.0            # rear legs (X)
LEG_Y = (18.0, 30.0)    # legs meet the board back between these panel y values
CABLE_GAP = 14.0        # gap in the rear rail for the cable
M3_CLEAR = 3.4
SIDE_WIN = (44.0, 60.0, -26.0, 28.0)   # material-saving windows in the board (|x| range, y range)
ACCESS_D = 10.0         # screwdriver holes in the front strip

T = np.radians(TILT)
ROT = 90.0 - TILT       # rotate panel -> desk about X


def to_desk(m):
    return m.rotate([ROT, 0, 0])


def desk_pt(y, z):
    """Panel (y, z) -> desk (Y, Z)."""
    return y * np.sin(T) - z * np.cos(T), y * np.cos(T) + z * np.sin(T)


def pointed_hole(d, z0, z1):
    """Round hole along z with a 45 deg pointed top (+y): prints without support when tilted."""
    r = d / 2
    return Manifold.hull(c.cyl(0, 0, z0, z1, d) + c.cyl(0, r * np.sqrt(2) - 0.1, z0, z1, 0.2))


def make_stand():
    # frame's lowest point decides the desk height
    z_frame_low = min(desk_pt(-c.OUT_H / 2, z)[1] for z in (0.0, c.DEPTH))
    Z_TOP = z_frame_low - LIFT                     # top of the base
    Z_DESK = Z_TOP - BASE_T
    y_front = desk_pt(-c.OUT_H / 2, c.DEPTH)[0] - FRONT_TOE

    # board (panel coords): the frame outline, extended downwards into the base, cut by the desk
    board = c.rbox(-c.OUT_W / 2, -c.OUT_H / 2 - 30, -BOARD_T, c.OUT_W / 2, c.OUT_H / 2, 0, c.R_OUT)
    board -= pointed_hole(c.BOX_HOLE_D, -BOARD_T - 1, 1)
    for sx in (-1, 1):
        board -= c.cyl(sx * c.BOX_PITCH / 2, 0, -BOARD_T - 1, 1, M3_CLEAR)
        x0, x1, y0, y1 = SIDE_WIN
        win = c.box(x0, y0, -BOARD_T - 1, x1, y1 - (x1 - x0) / 2, 1)
        win += Manifold.extrude(c.poly([(x0, y1 - (x1 - x0) / 2), (x1, y1 - (x1 - x0) / 2), ((x0 + x1) / 2, y1)]),
                                BOARD_T + 2).translate([0, 0, -BOARD_T - 1])
        board -= win if sx > 0 else win.mirror([1, 0, 0])
    stand = to_desk(board)

    # base ring (desk coords)
    w = BASE_HALF_W
    y_board_back = desk_pt((Z_DESK + BOARD_T * np.sin(T)) / np.cos(T), -BOARD_T)[0]
    front_strip_end = y_board_back + 3.0
    base = c.box(-w, y_front, Z_DESK, w, REAR, Z_TOP)
    base -= c.box(-w + RAIL, front_strip_end, Z_DESK - 1, w - RAIL, REAR - RAIL, Z_TOP + 1)
    base -= c.box(-CABLE_GAP / 2, REAR - RAIL - 1, Z_DESK - 1, CABLE_GAP / 2, REAR + 1, Z_TOP + 1)
    # screwdriver access to the frame's bottom screws: where their axes meet the base
    y_axis = (Z_TOP - c.SCREW_Z * np.sin(T)) / np.cos(T)
    acc_y, _ = desk_pt(y_axis, c.SCREW_Z)
    for sx in (-1, 1):
        base -= c.cyl(sx * c.TAB_X, acc_y, Z_DESK - 1, Z_TOP + 1, ACCESS_D)
    stand += base

    # rear legs: from the board's back (panel y in LEG_Y) down to the rear rail
    a = desk_pt(LEG_Y[1], -BOARD_T + 0.5)
    b = desk_pt(LEG_Y[0], -BOARD_T + 0.5)
    prof = [b, a, (REAR, Z_DESK - 1.0), (REAR - RAIL - 2.0, Z_DESK - 1.0)]   # cut flat below
    for sx in (-1, 1):
        x0 = sx * w - (LEG_W if sx > 0 else 0)
        stand += Manifold.extrude(c.poly(prof), LEG_W).rotate([90, 0, 90]).translate([x0, 0, 0])

    # flat on the desk
    stand ^= c.box(-200, -200, Z_DESK, 200, 200, 200)
    return stand, Z_DESK, acc_y


def main():
    out = os.path.dirname(os.path.abspath(__file__))
    stand, z_desk, acc_y = make_stand()
    frame, cover, plate = c.make_frame(), c.make_cover(), c.make_plate()
    board, plug = c.board_dummy(), c.plug_dummy()
    unit = {"frame": frame, "cover": cover, "board": board, "plug": plug}

    checks = {f"stand/{k}": (stand ^ to_desk(m)).volume() for k, m in unit.items()}
    checks["stand/plate"] = (stand ^ to_desk(plate)).volume()
    up = [0, c.SLIDE, 0]
    for k, m in unit.items():
        checks[f"install: stand/{k}"] = (stand ^ to_desk(m.translate(up))).volume()
    bad = {k: v for k, v in checks.items() if v > 1e-6}
    n = len(stand.decompose())
    if n != 1:
        bad["stand not one piece"] = n

    t = c.to_trimesh(stand.translate([0, 0, -z_desk]))
    t.export(os.path.join(out, "desk_stand.stl"))
    ext = t.bounds[1] - t.bounds[0]
    print(f"desk_stand.stl: {ext[0]:.1f} x {ext[1]:.1f} x {ext[2]:.1f} mm, volume {t.volume / 1000:.1f} cm3, "
          f"watertight {t.is_watertight}, {len(t.faces)} triangles")

    asm_parts = (("desk_stand", stand, (0.55, 0.55, 0.58)), ("wall_plate", to_desk(plate), (0.95, 0.55, 0.15)),
                 ("back_cover", to_desk(cover), (0.20, 0.45, 0.80)), ("frame", to_desk(frame), (0.92, 0.92, 0.90)),
                 ("board_dummy", to_desk(board), (0.10, 0.45, 0.25)), ("usb_plug_dummy", to_desk(plug), (0.15, 0.15, 0.15)))
    asm = []
    for name, m, rgb in asm_parts:
        m = m.translate([0, 0, -z_desk])
        mesh = m.to_mesh()
        asm.append((name, np.asarray(mesh.vert_properties)[:, :3], np.asarray(mesh.tri_verts), rgb))
    faces = write_step(os.path.join(out, "desk_assembly.step"), asm, "wallpanel_desk")
    print("desk_assembly.step: " + ", ".join(f"{k} {v} faces" for k, v in faces.items()))
    top = max(m.translate([0, 0, -z_desk]).bounding_box()[5] for _, m, _ in asm_parts)
    lo = min(m.bounding_box()[1] for _, m, _ in asm_parts)
    hi = max(m.bounding_box()[4] for _, m, _ in asm_parts)
    print(f"assembled on the desk: {c.OUT_W:.0f} wide x {hi - lo:.0f} deep x {top:.0f} high, tilt {TILT:.0f} deg; "
          f"screwdriver holes at Y {acc_y:.1f}")
    for k, v in checks.items():
        print(f"  clash {k}: {v:.3f} mm3")
    print(f"  desk_stand: {n} piece(s)")
    print("clash check: " + ("OK" if not bad else "FAILED " + ", ".join(map(str, bad))))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
