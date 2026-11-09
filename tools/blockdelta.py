#!/usr/bin/env python3
"""Make a block delta between two equally sized images (changed 4 KiB blocks only).

  blockdelta.py OLD.img NEW.img OUT.delta

OUT.delta: "SWEETDLT" | u32 version 1 | u32 block size | u64 image size |
u64 count, then count x (u64 byte offset, block bytes). Applied by
tools/sd-delta-write.ps1 (writes each block into SD partition 3 and reads it
back). Small edits of a 16 GiB root become a few hundred KiB instead of a
full rewrite, and the volume keeps its state (no fresh first boot).
"""
import struct
import sys

BLOCK = 4096
CHUNK = 1 << 20


def main():
    old_p, new_p, out_p = sys.argv[1:4]
    old, new = open(old_p, 'rb'), open(new_p, 'rb')
    size = new.seek(0, 2)
    if old.seek(0, 2) != size:
        sys.exit('images differ in size')
    old.seek(0)
    new.seek(0)
    blocks = []
    pos = 0
    while pos < size:
        a, b = old.read(CHUNK), new.read(CHUNK)
        if a != b:
            for i in range(0, len(b), BLOCK):
                if a[i:i + BLOCK] != b[i:i + BLOCK]:
                    blocks.append((pos + i, b[i:i + BLOCK]))
        pos += len(b)
    with open(out_p, 'wb') as out:
        out.write(struct.pack('<8sIIQQ', b'SWEETDLT', 1, BLOCK, size, len(blocks)))
        for off, data in blocks:
            out.write(struct.pack('<Q', off))
            out.write(data)
    print(f'{out_p}: {len(blocks)} changed blocks ({len(blocks) * BLOCK // 1024} KiB) of {size // BLOCK}')


if __name__ == '__main__':
    main()
