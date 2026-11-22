#!/usr/bin/env python3
"""Copies a level PAK under a new name: renames the level in the header and in the 'item' paths
(name fields only, file contents unchanged). Optionally replaces the data of the .ar item with another file.
Usage: tools_pak_rename.py src.PAK dst.PAK old_name new_name [new_file.ar]"""
import sys, struct

src, dst, old, new = sys.argv[1:5]
new_ar = sys.argv[5] if len(sys.argv) > 5 else None
d = bytearray(open(src, 'rb').read())
assert d[:4] == b'TATE'

def setname(off, length, s):
    b = s.encode('latin1')
    assert len(b) < length, s
    d[off:off+length] = b + b'\0' * (length - len(b))

assert d[16:48].split(b'\0')[0].decode() == old
setname(16, 0x30, new)

out = bytearray()
off = 0x80
items = []
while off + 0x80 <= len(d) and d[off:off+4] == b'item':
    size = struct.unpack_from('<I', d, off + 4)[0]
    items.append((off, size))
    off = (off + 0x80 + size + 0x7f) & ~0x7f
tail_start = off

renamed = 0
for o, size in items:
    name = d[o+16:o+0x80].split(b'\0')[0].decode('latin1')
    if name in (f'levels/{old}/loading.bmp', f'levels/{old}/misc.ini', f'build/win32/{old}.ar'):
        setname(o + 16, 0x70, name.replace(old, new))
        renamed += 1
        print('rename', name, '->', name.replace(old, new))

if new_ar:
    # rebuild the file with the new .ar content. PAK layout: header, main section ('item' entries),
    # aligned to 64 KB; u16 @6 = main section size in 64 KB blocks; then the language sections
    # (table @0x40: code + u32 + u16 blocks) - copied unchanged.
    ar = open(new_ar, 'rb').read()
    main_blocks = struct.unpack_from('<H', d, 6)[0]
    tail_start = main_blocks * 0x10000
    assert tail_start >= off, (hex(tail_start), hex(off))
    res = bytearray(d[:0x80])
    for o, size in items:
        hdr = bytearray(d[o:o+0x80])
        name = hdr[16:].split(b'\0')[0].decode('latin1')
        data = d[o+0x80:o+0x80+size]
        if name == f'build/win32/{new}.ar':
            data = ar
            struct.pack_into('<I', hdr, 4, len(ar))
        res += hdr + data
        res += b'\0' * ((-len(res)) % 0x80)
    res += b'\0' * ((-len(res)) % 0x10000)
    struct.pack_into('<H', res, 6, len(res) // 0x10000)
    print('main blocks', main_blocks, '->', len(res) // 0x10000)
    res += d[tail_start:]
    d = res

open(dst, 'wb').write(d)
print('items', len(items), 'renamed', renamed, 'size', len(d))
