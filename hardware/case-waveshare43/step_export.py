"""Minimal STEP (AP214) writer for closed triangle meshes, no CAD kernel needed.

Coplanar, edge-connected triangles are merged into planar faces (with holes), so flat walls
come out as single faces in FreeCAD / Fusion 360; round parts stay faceted. Each mesh becomes
a coloured, named part of one assembly (identity placement, model coordinates in mm).
"""
import datetime
from collections import defaultdict, deque

import numpy as np


class _Writer:
    def __init__(self):
        self.lines = []

    def add(self, text):
        self.lines.append(text)
        return f"#{len(self.lines)}"

    def pt(self, p):
        return self.add(f"CARTESIAN_POINT('',({p[0]:.6f},{p[1]:.6f},{p[2]:.6f}))")

    def dir(self, d):
        return self.add(f"DIRECTION('',({d[0]:.9f},{d[1]:.9f},{d[2]:.9f}))")

    def axis(self, origin=(0, 0, 0), z=(0, 0, 1), x=(1, 0, 0)):
        return self.add(f"AXIS2_PLACEMENT_3D('',{self.pt(origin)},{self.dir(z)},{self.dir(x)})")


def _planar_groups(verts, tris):
    """Split triangles into edge-connected groups lying in one plane."""
    a, b, c = verts[tris[:, 0]], verts[tris[:, 1]], verts[tris[:, 2]]
    cross = np.cross(b - a, c - a)
    area2 = np.linalg.norm(cross, axis=1)
    normals = cross / np.maximum(area2, 1e-30)[:, None]
    offsets = np.einsum("ij,ij->i", normals, a)

    edge_tris = defaultdict(list)
    for t, (i, j, k) in enumerate(tris):
        for u, v in ((i, j), (j, k), (k, i)):
            edge_tris[(min(u, v), max(u, v))].append(t)

    group = -np.ones(len(tris), dtype=int)
    groups = []
    for seed in np.argsort(-area2):           # start from big triangles: their normals are exact
        if group[seed] >= 0:
            continue
        gid, n0, d0 = len(groups), normals[seed], offsets[seed]
        members, queue = [], deque([seed])
        group[seed] = gid
        while queue:
            t = queue.popleft()
            members.append(t)
            i, j, k = tris[t]
            for u, v in ((i, j), (j, k), (k, i)):
                for nb in edge_tris[(min(u, v), max(u, v))]:
                    if group[nb] < 0 and (area2[nb] < 1e-9 or
                                          (normals[nb] @ n0 > 1 - 1e-7 and abs(verts[tris[nb]] @ n0 - d0).max() < 1e-5)):
                        group[nb] = gid
                        queue.append(nb)
        groups.append((n0, members))
    return groups


def _loops(tris, members):
    """Boundary loops of a triangle group, as vertex index lists (mesh winding)."""
    directed = set()
    for t in members:
        i, j, k = tris[t]
        directed.update(((i, j), (j, k), (k, i)))
    nxt = defaultdict(list)
    for u, v in directed:
        if (v, u) not in directed:
            nxt[u].append(v)
    loops = []
    while any(nxt.values()):
        start = next(u for u, vs in nxt.items() if vs)
        loop, u = [start], nxt[start].pop()
        while u != start:
            loop.append(u)
            u = nxt[u].pop()
        loops.append(loop)
    return loops


def _solid(w, name, verts, tris):
    vpoints = {}

    def vertex(i):
        if i not in vpoints:
            vpoints[i] = w.add(f"VERTEX_POINT('',{w.pt(verts[i])})")
        return vpoints[i]

    edges = {}

    def oriented_edge(u, v):
        key = (min(u, v), max(u, v))
        if key not in edges:
            p, q = verts[key[0]], verts[key[1]]
            d = q - p
            length = float(np.linalg.norm(d))
            line = w.add(f"LINE('',{w.pt(p)},{w.add(f'VECTOR({chr(39)}{chr(39)},{w.dir(d / length)},{length:.6f})')})")
            edges[key] = w.add(f"EDGE_CURVE('',{vertex(key[0])},{vertex(key[1])},{line},.T.)")
        return w.add(f"ORIENTED_EDGE('',*,*,{edges[key]},{'.T.' if u == key[0] else '.F.'})")

    faces = []
    for normal, members in _planar_groups(verts, tris):
        loops = _loops(tris, members)
        if not loops:
            continue
        x_ref = np.cross(normal, [1, 0, 0] if abs(normal[0]) < 0.9 else [0, 1, 0])
        x_ref /= np.linalg.norm(x_ref)
        y_ref = np.cross(normal, x_ref)

        def signed_area(loop):
            p = verts[loop]
            x, y = p @ x_ref, p @ y_ref
            return 0.5 * float(np.sum(x * np.roll(y, -1) - np.roll(x, -1) * y))

        areas = [signed_area(l) for l in loops]
        outer = int(np.argmax(areas))
        bounds = []
        for idx, loop in enumerate(loops):
            oes = ",".join(oriented_edge(loop[k], loop[(k + 1) % len(loop)]) for k in range(len(loop)))
            el = w.add(f"EDGE_LOOP('',({oes}))")
            kind = "FACE_OUTER_BOUND" if idx == outer else "FACE_BOUND"
            bounds.append(w.add(f"{kind}('',{el},.T.)"))
        plane = w.add(f"PLANE('',{w.axis(verts[loops[outer][0]], normal, x_ref)})")
        faces.append(w.add(f"ADVANCED_FACE('',({','.join(bounds)}),{plane},.T.)"))
    shell = w.add(f"CLOSED_SHELL('',({','.join(faces)}))")
    return w.add(f"MANIFOLD_SOLID_BREP('{name}',{shell})"), len(faces)


def write_step(path, parts, assembly_name="assembly"):
    """parts: list of (name, vertices Nx3, triangles Mx3, (r, g, b) 0..1). Returns face counts."""
    w = _Writer()
    app_ctx = w.add("APPLICATION_CONTEXT('core data for automotive mechanical design processes')")
    w.add(f"APPLICATION_PROTOCOL_DEFINITION('international standard','automotive_design',2000,{app_ctx})")
    prod_ctx = w.add(f"PRODUCT_CONTEXT('',{app_ctx},'mechanical')")
    pd_ctx = w.add(f"PRODUCT_DEFINITION_CONTEXT('part definition',{app_ctx},'design')")
    mm = w.add("( LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT(.MILLI.,.METRE.) )")
    rad = w.add("( NAMED_UNIT(*) PLANE_ANGLE_UNIT() SI_UNIT($,.RADIAN.) )")
    sr = w.add("( NAMED_UNIT(*) SI_UNIT($,.STERADIAN.) SOLID_ANGLE_UNIT() )")
    unc = w.add(f"UNCERTAINTY_MEASURE_WITH_UNIT(LENGTH_MEASURE(1.E-05),{mm},'distance_accuracy_value','')")
    ctx = w.add("( GEOMETRIC_REPRESENTATION_CONTEXT(3) GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT(("
                f"{unc})) GLOBAL_UNIT_ASSIGNED_CONTEXT(({mm},{rad},{sr})) "
                "REPRESENTATION_CONTEXT('Context3D','3D') )")

    def product(name):
        prod = w.add(f"PRODUCT('{name}','{name}','',({prod_ctx}))")
        w.add(f"PRODUCT_RELATED_PRODUCT_CATEGORY('part',$,({prod}))")
        pdf = w.add(f"PRODUCT_DEFINITION_FORMATION('','',{prod})")
        pd = w.add(f"PRODUCT_DEFINITION('design','',{pdf},{pd_ctx})")
        pds = w.add(f"PRODUCT_DEFINITION_SHAPE('','',{pd})")
        return pd, pds

    top_pd, top_pds = product(assembly_name)
    top_axis = w.axis()
    top_rep = w.add(f"SHAPE_REPRESENTATION('{assembly_name}',({top_axis}),{ctx})")
    w.add(f"SHAPE_DEFINITION_REPRESENTATION({top_pds},{top_rep})")

    styled, counts = [], {}
    for n, (name, verts, tris, rgb) in enumerate(parts, 1):
        verts = np.asarray(verts, dtype=float)
        tris = np.asarray(tris, dtype=int)
        pd, pds = product(name)
        solid, counts[name] = _solid(w, name, verts, tris)
        axis = w.axis()
        rep = w.add(f"ADVANCED_BREP_SHAPE_REPRESENTATION('{name}',({axis},{solid}),{ctx})")
        w.add(f"SHAPE_DEFINITION_REPRESENTATION({pds},{rep})")
        # place the part in the assembly (identity)
        nauo = w.add(f"NEXT_ASSEMBLY_USAGE_OCCURRENCE('{n}','{name}','',{top_pd},{pd},$)")
        nauo_pds = w.add(f"PRODUCT_DEFINITION_SHAPE('','',{nauo})")
        idt = w.add(f"ITEM_DEFINED_TRANSFORMATION('','',{axis},{w.axis()})")
        rel = w.add(f"( REPRESENTATION_RELATIONSHIP('','',{rep},{top_rep}) "
                    f"REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION({idt}) SHAPE_REPRESENTATION_RELATIONSHIP() )")
        w.add(f"CONTEXT_DEPENDENT_SHAPE_REPRESENTATION({rel},{nauo_pds})")
        # colour
        col = w.add(f"COLOUR_RGB('',{rgb[0]:.3f},{rgb[1]:.3f},{rgb[2]:.3f})")
        fas = w.add(f"FILL_AREA_STYLE('',({w.add(f'FILL_AREA_STYLE_COLOUR({chr(39)}{chr(39)},{col})')}))")
        ssu = w.add(f"SURFACE_SIDE_STYLE('',({w.add(f'SURFACE_STYLE_FILL_AREA({fas})')}))")
        psa = w.add(f"PRESENTATION_STYLE_ASSIGNMENT(({w.add(f'SURFACE_STYLE_USAGE(.BOTH.,{ssu})')}))")
        styled.append(w.add(f"STYLED_ITEM('color',({psa}),{solid})"))
    w.add(f"MECHANICAL_DESIGN_GEOMETRIC_PRESENTATION_REPRESENTATION('',({','.join(styled)}),{ctx})")

    stamp = datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S")
    with open(path, "w") as f:
        f.write("ISO-10303-21;\nHEADER;\n")
        f.write(f"FILE_DESCRIPTION(('{assembly_name}'),'2;1');\n")
        f.write(f"FILE_NAME('{assembly_name}.step','{stamp}',(''),(''),'case.py','case.py','');\n")
        f.write("FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\nENDSEC;\nDATA;\n")
        for i, line in enumerate(w.lines, 1):
            f.write(f"#{i}={line};\n")
        f.write("ENDSEC;\nEND-ISO-10303-21;\n")
    return counts
