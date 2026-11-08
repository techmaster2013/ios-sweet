#!/usr/bin/env python3
"""Raw a8r8g8b8 framebuffer dump (little endian: B G R A bytes) -> PNG.

  fb2png.py RAW OUT.png WIDTH HEIGHT [STRIDE_BYTES]

Standard library only (zlib), so it runs anywhere python3 does.
"""
import struct, sys, zlib


def main():
    raw_path, out_path = sys.argv[1], sys.argv[2]
    w, h = int(sys.argv[3]), int(sys.argv[4])
    stride = int(sys.argv[5]) if len(sys.argv) > 5 else w * 4
    data = open(raw_path, 'rb').read()
    rows = []
    for y in range(h):
        line = data[y * stride:y * stride + w * 4].ljust(w * 4, b'\0')
        rgb = bytearray(w * 3)
        rgb[0::3] = line[2::4]   # R
        rgb[1::3] = line[1::4]   # G
        rgb[2::3] = line[0::4]   # B
        rows.append(b'\0' + bytes(rgb))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(b''.join(rows), 6))
           + chunk(b'IEND', b''))
    open(out_path, 'wb').write(png)
    print(f'{out_path}: {w}x{h}', file=sys.stderr)


if __name__ == '__main__':
    main()
