#!/usr/bin/env python3
"""Ad-hoc code signature with entitlements for a thin arm64 Mach-O (like ldid -S).

  adhoc_sign.py BINARY ENTITLEMENTS.plist [IDENTIFIER] [--bundle APP_DIR]

Replaces the binary's existing code signature (LC_CODE_SIGNATURE must exist:
build with any signer first, e.g. xtool --sign) with a SuperBlob of:
CodeDirectory (SHA-256, 4 KiB pages, flags adhoc), empty Requirements,
XML Entitlements and DER Entitlements. With --bundle, Info.plist and
_CodeSignature/CodeResources (if present) are hashed into the special slots.
The new signature must fit the existing LC_CODE_SIGNATURE space (an ad-hoc
blob without a CMS signature is smaller than a developer one).
"""
import hashlib
import os
import plistlib
import struct
import sys

CSMAGIC_EMBEDDED_SIGNATURE = 0xfade0cc0
CSMAGIC_CODEDIRECTORY = 0xfade0c02
CSMAGIC_REQUIREMENTS = 0xfade0c01
CSMAGIC_EMBEDDED_ENTITLEMENTS = 0xfade7171
CSMAGIC_EMBEDDED_DER_ENTITLEMENTS = 0xfade7172
CSSLOT_CODEDIRECTORY, CSSLOT_REQUIREMENTS, CSSLOT_ENTITLEMENTS, CSSLOT_DER = 0, 2, 5, 7
CS_ADHOC = 0x2
PAGE_SHIFT = 12


def der_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, 'big')
    return bytes([0x80 | len(b)]) + b


def der(tag, body):
    return bytes([tag]) + der_len(len(body)) + body


def der_value(v):
    if isinstance(v, bool):
        return der(0x01, b'\xff' if v else b'\x00')
    if isinstance(v, int):
        b = v.to_bytes(max(1, (v.bit_length() + 8) // 8), 'big', signed=True)
        return der(0x02, b)
    if isinstance(v, str):
        return der(0x0c, v.encode())
    if isinstance(v, list):
        return der(0x30, b''.join(der_value(x) for x in v))
    if isinstance(v, dict):
        # DER SET (0x31) of SEQUENCE{UTF8String key, value}, keys sorted (Apple's format).
        items = [der(0x30, der(0x0c, k.encode()) + der_value(v[k])) for k in sorted(v)]
        return der(0x31, b''.join(items))
    raise TypeError(type(v))


def der_entitlements(ents):
    # [APPLICATION 16] { INTEGER 1, dict }
    return der(0x70, der(0x02, b'\x01') + der_value(ents))


def blob(magic, payload):
    return struct.pack('>II', magic, 8 + len(payload)) + payload


def code_directory(code, ident, special, text_limit):
    """special: {slot: bytes hash}; code: bytes up to codeLimit."""
    nspecial = max(special) if special else 0
    npages = (len(code) + (1 << PAGE_SHIFT) - 1) >> PAGE_SHIFT
    hashes = [hashlib.sha256(code[i << PAGE_SHIFT:(i + 1) << PAGE_SHIFT]).digest() for i in range(npages)]
    ident_b = ident.encode() + b'\0'
    version = 0x20400                       # with execSeg fields
    header_len = 9 * 4 + 4 + 4 * 4 + 8 + 3 * 8     # 88 bytes, through execSegFlags
    # magic,length,version,flags,hashOffset,identOffset,nSpecialSlots,nCodeSlots,codeLimit,
    # hashSize/hashType/platform/pageSize (4 bytes),spare2,scatterOffset,teamOffset,spare3,codeLimit64,
    # execSegBase,execSegLimit,execSegFlags
    ident_off = header_len
    hash_off = ident_off + len(ident_b) + 32 * nspecial
    body = bytearray()
    body += struct.pack('>IIIIIIIIIBBBBIIIIQQQQ',
                        CSMAGIC_CODEDIRECTORY, 0, version, CS_ADHOC,
                        hash_off, ident_off, nspecial, npages, len(code),
                        32, 2, 0, PAGE_SHIFT,               # SHA-256
                        0, 0, 0, 0, 0,                      # spare2, scatter, team, spare3, codeLimit64
                        0, text_limit, 1)                   # execSeg base/limit/flags (main binary)
    assert len(body) == header_len, (len(body), header_len)
    body += ident_b
    for slot in range(nspecial, 0, -1):
        body += special.get(slot, b'\0' * 32)
    for h in hashes:
        body += h
    struct.pack_into('>I', body, 4, len(body))
    return bytes(body)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    bundle = None
    if '--bundle' in sys.argv:
        bundle = sys.argv[sys.argv.index('--bundle') + 1]
        args = [a for a in args if a != bundle]
    path, ent_path = args[0], args[1]
    ident = args[2] if len(args) > 2 else os.path.basename(path)

    data = bytearray(open(path, 'rb').read())
    magic, cputype, _, _, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', data, 0)
    if magic != 0xfeedfacf:
        sys.exit('not a thin 64-bit Mach-O (fat binaries: thin them first)')
    off = 32
    sig = None
    linkedit = None
    text_limit = 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == 0x1d:                     # LC_CODE_SIGNATURE
            sig = (off, *struct.unpack_from('<II', data, off + 8))
        elif cmd == 0x19 and data[off + 8:off + 24].rstrip(b'\0') == b'__TEXT':
            text_limit = struct.unpack_from('<Q', data, off + 48)[0]    # filesize
        elif cmd == 0x19 and data[off + 8:off + 24].rstrip(b'\0') == b'__LINKEDIT':
            linkedit = off
        off += size
    if not sig:
        sys.exit('no LC_CODE_SIGNATURE: sign it once first')
    _, dataoff, datasize = sig

    ents_xml = open(ent_path, 'rb').read()
    ents = plistlib.loads(ents_xml)
    xml = plistlib.dumps(ents, fmt=plistlib.FMT_XML)
    req = blob(CSMAGIC_REQUIREMENTS, struct.pack('>I', 0))      # empty requirement set
    ent_blob = blob(CSMAGIC_EMBEDDED_ENTITLEMENTS, xml)
    der_blob = blob(CSMAGIC_EMBEDDED_DER_ENTITLEMENTS, der_entitlements(ents))

    special = {
        CSSLOT_REQUIREMENTS: hashlib.sha256(req).digest(),
        CSSLOT_ENTITLEMENTS: hashlib.sha256(ent_blob).digest(),
        CSSLOT_DER: hashlib.sha256(der_blob).digest(),
    }
    if bundle:
        info = os.path.join(bundle, 'Info.plist')
        if os.path.exists(info):
            special[1] = hashlib.sha256(open(info, 'rb').read()).digest()
        res = os.path.join(bundle, '_CodeSignature', 'CodeResources')
        if os.path.exists(res):
            special[3] = hashlib.sha256(open(res, 'rb').read()).digest()

    def superblob():
        cd = code_directory(bytes(data[:dataoff]), ident, special, text_limit)
        entries = [(CSSLOT_CODEDIRECTORY, cd), (CSSLOT_REQUIREMENTS, req),
                   (CSSLOT_ENTITLEMENTS, ent_blob), (CSSLOT_DER, der_blob)]
        out = bytearray()
        pos = 12 + 8 * len(entries)
        index = b''
        for slot, b in entries:
            index += struct.pack('>II', slot, pos)
            pos += len(b)
        out += struct.pack('>III', CSMAGIC_EMBEDDED_SIGNATURE, pos, len(entries)) + index
        for _, b in entries:
            out += b
        return out

    out = superblob()
    if len(out) > datasize:
        if dataoff + datasize != len(data) or linkedit is None:
            sys.exit(f'new signature ({len(out)} bytes) does not fit the old one ({datasize})')
        # The signature ends the file (a fresh linker signature): grow it, and
        # __LINKEDIT with it. The load commands are hashed, so patch them first.
        grow = ((len(out) + 15) & ~15) - datasize
        datasize += grow
        struct.pack_into('<I', data, sig[0] + 12, datasize)
        lo = linkedit
        filesize = struct.unpack_from('<Q', data, lo + 48)[0] + grow
        struct.pack_into('<Q', data, lo + 48, filesize)
        struct.pack_into('<Q', data, lo + 32, (filesize + 0x3fff) & ~0x3fff)     # vmsize
        data += b'\0' * grow
        out = superblob()
    data[dataoff:dataoff + datasize] = bytes(out) + b'\0' * (datasize - len(out))
    open(path, 'wb').write(data)
    print(f'{path}: ad-hoc signed as {ident}, {len(ents)} entitlements, '
          f'{(dataoff + 4095) // 4096} pages, signature {len(out)}/{datasize} bytes')


if __name__ == '__main__':
    main()
