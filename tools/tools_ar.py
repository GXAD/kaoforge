#!/usr/bin/env python3
"""Reader for the Kao2 engine's .ar scene archives (version 0x89), reconstructed from the engine's serialize() methods.
Usage: tools_ar.py file.ar [--dump]
Output: the object tree (type, name, fields). An unknown class raises an error with its offset (a handler has to be added)."""
import sys, struct

TYPES = {}
try:
    import os
    for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'typeinfo.txt')):
        p = line.split()
        if p and not p[0].startswith('#'): TYPES[int(p[0], 16)] = p[1]
except OSError:
    pass


class ArError(Exception):
    pass


class Reader:
    def __init__(self, data):
        self.d = data
        self.o = 0
        self.refs = []      # shared reference table (objects, strings, tree nodes)
        self.raw = []       # table for the special 3fX arrays (FUN_005c7a80/7c10)
        self.flag6 = 0      # byte ar[6] (0 when loading a level)

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.o)[0]; self.o += 4; return v

    def i32(self):
        v = struct.unpack_from('<i', self.d, self.o)[0]; self.o += 4; return v

    def u16(self):
        v = struct.unpack_from('<H', self.d, self.o)[0]; self.o += 2; return v

    def u8(self):
        v = self.d[self.o]; self.o += 1; return v

    def f(self, n=1):
        v = struct.unpack_from('<%df' % n, self.d, self.o); self.o += 4 * n
        return v[0] if n == 1 else list(v)

    def raw_bytes(self, n):
        v = self.d[self.o:self.o + n]; self.o += n; return v

    # --- referencje ---
    def string(self):
        tag = self.u32()
        if tag == 1:
            return None
        if tag == 0:
            n = self.u32()
            s = self.raw_bytes(n).decode('latin1')
            self.refs.append(s)
            return s
        return self.refs[tag - 2]

    def obj(self):
        start = self.o
        tag = self.u32()
        if tag == 1:
            return None
        if tag >= 2:
            r = self.refs[tag - 2]
            return {'ref': r['type'] + ':' + str(r.get('name')) if isinstance(r, dict) else r}
        tid = self.u32()
        name = TYPES.get(tid, hex(tid))
        o = {'type': name, 'tid': tid, 'off': start}
        self.refs.append(o)
        h = HANDLERS.get(name)
        if h is None:
            raise ArError(f'no handler for {name} ({tid:#x}) @ {start:#x}')
        h(self, o)
        return o

    def objarray(self):
        n = self.u32()
        return [self.obj() for _ in range(n)]

    def tree(self):
        tag = self.u32()
        if tag == 1:
            return None
        if tag >= 2:
            return {'treeref': tag - 2}
        node = {'tree': True}
        self.refs.append(node)
        node['l'] = self.tree()
        node['r'] = self.tree()
        node['v'] = self.obj()
        return node

    def array3fx(self):
        tag = self.u32()
        if tag == 1:
            return None
        if tag == 0:
            x = self.u32()
            n = self.u32()
            data = self.f(4 * n) if n else []
            a = {'x': x, 'n': n, 'data': data}
            self.refs.append(a)             # shared table with the objects (nrefs = total)
            return a
        return {'rawref': self.refs[tag - 2]}


# --- class handlers (field order = serialize() in the exe) ---
def h_node(r, o):                       # eNode::serialize 0x53f480
    o['name'] = r.string()
    o['parent'] = r.obj()
    o['u0c'] = r.u32()
    o['r18'] = r.obj()
    o['flags'] = r.u32()
    o['sphere'] = r.f(3) + [r.f()]
    o['r34'] = r.obj()
    o['u30'] = r.u16()


def h_group(r, o):                      # eGroup 0x55ea10
    h_node(r, o)
    o['children'] = r.objarray()


def xform(r):                           # 0x599ae0: f(+1c) quat(4) pos(3)
    s = r.f()
    q = r.f(4)
    p = r.f(3)
    return {'scale': s, 'rot': q, 'pos': p}


def h_transform(r, o):                  # eTransform 0x54c350
    if not r.flag6:
        h_group(r, o)
    o['xform'] = xform(r)
    o['xform2'] = xform(r)
    if not r.flag6:
        o['ra8'] = r.obj()


def h_pivot(r, o):                      # ePivot 0x562fe0
    h_transform(r, o)
    o['tree1'] = r.tree()               # FUN_005a5040
    o['tree2'] = r.tree()
    o['anims'] = r.objarray()
    o['b18'] = r.u8()
    o['u34'] = r.u32()
    o['b38'] = r.u8()
    if not r.flag6:
        o['e8'] = r.objarray()          # FUN_00564390


def h_scene(r, o):                      # eScene 0x57e9d0
    o['src'] = r.string()
    o['info'] = r.string()
    n = r.u32()                         # osadzony eCollisionMgr 0x57f160
    o['cm'] = [r.u32() for _ in range(n)]
    o['cm10'] = r.u32()
    h_pivot(r, o)
    o['p170'] = r.f(3)
    o['a194'] = r.objarray()
    o['c17c'] = r.f(4)


def h_geometry(r, o):                   # eGeometry 0x53d850
    h_node(r, o)
    o['material'] = r.obj()
    o['bbox'] = r.f(6)


def h_trimesh(r, o):                    # eTriMesh 0x536f50
    h_geometry(r, o)
    o['one'] = r.u32()
    o['geoset'] = r.obj()


def h_geoset(r, o):                     # eGeoSet 0x537070
    o['u08'] = r.u32()
    o['u0c'] = r.u32()
    o['r18'] = r.obj()
    o['x1c'] = r.array3fx()
    o['x24'] = r.array3fx()
    n = r.u32()
    o['tex'] = [(r.obj(), r.u32()) for _ in range(n)]
    o['r3c'] = r.obj()
    o['r14'] = r.obj()
    o['r5c'] = r.obj()
    o['r58'] = r.obj()


def geoarray(size, fmt):
    def h(r, o):
        n = r.u32()
        o['n'] = n
        o['data'] = list(struct.unpack_from('<%d%s' % (n * size // struct.calcsize(fmt), fmt), r.d, r.o)) if n else []
        r.o += n * size
    return h


def h_material(r, o):                   # eMaterial 0x54d3a0
    o['textures'] = r.objarray()
    o['b14'] = r.u8()
    o['state'] = r.obj()
    o['blend'] = (r.u32(), r.u16(), r.u16())
    o['name'] = r.string()
    o['u28'] = r.u32()
    o['u2c'] = r.u32()


def h_texture(r, o):                    # eTexture 0x5532e0
    o['bitmap'] = r.obj()
    o['flags'] = r.u32()
    o['r0c'] = r.obj()
    o['u14'] = r.u32()


BPP = {0: 4, 4: 4, 0x2c: 4, 0x2d: 4, 1: 3, 2: 1, 3: 1, 5: 1, 6: 1, 0x2e: 1, 0x2f: 1, 0x4d: 1}
PALETTED = {2, 3, 5, 6, 0x2e, 0x2f}


def h_bitmap(r, o):                     # eBitmap 0x54f590
    o['w'], o['h'], o['vw'], o['vh'], o['fmt'] = r.u32(), r.u32(), r.u32(), r.u32(), r.u32()
    size = o['vw'] * o['vh'] // 2 if o['fmt'] == 0x4d else o['vw'] * o['vh'] * BPP.get(o['fmt'], 0)
    o['data_off'] = r.o
    o['data_len'] = size
    r.o += size
    if o['fmt'] in PALETTED:
        o['pal_off'] = r.o
        r.o += 0x400
    o['path'] = r.string()
    o['r30'] = r.obj()


def h_matstate(r, o):                   # eMaterialState 0x55d760
    o['c18'] = r.f(4)
    o['c08'] = r.f(4)
    o['c28'] = r.f(4)
    o['c38'] = r.f(4)
    o['u48'] = r.u32()
    o['b4c'] = r.u8()


def h_env(r, o):                        # eEnvironment 0x5544e0
    h_group(r, o)
    o['lights'] = r.objarray()          # FUN_00555450
    o['c54'] = r.f(4)
    if o['flags'] & 0x1000:
        o['fog'] = r.obj()


def h_light(r, o):                      # eLight 0x555c20 (eAmbientLight uses the same)
    h_node(r, o)
    o['c3c'] = r.f(4)
    o['c4c'] = r.f(4)
    o['c5c'] = r.f(4)


def h_dirlight(r, o):                   # eDirectionalLight 0x556bc0
    h_light(r, o)
    o['p6c'] = r.f(3)
    o['target'] = r.obj()


def h_omnilight(r, o):                  # eOmniLight 0x556450
    h_light(r, o)
    o['p6c'] = r.f(3)
    o['p78'] = r.f(3)


def h_animstate(r, o):                  # eAnimState 0x5a3c90: 16 x u32 (pola 0x08..0x44)
    o['raw'] = [r.u32() for _ in range(16)]


def h_albox(r, o):                      # eALBox 0x598800 - a collision broad-phase (sweep-and-prune) entry
    o['scene'] = r.obj()                # +0x08: scene (table scene+0x148[index] = this)
    o['node'] = r.obj()                 # +0x0c: owner node
    o['zero'] = r.u32()
    o['index'] = r.u32()                # +0x7c
    eps = []                            # 3 osie x (min, max); FUN_0057c220
    for _ in range(6):
        v = r.f(); idx = r.u16(); a = r.u8(); b = r.u8(); l1 = r.i32(); l2 = r.i32()
        eps.append((v, idx, a, b, l1, l2))
    o['ep'] = eps
    n = r.u32(); o['bytes70'] = list(r.raw_bytes(n))
    o['refs80'] = r.objarray()


def h_alzonesensor(r, o):               # eALZoneSensor 0x585220
    h_albox(r, o)
    o['actor'] = r.obj()                # FUN_005c5dc0 (typowany ref)
    o['ncoll'] = r.u32()
    o['name'] = r.string()


HANDLERS = {
    'eALBox': h_albox, 'eALZoneSensor': h_alzonesensor,
    'eAnimState': h_animstate,
    'eEnvironment': h_env, 'eLight': h_light, 'eAmbientLight': h_light,
    'eDirectionalLight': h_dirlight, 'eOmniLight': h_omnilight,
    'eNode': h_node, 'eGroup': h_group, 'eTransform': h_transform, 'ePivot': h_pivot, 'eScene': h_scene,
    'eGeometry': h_geometry, 'eTriMesh': h_trimesh, 'eGeoSet': h_geoset,
    'eGeoArray<ushort>': geoarray(2, 'H'), 'eGeoArray<ePoint2>': geoarray(8, 'f'),
    'eGeoArray<ePoint3>': geoarray(12, 'f'), 'eGeoArray<ePoint4>': geoarray(16, 'f'), 'e3fXArray': geoarray(16, 'f'),
    # geoset collision AABB tree (FUN_005b2db0/5b3080): 32-byte node = min3f-1, i32 left, max3f+1, i32 right;
    # a child with bit 0x80000000 = leaf (triangle: position in the strip indices | 0x40000000 for odd ones)
    'eGeoArray<eABB>': geoarray(32, 'I'), 'eGeoArray<uint>': geoarray(4, 'I'),
    'eMaterial': h_material, 'eTexture': h_texture, 'eBitmap': h_bitmap, 'eMaterialState': h_matstate,
}


LAST = {}


def parse(data):
    r = Reader(data)
    LAST['r'] = r
    assert data[:4] == b'tate', data[:4]
    r.o = 4
    ver = r.u32()
    nrefs = r.u32()
    root = r.obj()
    return {'version': ver, 'nrefs': nrefs, 'root': root, 'end': r.o, 'len': len(data), 'nreg': len(r.refs)}


def show(o, ind=0, seen=None):
    if o is None:
        return
    pad = '  ' * ind
    if 'ref' in o:
        print(pad + '-> ' + o['ref']); return
    keys = [k for k in o if k not in ('children', 'type', 'tid', 'off', 'data')]
    summary = {k: o[k] for k in keys if not isinstance(o[k], (dict, list)) or k in ('sphere', 'bbox', 'xform', 'pos')}
    print(f"{pad}{o['type']} @{o['off']:#x} {summary}")
    for k in keys:
        v = o[k]
        if isinstance(v, dict) and 'type' in v:
            print(f'{pad}  .{k}:'); show(v, ind + 2)
        elif isinstance(v, list):
            for i, x in enumerate(v):
                if isinstance(x, dict) and 'type' in x:
                    print(f'{pad}  .{k}[{i}]:'); show(x, ind + 2)
                elif isinstance(x, tuple) and x and isinstance(x[0], dict):
                    print(f'{pad}  .{k}[{i}]:'); show(x[0], ind + 2)
    for c in o.get('children', []):
        show(c, ind + 1)


if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    try:
        res = parse(d)
        show(res['root'])
        print(f"OK version={res['version']:#x} nrefs={res['nrefs']} registered={res['nreg']} end={res['end']:#x} len={res['len']:#x}")
    except ArError as e:
        print('STOP:', e)
        objs = [x for x in LAST['r'].refs if isinstance(x, dict) and 'type' in x]
        if objs:
            show(objs[0])
