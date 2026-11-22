#!/usr/bin/env python3
"""Reader for kaoforge_export/*.bin (written by the in-game export). load(path) -> (tex{id:dict}, geo{id:dict}, meshes[list])."""
import struct, sys
def load(path):
    d = open(path, 'rb').read(); assert d[:4] == b'KEXP'; o = 8
    tex, geo, meshes = {}, {}, []
    def u(): nonlocal o; v = struct.unpack_from('<I', d, o)[0]; o += 4; return v
    def blob(): nonlocal o; n = u(); b = d[o:o+n]; o += n; return b
    def s(): nonlocal o; n = struct.unpack_from('<H', d, o)[0]; o += 2; v = d[o:o+n].decode('latin1'); o += n; return v
    def arr(): n = u(); b = blob(); return n, b
    while o < len(d):
        t = d[o:o+1]; o += 1
        if t == b'T':
            i = u(); w, h, vw, vh, fmt = u(), u(), u(), u(), u()
            tex[i] = dict(w=w, h=h, vw=vw, vh=vh, fmt=fmt, data=blob(), pal=blob(), path=s())
        elif t == b'G':
            i = u(); nv = u()
            geo[i] = dict(nv=nv, strips=arr(), idx=arr(), pos=arr(), nrm=arr(), uv=arr())
        elif t == b'M':
            chain = s(); g, tx = u(), u()
            w = struct.unpack_from('<8f', d, o); o += 32
            bb = struct.unpack_from('<6f', d, o); o += 24
            fl = u(); mat = d[o:o+0x40]; o += 0x40
            meshes.append(dict(chain=chain, geo=g, tex=tx, world=w, bbox=bb, flags=fl, mat=mat))
        else: raise ValueError(f'bad record {t} @ {o-1:#x}')
    return tex, geo, meshes
if __name__ == '__main__':
    import collections
    tex, geo, ms = load(sys.argv[1])
    print(len(tex), 'textures', len(geo), 'geosets', len(ms), 'meshes')
    print(collections.Counter(t['fmt'] for t in tex.values()))
    prox = collections.Counter()
    for m in ms:
        ps = [seg for seg in m['chain'].split('/') if '|' in seg]
        prox[ps[-1].split('|')[1] if ps else '(no proxy) ' + '/'.join(x.split(':')[0] for x in m['chain'].split('/')[1:4])] += 1
    for k, v in prox.most_common(60): print(f'{v:5d} {k}')
