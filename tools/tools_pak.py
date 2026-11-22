#!/usr/bin/env python3
"""Lister/extractor for Kao2 PAK files. Usage: tools_pak.py file.PAK [output_dir]"""
import sys, struct, os
d = open(sys.argv[1], 'rb').read()
out = sys.argv[2] if len(sys.argv) > 2 else None
assert d[:4] == b'TATE'
print('hdr', d[4:16].hex(), 'name', d[16:48].split(b'\0')[0])
off = 0x80
n = 0
while off + 0x80 <= len(d):
    tag = d[off:off+4]
    if tag != b'item':
        print('stop at', hex(off), tag); break
    size = struct.unpack_from('<I', d, off+4)[0]
    rest = d[off+8:off+16].hex()
    name = d[off+16:off+0x80].split(b'\0')[0].decode('latin1')
    print(f'{off:#010x} {size:10d} {rest} {name}')
    if out:
        p = os.path.join(out, os.path.normpath(name.replace('\\', '/').lstrip('/')))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'wb').write(d[off+0x80:off+0x80+size])
    off += 0x80 + size
    off = (off + 0x7f) & ~0x7f
    n += 1
print('entries', n, 'end', hex(off), 'len', hex(len(d)))
