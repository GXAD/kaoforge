#!/usr/bin/env python3
"""Props from the in-game export (kaoforge_export/*.bin) -> meshes in world space, ready for the map generator.
A prop = the contents of one eProxy (e.g. "drzewo01.ar"); the first instance of each file across all levels is used.
World (from the engine): eTransform +0x68 quat (x,y,z,w), +0x78 pos, +0x84 scale (may be negative = mirrored).
World vertex = pos + scale * q v q*. Textures -> RGBA."""
import glob, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tools_kexp import load

def qrot(q, v):
    x, y, z, w = q; vx, vy, vz = v
    tx = 2 * (y * vz - z * vy); ty = 2 * (z * vx - x * vz); tz = 2 * (x * vy - y * vx)
    return (vx + w * tx + (y * tz - z * ty), vy + w * ty + (z * tx - x * tz), vz + w * tz + (x * ty - y * tx))

def to_rgba(t):
    n = t['vw'] * t['vh']; f = t['fmt']; d = t['data']
    if f in (0, 4, 0x2c, 0x2d) and len(d) >= n * 4: return d[:n * 4]
    if f == 1 and len(d) >= n * 3: return b''.join(d[i*3:i*3+3] + b'\xff' for i in range(n))
    if f in (2, 3, 5, 6, 0x2e, 0x2f) and len(d) >= n and len(t['pal']) == 0x400:
        p = t['pal']; return b''.join(p[i*4:i*4+4] for i in d[:n])
    return None

def tris_of(g):
    """Triangle list (i0,i1,i2) from the strips; odd triangles in a strip are flipped (the usual strip convention)."""
    ns, sb = g['strips']; ni, ib = g['idx']
    strips = struct.unpack('<%dH' % ns, sb) if ns else (); idx = struct.unpack('<%dH' % ni, ib) if ni else ()
    out = []; base = 0
    if not strips or sum(strips) > len(idx):          # no strips -> a plain triangle list
        for k in range(0, len(idx) - 2, 3):
            a, b, c = idx[k], idx[k+1], idx[k+2]
            if a != b and b != c and a != c: out.append((a, b, c))
        return out
    for n in strips:
        for k in range(2, n):
            a, b, c = idx[base+k-2], idx[base+k-1], idx[base+k]
            if a == b or b == c or a == c: continue
            out.append((a, b, c) if k % 2 == 0 else (b, a, c))
        base += n
    return out

SKIP_NAME = ('invisible', 'collision', 'kolizja', 'shadow', 'cien', 'kawalk', 'odlamek')   # helper/debris meshes, by the game's own (partly Polish) naming

def collect(exp_dir):
    props = {}; textures = {}
    for f in sorted(glob.glob(os.path.join(exp_dir, '*.bin'))):
        tex, geo, ms = load(f)
        inst = {}
        for m in ms:
            segs = m['chain'].split('/')
            pi = [i for i, s in enumerate(segs) if '|' in s and s.split('|')[1].endswith('.ar')]
            if not pi: continue
            i = pi[-1]; key = segs[i].split('|')[1].lower()
            if key in props: continue
            inst.setdefault(key, {}).setdefault('/'.join(segs[:i+1]), []).append(m)
        for key, by in inst.items():
            first = sorted(by)[0]
            parts = []
            for m in by[first]:
                low = m['chain'].lower()
                if any(s in low.rsplit('/', 1)[-1] for s in SKIP_NAME): continue
                t = tex.get(m['tex']); g = geo.get(m['geo'])
                if not t or not g or g['pos'][0] == 0: continue
                tk = (os.path.basename(f), m['tex'])
                if tk not in textures:
                    rgba = to_rgba(t)
                    if rgba is None: continue
                    textures[tk] = dict(tw=t['vw'], th=t['vh'], px=rgba, path=t['path'])
                parts.append((m, g, tk))
            if parts: props[key] = (os.path.basename(f)[:-4], parts)
    return props, textures

def prop_meshes(parts):
    """-> list of (geo dict like in the generator, tex_key, b14, blend) in world space, plus the overall bbox."""
    out = []; lo = [1e30] * 3; hi = [-1e30] * 3
    for m, g, tk in parts:
        q = m['world'][:4]; p = m['world'][4:7]; s = m['world'][7]
        nv, pb = g['pos']; _, nb = g['nrm']; nu, ub = g['uv']
        P = []; N = []; U = []
        for i in range(nv):
            v = struct.unpack_from('<3f', pb, 16 * i); r = qrot(q, v)
            w = (p[0] + s * r[0], p[1] + s * r[1], p[2] + s * r[2]); P.append(w)
            for k in range(3): lo[k] = min(lo[k], w[k]); hi[k] = max(hi[k], w[k])
            n = struct.unpack_from('<3f', nb, 16 * i) if len(nb) >= 16 * (i + 1) else (0, 0, 1)
            rn = qrot(q, n); sg = 1 if s >= 0 else -1; N.append((rn[0] * sg, rn[1] * sg, rn[2] * sg))
            U.append(struct.unpack_from('<2f', ub, 8 * i) if len(ub) >= 8 * (i + 1) else (0, 0))
        T = tris_of(g)
        if s < 0: T = [(b, a, c) for a, b, c in T]
        if not T: continue
        out.append(dict(P=P, N=N, U=U, T=T, tk=tk, b14=m['mat'][0x14], blend=m['mat'][0x1c:0x24]))
    return out, lo, hi

if __name__ == '__main__':
    props, textures = collect(sys.argv[1])
    print(len(props), 'props,', len(textures), 'textures')
    for k, (lvl, parts) in sorted(props.items()):
        ms, lo, hi = prop_meshes(parts)
        if max(hi[i] - lo[i] for i in range(3)) < 20: continue
        print(f'{k:40s} {lvl:24s} meshes {len(ms):3d}  size {hi[0]-lo[0]:7.0f} {hi[1]-lo[1]:7.0f} {hi[2]-lo[2]:7.0f}')
