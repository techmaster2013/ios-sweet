#!/usr/bin/env python3
"""Symbolize addresses inside an iOS 16 dyld shared cache (split cache).

  dsc_sym.py CACHE_DIR SLIDE ADDR [ADDR ...]

CACHE_DIR holds dyld_shared_cache_arm64 and its .01 ... and .symbols
subcaches. SLIDE is the shared-cache slide from a SWEET crash line
("CRASH name[pid] ... slide 0x..."), ADDR the pc/lr/backtrace values.
Prints image + nearest preceding symbol (local symbols from .symbols,
exported ones from each image's own symbol table).
"""
import bisect
import glob
import os
import struct
import sys


def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]


class Cache:
    def __init__(self, d):
        main = os.path.join(d, 'dyld_shared_cache_arm64')
        self.files = {}
        self.maps = []           # (vmaddr, size, fileoff, path)
        for p in [main] + sorted(glob.glob(main + '.[0-9]*')):
            if p.endswith('.symbols'):
                continue
            self.add(p)
        self.hdr = self.read_file(main, 0, 0x200)
        h = self.hdr
        img_off, img_cnt = u32(h, 0x1c0), u32(h, 0x1c4)
        if not img_cnt:
            img_off, img_cnt = u32(h, 0x18), u32(h, 0x1c)
        raw = self.read_file(main, img_off, img_cnt * 32)
        self.images = []
        for i in range(img_cnt):
            addr, path_off = u64(raw, i * 32), u32(raw, i * 32 + 24)
            self.images.append((addr, self.cstr_file(main, path_off)))
        self.images.sort()
        self.syms = None
        self.symfile = main + '.symbols'

    def add(self, p):
        h = open(p, 'rb').read(0x200)
        assert h[:7] == b'dyld_v1', p
        moff, mcnt = u32(h, 0x10), u32(h, 0x14)
        m = self.read_file(p, moff, mcnt * 32)
        for i in range(mcnt):
            a, s, fo = u64(m, i * 32), u64(m, i * 32 + 8), u64(m, i * 32 + 16)
            self.maps.append((a, s, fo, p))
        self.maps.sort()

    def read_file(self, p, off, n):
        f = self.files.get(p)
        if f is None:
            f = self.files[p] = open(p, 'rb')
        f.seek(off)
        return f.read(n)

    def cstr_file(self, p, off):
        b = self.read_file(p, off, 1024)
        return b[:b.index(b'\0')].decode()

    def read_vm(self, addr, n):
        for a, s, fo, p in self.maps:
            if a <= addr < a + s:
                return self.read_file(p, fo + addr - a, n)
        return None

    def image_of(self, addr):
        i = bisect.bisect_right([a for a, _ in self.images], addr) - 1
        return self.images[i] if i >= 0 else (0, '?')

    def image_symbols(self, base):
        """Exported/defined symbols from the image's LC_SYMTAB (in the cache's LINKEDIT)."""
        mh = self.read_vm(base, 0x8000)
        if not mh or u32(mh, 0) != 0xfeedfacf:
            return []
        ncmds, off = u32(mh, 16), 32
        linkedit = symtab = None
        for _ in range(ncmds):
            cmd, size = u32(mh, off), u32(mh, off + 4)
            if cmd == 0x19 and mh[off + 8:off + 24].rstrip(b'\0') == b'__LINKEDIT':
                linkedit = (u64(mh, off + 24), u64(mh, off + 40))        # vmaddr, fileoff
            elif cmd == 0x2:
                symtab = struct.unpack_from('<IIII', mh, off + 8)          # symoff nsyms stroff strsize
            off += size
        if not linkedit or not symtab:
            return []
        le_vm, le_fo = linkedit
        symoff, nsyms, stroff, strsize = symtab
        nl = self.read_vm(le_vm + symoff - le_fo, nsyms * 16) or b''
        st = self.read_vm(le_vm + stroff - le_fo, strsize) or b''
        out = []
        for i in range(len(nl) // 16):
            strx, ntype, sect, desc, val = struct.unpack_from('<IBBHQ', nl, i * 16)
            if (ntype & 0x0e) == 0x0e and val and strx < len(st):      # N_SECT
                e = st.find(b'\0', strx)
                out.append((val, st[strx:e].decode(errors='replace')))
        return out

    def local_symbols(self):
        """All local symbols from the .symbols subcache: {image vm offset: [(addr, name)]}."""
        if self.syms is not None:
            return self.syms
        self.syms = {}
        if not os.path.exists(self.symfile):
            return self.syms
        h = self.read_file(self.symfile, 0, 0x200)
        lo = u64(h, 0x48)
        info = self.read_file(self.symfile, lo, 24)
        nlo, nlc, so, ss, eo, ec = struct.unpack_from('<6I', info, 0)
        nl = self.read_file(self.symfile, lo + nlo, nlc * 16)
        st = self.read_file(self.symfile, lo + so, ss)
        ents = self.read_file(self.symfile, lo + eo, ec * 16)
        base = self.maps[0][0]
        for i in range(ec):
            dyoff, start, cnt = struct.unpack_from('<QII', ents, i * 16)
            lst = []
            for k in range(start, start + cnt):
                strx, ntype, sect, desc, val = struct.unpack_from('<IBBHQ', nl, k * 16)
                if val and strx < len(st):
                    e = st.find(b'\0', strx)
                    lst.append((val, st[strx:e].decode(errors='replace')))
            self.syms[base + dyoff] = lst
        return self.syms

    def symbolize(self, addr):
        base, path = self.image_of(addr)
        cands = self.image_symbols(base) + self.local_symbols().get(base, [])
        best = max((c for c in cands if c[0] <= addr), default=None)
        name = f'{best[1]}+0x{addr - best[0]:x}' if best else '?'
        return f'{path.rsplit("/", 1)[-1]}: {name}'


def main():
    c = Cache(sys.argv[1])
    slide = int(sys.argv[2], 16)
    for a in sys.argv[3:]:
        v = int(a, 16)
        print(f'{a} -> 0x{v - slide:x}  {c.symbolize(v - slide)}')


if __name__ == '__main__':
    main()
