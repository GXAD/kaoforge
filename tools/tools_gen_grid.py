#!/usr/bin/env python3
"""Scene generator for Kao2 .ar files (version 0x89): a checkered floor, cubes, barrels and props (every mesh has collision).
Structure (modelled on the game's own files, see docs/ENGINE.md):
  $root (eScene) -> global_env (eEnvironment) -> #active_group (eGroup) -> Ambient01 (eAmbientLight)
                                             -> #VS-grid/1 (eGroup, bit 0x10000000 = drawn) -> meshes (eTriMesh + eALBox)
The scene info MUST contain ".COMPILED." - otherwise the game compiles the scene on load (FUN_00413d20), and the
convertProxiesToMX step (FUN_00429200) opens a developer-only file objects/px_simple_occ.TXT that the Steam build
does not have -> "invalid magic" error.
Usage: tools_gen_grid.py output.ar [spec.json]   (normally driven by kaoforge.py build)
(Kao spawns at 0,0,0 - the floor must be BELOW it; a start exactly on the plane falls through)"""
import sys, struct, math, os
sys.setrecursionlimit(10000)

VERSION = 0x89
T = {'eScene': 0x8001, 'eEnvironment': 0x3f800001, 'eGroup': 0x8111, 'eTransform': 0x8112,
     'eAmbientLight': 0x3454503, 'eDirectionalLight': 0x3454501, 'eTriMesh': 0x3005, 'eGeoSet': 0x3004, 'eMaterial': 0x1003,
     'eTexture': 0x1002, 'eBitmap': 0x1001, 'eMaterialState': 0x1005, 'eAnimState': 0xac000001,
     'eALBox': 0x1231325, 'abb': 0xeeeeeee4, 'u16': 0xeeeeeee0, 'p2': 0xeeeeeee1, 'p4': 0xeeeeeee3, '3fx': 0xeeeeeedf}


class W:
    def __init__(self):
        self.b = bytearray()
        self.n = 0                      # registration counter (objects, strings, tree nodes, 3fX arrays)

    def u32(self, v): self.b += struct.pack('<I', v & 0xffffffff)
    def u16(self, v): self.b += struct.pack('<H', v)
    def u8(self, v): self.b += bytes([v])
    def f(self, *v): self.b += struct.pack('<%df' % len(v), *v)
    def null(self): self.u32(1)
    def ref(self, idx): self.u32(idx + 2)

    def reg(self):
        i = self.n; self.n += 1; return i

    def string(self, s):
        self.u32(0); b = s.encode('latin1'); self.u32(len(b)); self.b += b
        return self.reg()

    def new(self, tname):               # header of a new object; returns its index
        self.u32(0); self.u32(T[tname]); return self.reg()

    def node(self, name, parent, flags, u0c=0xffffff, u30=0xff, r18=None):   # eNode::serialize
        self.string(name)
        if parent is None: self.null()
        else: self.ref(parent)
        self.u32(u0c)
        if r18 is None: self.null()     # +0x18 (collision eALBox or NULL)
        else: r18()
        self.u32(flags)
        self.f(0, 0, 0, -1)             # sfera (przeliczana)
        self.null()                     # +0x34 Actor (no script)
        self.u16(u30)

    def xform_identity(self):
        self.f(1.0); self.f(0, 0, 0, 1); self.f(0, 0, 0)

    def geoarray(self, tname, fmt, values, count):
        idx = self.new(tname); self.u32(count)
        if count: self.b += struct.pack('<%d%s' % (len(values), fmt), *values)
        return idx

    def arr3fx(self, values4):          # specjalna tablica 3fX (FUN_0053e460)
        self.u32(0); self.u32(T['3fx']); n = len(values4) // 4; self.u32(n)
        self.f(*values4)
        return self.reg()


def make_texture(size=128, line=3):
    px = bytearray()
    for y in range(size):
        for x in range(size):
            border = x < line or y < line or x >= size - line or y >= size - line
            mid = abs(x - size // 2) < 1 or abs(y - size // 2) < 1
            c = 70 if border else (150 if mid else 200)
            px += bytes((c, c, c + 10 if not border else c, 255))   # RGBA, lekko niebieskawe pola
    return bytes(px)


def make_wood_texture(size=64):
    """Barrel texture: vertical staves (brown), two dark hoops."""
    px = bytearray()
    for y in range(size):
        for x in range(size):
            plank = (x % 16) in (0, 15)
            hoop = abs(y - size * 0.2) < 3 or abs(y - size * 0.8) < 3
            grain = ((x * 7 + y * 3) % 11) * 2
            if hoop: c = (60, 60, 66)
            elif plank: c = (85, 50, 25)
            else: c = (150 + grain, 95 + grain // 2, 45)
            px += bytes((c[0], c[1], c[2], 255))
    return bytes(px)


def barrel_geo(cx, cy, z, r=60.0, h=140.0, seg=12):
    """Cylinder (barrel): sides = 4-vertex strips like box_geo (du x dv = outwards), lid = triangles (c, p0, p1)."""
    g = {'pos': [], 'nrm': [], 'uv': [], 'idx': [], 'strips': []}
    v = 0
    pts = [(cx + r * math.cos(2 * math.pi * k / seg), cy + r * math.sin(2 * math.pi * k / seg)) for k in range(seg + 1)]
    for k in range(seg):
        (x0, y0), (x1, y1) = pts[k], pts[k + 1]
        am = 2 * math.pi * (k + 0.5) / seg
        n = (math.cos(am), math.sin(am), 0.0)
        o = (x0, y0, z); du = (x1 - x0, y1 - y0, 0.0); dv = (0.0, 0.0, h)
        for (a, b) in ((1, 0), (1, 1), (0, 0), (0, 1)):
            g['pos'] += [o[i] + a * du[i] + b * dv[i] for i in range(3)] + [1.0]
            g['nrm'] += list(n) + [0.0]; g['uv'] += [(k + a) / seg * 3.0, b]
            g['idx'].append(v); v += 1
        g['strips'].append(4)
    for k in range(seg):
        for (x, y) in ((cx, cy), pts[k], pts[k + 1]):
            g['pos'] += [x, y, z + h, 1.0]; g['nrm'] += [0.0, 0.0, 1.0, 0.0]
            g['uv'] += [0.5 + (x - cx) / (2 * r), 0.5 + (y - cy) / (2 * r)]
            g['idx'].append(v); v += 1
        g['strips'].append(3)
    g['bbox'] = (cx - r, cy - r, z, cx + r, cy + r, z + h)
    g['albox'] = g['bbox']
    return g


_BARREL = None
def orig_barrel_geo(cx, cy, z):
    """The game's own barrel model, if the user exported it into build/barrel_model.json; otherwise the built-in cylinder barrel."""
    global _BARREL
    import json, os
    if _BARREL is None:                                   # optional: user-exported game model (never shipped with KaoForge)
        f = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'barrel_model.json')
        _BARREL = json.load(open(f)) if os.path.exists(f) else False
    if _BARREL is False:
        return barrel_geo(cx, cy, z)                      # built-in cylinder barrel
    b = _BARREL
    pos = []
    for i in range(0, len(b['pos']), 4):
        pos += [b['pos'][i] + cx, b['pos'][i + 1] + cy, b['pos'][i + 2] + z, 1.0]
    g = {'pos': pos, 'nrm': list(b['nrm']), 'uv': list(b['uv']), 'idx': list(b['idx']), 'strips': list(b['strips'])}
    xs, ys, zs = pos[0::4], pos[1::4], pos[2::4]
    g['bbox'] = (min(xs), min(ys), min(zs), max(xs), max(ys), max(zs))
    g['albox'] = g['bbox']
    return g


def barrel_layout():
    """Smashable barrels: a ring around the spawn plus a few near the stairs and blocks."""
    pts = [(500 * math.cos(2 * math.pi * k / 8), 500 * math.sin(2 * math.pi * k / 8)) for k in range(8)]
    pts += [(800, 500), (950, 500), (-1500, 400), (-1300, 1150), (1500, -1800), (-2500, -800)]
    return [('barrel%02d' % (k + 1), x, y) for k, (x, y) in enumerate(pts)]


def build_aabb_tree(pos, idx, strips):
    """Collision tree like FUN_005b2db0/FUN_005b3080: triangle list from the strips, preorder nodes,
    node = [min-1 (3f), left (u32), max+1 (3f), right (u32)], leaf = 0x80000000 | triangle."""
    tris = []                               # (value, bbox)
    base = 0
    for n in strips:
        for k in range(2, n):
            p = base - 2 + k
            val = p | (0x40000000 if k % 2 else 0)
            vs = [idx[p], idx[p + 1], idx[p + 2]]
            xs = [pos[4 * q] for q in vs]; ys = [pos[4 * q + 1] for q in vs]; zs = [pos[4 * q + 2] for q in vs]
            tris.append((val, (min(xs), min(ys), min(zs), max(xs), max(ys), max(zs))))
        base += n
    nodes = []

    def bbox(items):
        return (min(t[1][0] for t in items), min(t[1][1] for t in items), min(t[1][2] for t in items),
                max(t[1][3] for t in items), max(t[1][4] for t in items), max(t[1][5] for t in items))

    def rec(items):
        if len(items) == 1:
            return 0x80000000 | items[0][0]
        me = len(nodes); nodes.append(None)
        b = bbox(items)
        ext = (b[3] - b[0], b[4] - b[1], b[5] - b[2])
        ax = ext.index(max(ext))
        items = sorted(items, key=lambda t: t[1][ax] + t[1][ax + 3])
        mid = len(items) // 2
        left = rec(items[:mid]); right = rec(items[mid:])
        nodes[me] = (b[0] - 1, b[1] - 1, b[2] - 1, left, b[3] + 1, b[4] + 1, b[5] + 1, right)
        return me
    rec(tris)
    assert len(nodes) == len(tris) - 1
    out = bytearray()
    for nd in nodes:
        out += struct.pack('<3fI3fI', *nd)
    return len(nodes), bytes(out)


def floor_geo(cells, cell, z, ox=0.0, oy=0.0):
    half = cells * cell / 2
    g = {'pos': [], 'nrm': [], 'uv': [], 'idx': [], 'strips': []}
    v = 0
    for j in range(cells):
        for i in range(cells):
            x0, y0 = ox - half + i * cell, oy - half + j * cell
            x1, y1 = x0 + cell, y0 + cell
            for (x, y, u, t) in ((x1, y0, 1, 0), (x1, y1, 1, 1), (x0, y0, 0, 0), (x0, y1, 0, 1)):
                g['pos'] += [x, y, z, 1.0]; g['nrm'] += [0.0, 0.0, 1.0, 0.0]; g['uv'] += [u, t]
                g['idx'].append(v); v += 1
            g['strips'].append(4)
    g['bbox'] = (ox - half, oy - half, z, ox + half, oy + half, z)
    g['albox'] = (ox - half, oy - half, z, ox + half, oy + half, z + 0.0001)
    return g


EXTRA_MATS = {}        # mkey -> dict(tw, th, px, b14, blend, name)  (props from the game)
SPEC_LIGHT = {'sky': [0.45, 0.62, 0.85], 'ambient': [0.42, 0.45, 0.52], 'sun': {'color': [0.80, 0.74, 0.62], 'dir': [500.0, 350.0, 900.0]}}

def prop_layout(z, exp_dir, inner=6500.0, outer=29500.0, gap=400.0):
    """Props from the in-game export (tools_props.py): one instance each, in rows outside the map centre.
    Returns [(name, geo, mkey)]; registers the textures in EXTRA_MATS."""
    import tools_props
    props, textures = tools_props.collect(exp_dir)
    items = []
    for key, (lvl, parts) in sorted(props.items()):
        ms, lo, hi = tools_props.prop_meshes(parts)
        if not ms or max(hi[i] - lo[i] for i in range(3)) < 20: continue
        items.append((key, ms, lo, hi))
    items.sort(key=lambda it: -(it[3][1] - it[2][1]))          # deepest first (tidier rows)
    out = []; x = -outer; y = inner; row_d = 0                  # rows from the north, just behind the cubes
    for key, ms, lo, hi in items:
        sx, sy = hi[0] - lo[0], hi[1] - lo[1]
        if sx > 2 * outer or sy > 2 * outer: continue
        while True:
            if x + sx > outer: x = -outer; y += row_d + gap; row_d = 0
            if y + sy > outer and y > 0: x = -outer; y = -outer; row_d = 0   # north is full -> south
            if y + sy > outer: break
            if not (x + sx > -inner and x < inner and y + sy > -inner and y < inner): break
            x = inner + gap                                       # skip the map centre
        if y + sy > outer: print('no room for', key); continue
        dx, dy, dz = x - lo[0], y - lo[1], z - lo[2]
        for mi, m in enumerate(ms):
            tk = m['tk']; mkey = 'p%s_%d_%d' % (tk[0][:12], tk[1] & 0xffffffff, m['b14'])
            if mkey not in EXTRA_MATS:
                t = textures[tk]
                EXTRA_MATS[mkey] = dict(tw=t['tw'], th=t['th'], px=t['px'], b14=m['b14'], blend=m['blend'], name=os.path.basename(t['path'].replace('\\', '/')) or mkey)
            g = {'pos': [], 'nrm': [], 'uv': [], 'idx': [], 'strips': []}
            for (px_, py_, pz_), n, u in zip(m['P'], m['N'], m['U']):
                g['pos'] += [px_ + dx, py_ + dy, pz_ + dz, 1.0]; g['nrm'] += [n[0], n[1], n[2], 0.0]; g['uv'] += [u[0], u[1]]
            for a, b, c in m['T']:
                g['idx'] += [a, b, c]; g['strips'].append(3)
            if len(m['P']) >= 65536: continue
            xs, ys, zs = g['pos'][0::4], g['pos'][1::4], g['pos'][2::4]
            g['bbox'] = (min(xs), min(ys), min(zs), max(xs), max(ys), max(zs))
            g['albox'] = g['bbox']
            out.append(('%s_%02d' % (key[:-3], mi), g, mkey))
        print('prop %-34s @ %7.0f %7.0f  (%d meshes)' % (key, x, y, len(ms)))
        x += sx + gap; row_d = max(row_d, sy)
    return out


def box_geo(x0, y0, z0, x1, y1, z1):
    """Box: 6 faces, each a strip of 4 vertices in the same order as the game's Plane01
    (u=1,v=0),(1,1),(0,0),(0,1); du x dv = outward normal; UV 0..1 per face."""
    dx, dy, dz = x1 - x0, y1 - y0, z1 - z0
    faces = [((x0, y0, z1), (dx, 0, 0), (0, dy, 0), (0, 0, 1)),
             ((x0, y0, z0), (0, dy, 0), (dx, 0, 0), (0, 0, -1)),
             ((x1, y0, z0), (0, dy, 0), (0, 0, dz), (1, 0, 0)),
             ((x0, y0, z0), (0, 0, dz), (0, dy, 0), (-1, 0, 0)),
             ((x0, y1, z0), (0, 0, dz), (dx, 0, 0), (0, 1, 0)),
             ((x0, y0, z0), (dx, 0, 0), (0, 0, dz), (0, -1, 0))]
    g = {'pos': [], 'nrm': [], 'uv': [], 'idx': [], 'strips': []}
    v = 0
    for o, du, dv, n in faces:
        for (a, b) in ((1, 0), (1, 1), (0, 0), (0, 1)):
            g['pos'] += [o[k] + a * du[k] + b * dv[k] for k in range(3)] + [1.0]
            g['nrm'] += list(n) + [0.0]; g['uv'] += [a, b]
            g['idx'].append(v); v += 1
        g['strips'].append(4)
    g['bbox'] = (x0, y0, z0, x1, y1, z1)
    g['albox'] = g['bbox']
    return g


def default_layout(z):
    """Default cubes: stairs, a tower, floating platforms and scattered blocks."""
    cubes = []
    for k in range(8):                                  # stairs along +X: steps of 100 units (Kao jumps ~140)
        x0 = 600 + k * 300
        cubes.append(('stairs%02d' % (k + 1), (x0, -300, z, x0 + 300, 300, z + 100 * (k + 1))))
    cubes.append(('tower', (3100, -400, z, 3900, 400, z + 900)))
    for k in range(6):                                  # floating platforms from the tower towards +Y
        y0 = 800 + k * 700
        cubes.append(('platform%02d' % (k + 1), (3200, y0, z + 850 + k * 90, 3800, y0 + 400, z + 900 + k * 90)))
    blocks = [(-1500, 800, 400, 150), (-2500, -1200, 600, 300), (-1200, -2600, 300, 80), (1500, -2200, 500, 220),
              (-3800, 2500, 900, 500), (2200, 2600, 350, 120), (-600, 3500, 450, 260), (4800, -3800, 1200, 200)]
    for k, (cx, cy, size, h) in enumerate(blocks):
        cubes.append(('block%02d' % (k + 1), (cx - size / 2, cy - size / 2, z, cx + size / 2, cy + size / 2, z + h)))
    return cubes


def sap_links(boxes):
    """Sweep-and-prune lists for eALBox: for every axis the interval ends are sorted, with prev/next links.
    Returns links[i][axis][0=min/1=max] = (prev, next). Encoding (FUN_0057c110): sentinel -1/+1,
    inny koniec ±(indeks+2), minus = koniec min."""
    links = [[[None, None] for _ in range(3)] for _ in boxes]
    for ax in range(3):
        ends = []
        for i, b in enumerate(boxes):
            ends.append((b[ax], 0, i)); ends.append((b[ax + 3], 1, i))
        ends.sort()
        enc = lambda e: (e[2] + 2) if e[1] else -(e[2] + 2)
        for k, e in enumerate(ends):
            prev = -1 if k == 0 else enc(ends[k - 1])
            nxt = 1 if k == len(ends) - 1 else enc(ends[k + 1])
            links[e[2]][ax][e[1]] = (prev, nxt)
    return links


GAME_DIR = os.environ.get('KAOFORGE_GAME', '')   # set by kaoforge.py (needed only for the prop catalog export dir)

DEFAULT_SPEC = {
    # map description (kaoforge.py build: maps/*.json). Missing keys take these values.
    'floor': {'tiles': 3, 'cells': 100, 'cell': 200.0, 'z': -200.0},
    'sky': [0.45, 0.62, 0.85],
    'ambient': [0.42, 0.45, 0.52],
    'sun': {'color': [0.80, 0.74, 0.62], 'dir': [500.0, 350.0, 900.0]},
    'cubes': 'default',                 # 'default' | [] | [{"name":..., "box":[x0,y0,z0,x1,y1,z1]}]  (z relative to the floor)
    'barrels': 'default',               # 'default' | [] | [[x, y], ...]   (smashable barrel)
    'props': 'all',                     # 'all' (whole catalog in rows) | [] | [{"prop":"drzewo01.ar","pos":[x,y],"rot":deg,"scale":1}]
    'export_dir': None,                 # default: <game>/kaoforge_export
}

def place_prop(ms, lo, hi, x, y, z, rot_deg=0.0, scale=1.0, key='prop'):
    """Prop (world space from the export) -> bbox centre xy at (x,y), bottom at z, rotated about Z, scaled."""
    import math as _m
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    c, s_ = _m.cos(_m.radians(rot_deg)), _m.sin(_m.radians(rot_deg))
    out = []
    for mi, m in enumerate(ms):
        g = {'pos': [], 'nrm': [], 'uv': [], 'idx': [], 'strips': []}
        for (px_, py_, pz_), n, u in zip(m['P'], m['N'], m['U']):
            dx, dy = (px_ - cx) * scale, (py_ - cy) * scale
            g['pos'] += [x + dx * c - dy * s_, y + dx * s_ + dy * c, z + (pz_ - lo[2]) * scale, 1.0]
            g['nrm'] += [n[0] * c - n[1] * s_, n[0] * s_ + n[1] * c, n[2], 0.0]; g['uv'] += [u[0], u[1]]
        T = m['T'] if scale >= 0 else [(b, a, cc) for a, b, cc in m['T']]
        for a, b, cc in T:
            g['idx'] += [a, b, cc]; g['strips'].append(3)
        if not g['idx'] or len(m['P']) >= 65536: continue
        xs, ys, zs = g['pos'][0::4], g['pos'][1::4], g['pos'][2::4]
        g['bbox'] = (min(xs), min(ys), min(zs), max(xs), max(ys), max(zs)); g['albox'] = g['bbox']
        out.append((g, m))
    return out

def _prop_mat(m, textures):
    tk = m['tk']; mkey = 'p%s_%d_%d' % (tk[0][:12], tk[1] & 0xffffffff, m['b14'])
    if mkey not in EXTRA_MATS:
        t = textures[tk]
        EXTRA_MATS[mkey] = dict(tw=t['tw'], th=t['th'], px=t['px'], b14=m['b14'], blend=m['blend'],
                                name=os.path.basename(t['path'].replace('\\', '/')) or mkey)
    return mkey

def spec_props(spec, z):
    import tools_props
    exp = spec.get('export_dir') or os.path.join(GAME_DIR, 'kaoforge_export')
    want = spec['props']
    if not want or not os.path.isdir(exp): return []
    if want == 'all': return prop_layout(z, exp)
    props, textures = tools_props.collect(exp)
    out = []; cache = {}
    for k, p in enumerate(want):
        key = p['prop'].lower()
        if key not in props: print('prop not in the catalog:', key); continue
        if key not in cache: cache[key] = tools_props.prop_meshes(props[key][1])
        ms, lo, hi = cache[key]
        pz = z + p.get('z', 0.0)
        for mi, (g, m) in enumerate(place_prop(ms, lo, hi, p['pos'][0], p['pos'][1], pz, p.get('rot', 0.0), p.get('scale', 1.0))):
            out.append(('%s_%d_%02d' % (key[:-3], k, mi), g, _prop_mat(m, textures)))
    return out

def build(spec=None):
    sp = dict(DEFAULT_SPEC); sp.update(spec or {})
    fl = dict(DEFAULT_SPEC['floor']); fl.update(sp.get('floor') or {})
    cells, cell, z, tiles = int(fl['cells']), float(fl['cell']), float(fl['z']), int(fl['tiles'])
    w = W()
    half = cells * cell; r = range(-(tiles // 2), tiles - tiles // 2)
    meshes = [('grid_floor%d%d' % (i + 1, j + 1), floor_geo(cells, cell, z, i * half, j * half), 'grid') for i in r for j in r]
    if sp['cubes'] == 'default':
        for name, b in default_layout(z): meshes.append((name, box_geo(*b), 'grid'))
    else:
        for c in sp['cubes']:
            b = c['box']; meshes.append((c['name'], box_geo(b[0], b[1], z + b[2], b[3], b[4], z + b[5]), 'grid'))
    bl = barrel_layout() if sp['barrels'] == 'default' else [('barrel%02d' % (k + 1), x, y) for k, (x, y) in enumerate(sp['barrels'])]
    for name, x, y in bl: meshes.append((name, orig_barrel_geo(x, y, z), 'wood'))
    meshes += spec_props(sp, z)
    SPEC_LIGHT.update(sky=sp['sky'], ambient=sp['ambient'], sun=sp['sun'])
    boxes = [g['albox'] for _, g, _ in meshes]
    links = sap_links(boxes)
    # eALBox pair table (the game's own levels: (idx+1)//2 bytes): for j < i nibble j>>1, bits 2/4/6 = the x/y/z intervals
    # overlap, << (j & 1). An empty (NULL) table crashes the game on a pair event (FUN_00597a10).
    pairbits = []
    for i, bi in enumerate(boxes):
        t = bytearray((i + 1) // 2)
        for j in range(i):
            bj = boxes[j]; v = 0
            for ax in range(3):
                if bi[ax] <= bj[ax + 3] and bj[ax] <= bi[ax + 3]: v |= 1 << (ax * 2 + 2)
            t[j >> 1] |= v << (j & 1)
        pairbits.append(bytes(t))

    out_hdr = bytearray(b'tate') + struct.pack('<II', VERSION, 0)
    root = w.new('eScene')
    w.string('E:\\game\\levels\\99_grid\\mesh\\99_grid.max')
    w.string('exported by KaoForge tools_gen_grid.py\r\n\r\n.COMPILED.')
    n = len(meshes)
    w.u32(1); w.u32(n); w.u32(n - 1)                  # embedded eCollisionMgr: [ALBox count], max index (like level 05: [821], 820)
    w.node('$root', None, 0x7007349d, u0c=0xffffff, u30=0)
    w.u32(1)                                          # dzieci roota: global_env
    env = w.new('eEnvironment')
    w.node('global_env', root, 0x7000249d, u0c=0, u30=0)
    # The renderer only descends into branches with bit 0x10000000 (F7 scene dump of level 05) -> meshes go in #VS-grid/1, not #active_group.
    w.u32(2)
    act = w.new('eGroup')
    w.node('#active_group', env, 0x4000249d)
    w.u32(2)                                          # lights: ambient + sun
    amb = w.new('eAmbientLight')
    w.node('Ambient01', act, 0x4000209d)
    w.f(0, 0, 0, 0); w.f(*SPEC_LIGHT['ambient'], 1.0); w.f(0, 0, 0, 0)   # cool ambient (colour in c4c, like Direct03 in level 05)
    # Sun = eDirectionalLight like in level 05 (master_diff_chmurkowy01, Direct07...):
    # c3c = diffuse colour, c4c = 0, c5c = specular (= diffuse), p6c = direction vector (towards the light), target = NULL
    sun = w.new('eDirectionalLight')
    w.node('Slonce', act, 0x4000209d)
    w.f(*SPEC_LIGHT['sun']['color'], 1.0); w.f(0, 0, 0, 1.0); w.f(*SPEC_LIGHT['sun']['color'], 1.0)
    w.f(*SPEC_LIGHT['sun']['dir'])
    w.null()
    vs = w.new('eGroup')
    w.node('#VS-grid/1', env, 0x7000249d, u30=0)
    w.u32(n)
    mats = {}
    for mi, (name, g, mkey) in enumerate(meshes):
        tm = w.new('eTriMesh')

        def albox(mi=mi, tm=tm):
            w.new('eALBox')
            w.ref(root); w.ref(tm); w.u32(0); w.u32(mi)
            b = boxes[mi]
            for ax in range(3):
                for side in (0, 1):
                    prev, nxt = links[mi][ax][side]
                    w.f(b[ax + 3 * side]); w.u16(mi); w.u8(side); w.u8(ax); w.b += struct.pack('<ii', prev, nxt)
            w.u32(len(pairbits[mi])); w.b += pairbits[mi]   # +0x70/+0x78: bity par z mniejszymi indeksami (FUN_00597a10)
            w.u32(0)
        # flags like Box01 of level 05; u30 = 0 (collides); u0c = the object colour from Max (arbitrary)
        w.node(name + ' / grid', vs, 0x1002208d, u0c=0x94b11a, u30=0, r18=albox)
        if mkey not in mats:
            mats[mkey] = w.new('eMaterial')
            w.u32(1)
            w.new('eTexture')
            w.new('eBitmap')
            em = EXTRA_MATS.get(mkey)
            if mkey == 'grid':
                tw = th = 128; px = make_texture(128)
            elif em:
                tw, th, px = em['tw'], em['th'], em['px']
            else:                                      # barrel texture: user-exported game texture if present, else a generated wood texture
                f = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'barrel_tex.rgba')
                if _BARREL and os.path.exists(f): tw, th, px = 64, 128, open(f, 'rb').read()
                else: tw, th, px = 64, 64, make_wood_texture(64)
            w.u32(tw); w.u32(th); w.u32(tw); w.u32(th); w.u32(0)
            w.b += px
            w.string('levels\\99_grid\\%s.tga' % (em['name'] if em else mkey))
            w.null()
            w.u32(4); w.null(); w.u32(0)              # eTexture flags, +0x0c, +0x14
            w.u8(em['b14'] if em else 8)              # material flags from the game (8 = opaque)
            w.new('eMaterialState')
            gg = 0.5882353186607361
            w.f(gg, gg, gg, 1); w.f(gg, gg, gg, 1); w.f(0, 0, 0, 1); w.f(0, 0, 0, 1); w.u32(1096495922); w.u8(1)
            if em: w.b += bytes(em['blend'])           # blend from the material in memory (+0x1c: u32,u16,u16)
            else: w.u32(0); w.u16(0); w.u16(0)
            w.string(mkey)
            w.u32(0); w.u32(0x3f000000)
        else:
            w.ref(mats[mkey])                         # shared material
        w.f(*g['bbox'])                               # real bbox (an empty one = culled by the renderer)
        w.u32(1)
        w.new('eGeoSet')
        v = len(g['pos']) // 4                    # vertex count (there may be more indices)
        assert v < 65536
        w.u32(0x1f); w.u32(v)
        w.geoarray('u16', 'H', g['idx'], len(g['idx']))
        w.arr3fx(g['pos']); w.arr3fx(g['nrm'])
        w.u32(1); w.geoarray('p2', 'f', g['uv'], v); w.u32(0)
        w.geoarray('p4', 'f', [1.0] * (4 * v), v)
        w.geoarray('u16', 'H', g['strips'], len(g['strips']))
        nn, tree_bytes = build_aabb_tree(g['pos'], g['idx'], g['strips'])
        w.new('abb'); w.u32(nn); w.b += tree_bytes
        w.null()
    w.u32(2); w.ref(amb); w.ref(sun)                  # eEnvironment: lights
    w.f(0.5, 0.5, 0.5, 1.0)
    w.xform_identity(); w.xform_identity(); w.null()  # root: eTransform
    w.u32(0); tree = w.reg()                          # ePivot: tree node with a default eAnimState
    w.null(); w.null()
    w.new('eAnimState')
    for x in (0, 0x3f800000, 0, 0, 0, 0, 0, 0x3f800000, 2, 0, 1900671690, 0, 0, 0, 0, 0):
        w.u32(x)
    w.ref(tree)
    w.u32(0); w.u8(0); w.u32(0); w.u8(1)
    w.u32(0)
    w.f(*SPEC_LIGHT['sky'])                           # background colour
    w.u32(0)
    w.f(0, 0, 0, 1)
    struct.pack_into('<I', out_hdr, 8, w.n)
    return bytes(out_hdr + w.b)


if __name__ == '__main__':
    import json
    spec = json.load(open(sys.argv[2])) if len(sys.argv) > 2 else None
    data = build(spec)
    open(sys.argv[1], 'wb').write(data)
    print('written', sys.argv[1], len(data), 'B')
