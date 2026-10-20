#!/usr/bin/env python3
"""Reproduce UFC-83's static AssetStream evidence without executing guest code.

python tools/re/ufc83_asset_task.py out/eboot.elf [--disassemble]
The disassembler receives the executable PT_LOAD at its virtual address, not
an offset into BBPROBE5's serialized header/import/segment tables.
"""
import argparse
import hashlib
import struct
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--disassemble', action='store_true')
    args = parser.parse_args()
    blob = args.elf.read_bytes()
    if blob[:6] != b'\x7fELF\x02\x01':
        parser.error('expected a little-endian ELF64')
    phoff = struct.unpack_from('<Q', blob, 32)[0]
    phsize, phnum = struct.unpack_from('<HH', blob, 54)
    segments = [struct.unpack_from('<IIQQQQQQ', blob, phoff + phsize*i)
                for i in range(phnum)]

    def read(va, size):
        for kind, _, offset, base, _, filesz, _, _ in segments:
            if kind in (1, 0x61000010) and base <= va and va+size <= base+filesz:
                return blob[offset+va-base:offset+va-base+size]
        raise ValueError(f'unmapped ELF address {va:#x}')

    def table(va, count):
        return struct.unpack('<'+'Q'*count, read(va, 8*count))

    print(f'ELF sha256={hashlib.sha256(blob).hexdigest()}')
    manager = table(0x25e2158, 1)[0]
    vtable = table(manager, 1)[0]
    assert manager == 0x25e2150 and vtable == 0x24b6108
    print(f'manager global +0x25e2158 -> +{manager:#x}, vtable +{vtable:#x}')
    for slot in (0x10, 0x30, 0x38):
        print(f'manager vtable+{slot:#x} -> +{table(vtable+slot, 1)[0]:#x}')
    request_vtable = 0x24b60b8
    assert table(request_vtable+0x20, 1)[0] == 0xe3c6f0
    print(f'whole-file request vtable +{request_vtable:#x}, translate +0xe3c6f0')
    for addr in (0x1f19b77, 0x1f19b89, 0x1f19b9d):
        print(f'+{addr:#x}: {read(addr, 32).split(bytes([0]))[0].decode()}')
    # Poll return and status publication: guards used by read-only sampler snapshots.
    assert read(0x1bab6eb, 5) == bytes.fromhex('e8502929ff')
    assert read(0xe3d1cc, 4) == bytes.fromhex('49894638')
    assert read(0xe42700, 10) == bytes.fromhex('0faee88b463883e00fc3')
    print('status = request[+0x38] & 15; publisher +0xe3d170/store +0xe3d1cc')
    # EAThread records *ScePthread after create, then looks up *self in sleep.
    assert read(0x46ee9, 14) == bytes.fromhex('488b4308488d7dc04c8b304c8933')
    assert read(0x43e89, 8) == bytes.fromhex('e85265db01488b10')
    assert read(0x470eb, 5) == bytes.fromhex('e810faffff')
    print('EAThread identity: create +0x46ee9 -> *handle saved at +0x46ef4; '
          'sleep +0x43e89 -> *self, lookup +0x297c350, fallback +0x297c338')
    if args.disassemble:
        text = next(s for s in segments if s[0] == 1 and s[1] & 1)
        with tempfile.NamedTemporaryFile() as f:
            f.write(blob[text[2]:text[2]+text[5]])
            f.flush()
            for lo, hi in [(0x1bab670, 0x1bab6f0), (0xe3a870, 0xe3a9dd),
                           (0xe3e050, 0xe3e0ed), (0xe42700, 0xe4270a),
                           (0xe3d170, 0xe3d1de), (0xe40800, 0xe40891),
                           (0xe40944, 0xe40973), (0xe40a61, 0xe40aa0),
                           (0xe3c6f0, 0xe3c7c0), (0xe3f1e0, 0xe3f37f),
                           (0x43d30, 0x43eea), (0x46e48, 0x46f16),
                           (0x46b34, 0x46b6f), (0x470c0, 0x470f0),
                           (0x471a0, 0x47230), (0x1dfa3e0, 0x1dfa3f0)]:
                subprocess.run(['objdump', '-D', '-b', 'binary', '-m', 'i386:x86-64',
                                '-Mintel', f'--adjust-vma={text[3]}',
                                f'--start-address={lo}', f'--stop-address={hi}', f.name],
                               check=True)


if __name__ == '__main__':
    main()
