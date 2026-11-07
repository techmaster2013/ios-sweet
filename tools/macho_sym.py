#!/usr/bin/env python3
"""Symbolize kernel addresses with the Mach-O symbol table (no Apple tools needed).

  macho_sym.py KERNEL ADDR [ADDR ...]

Prints the nearest preceding symbol + offset for each address (hex, with or
without 0x). The payload loads the kernel unslid, so panic-log addresses and
link addresses match.
"""
import bisect, struct, sys

LC_SYMTAB = 0x2


def symbols(path):
    data = open(path, 'rb').read()
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', data, 0)
    assert magic == 0xfeedfacf, 'not a 64-bit Mach-O'
    off = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == LC_SYMTAB:
            symoff, nsyms, stroff, _ = struct.unpack_from('<IIII', data, off + 8)
            out = []
            for i in range(nsyms):
                strx, ntype, nsect, _, value = struct.unpack_from('<IBBhQ', data, symoff + 16 * i)
                if ntype & 0xe0 or nsect == 0 or value == 0:   # skip stabs and undefined
                    continue
                end = data.index(b'\0', stroff + strx)
                out.append((value, data[stroff + strx:end].decode(errors='replace')))
            out.sort()
            return out
        off += size
    raise SystemExit('no LC_SYMTAB')


def main():
    syms = symbols(sys.argv[1])
    addrs = [v for v, _ in syms]
    for a in sys.argv[2:]:
        x = int(a, 16)
        i = bisect.bisect_right(addrs, x) - 1
        print(f'{x:#x}  ' + (f'{syms[i][1]}+{x - syms[i][0]:#x}' if i >= 0 else '?'))


if __name__ == '__main__':
    main()
