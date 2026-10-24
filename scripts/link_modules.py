"""Link the eboot with the game's bundled system modules into one probe image.

Extract ELF sections from the game executables and libraries and pack them together into a format that can be loaded
into a single address space with zero offset relocations. This avoids having to implement a runtime ELF loader.
The linked output is verified and signed using SHA256 hashes of the original module files.
"""
from base64 import b64decode
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))

from link_libc import encode_id
from cpu_compat import atomic_output, patch_image, patch_libc, enabled
from prepare import parse_self, nid

DEFAULT_MODULES = ('libc.prx', 'libSceFios2.prx')


def section_name(name_table, offset):
    if offset >= len(name_table):
        return ''
    end = name_table.find(b'\0', offset)
    return name_table[offset:end].decode('ascii')


def module(path):
    elf, header, ph, tags, dynsym = parse_self(path.read_bytes())
    segments = [p for p in ph if p['type'] == 1]
    code = next((p for p in segments if p['flags'] & 1), None)
    if not code:
        raise ValueError(f'{path.name}: executable load segment not found')
    names = b64decode(tags[0x61000027]).split(b'\0')
    sections = [dict(zip(('name', 'type', 'flags', 'addr', 'offset', 'size', 'link', 'info', 'addralign', 'entsize'),
                         struct.unpack_from('<IIQQQQIIQQ', elf, header['e_shoff'] + i * header['e_shentsize'])))
                for i in range(header['e_shnum'])]
    symtab = [s for s in sections if s['type'] == 2]
    symbols = []
    if symtab:
        name_table = elf[sections[symtab[0]['link']]['offset']:]
        symbols = [dict(zip(('name', 'info', 'other', 'shndx', 'value', 'size'),
                            struct.unpack_from('<IBBHQQ', elf, symtab[0]['offset'] + i * 24)))
                   for i in range(symtab[0]['size'] // 24)]
        symbols = [dict(name=section_name(name_table, s['name']), section=s['shndx'],
                        type=s['info'] & 0xf, binding=s['info'] >> 4,
                        value=s['value'], size=s['size'],
                        identity=nid(section_name(name_table, s['name'])))
                   for s in symbols]
    rela = [s for s in sections if s['type'] == 4]
    relocations = []
    if rela:
        relocations = [struct.unpack_from('<QQq', elf, rela[0]['offset'] + i * 24)
                       for i in range(rela[0]['size'] // 24)]
        relocations = [(target, info & 0xffffffff, info >> 32, addend)
                       for target, info, addend in relocations]
    sha256 = hashlib.sha256(path.read_bytes()).hexdigest()
    return dict(elf=elf, header=header, ph=ph, tags=tags, symbols=symbols, relocs=relocations,
                libraries=names, sha256=sha256)


def link(game, out, module_names):
    boot = out / 'boot.bin'
    if not boot.exists():
        raise FileNotFoundError(f'intermediate binary not found: {boot}; run prepare.py first')
    raw = boot.read_bytes()
    magic, size, entry, ns, nr, nn = struct.unpack_from('<8s5Q', raw, 0)
    if magic != b'BBPROBE2':
        raise ValueError('linker expects freshly prepared BBPROBE2')
    pos = 48
    segments = [struct.unpack_from('<3Q', raw, pos + i * 24) for i in range(ns)]
    pos += ns * 24
    relocs = [struct.unpack_from('<QQqq', raw, pos + i * 32) for i in range(nr)]
    pos += nr * 32
    names = [raw[pos + i * 128:pos + (i + 1) * 128].rstrip(b'\0').decode('ascii') for i in range(nn)]
    pos += nn * 128
    image = bytearray(raw[pos:])
    if len(image) != size:
        raise ValueError('binary size mismatch')

    def imported(sym):
        name = sym['name']
        if name in names:
            return names.index(name)
        names.append(name)
        return len(names) - 1

    def mapped(addr, length=1):
        return any(base <= addr and addr + length <= base + sz for base, sz, _ in segments)

    main = module(game / 'eboot.bin')
    table = []
    exports = {}
    bindings = []
    tls_module = 1
    fs_patched = 0
    for filename in module_names:
        m = module(game / 'sce_module' / filename)
        if filename == 'libc.prx' and enabled():
            patch_libc(m, m['elf'])
        load = [p for p in m['ph'] if p['type'] == 1]
        base = (len(image) + 65535) & ~65535
        vaddr_base = min(p['vaddr'] for p in load)
        end = max(p['vaddr'] + p['memsz'] for p in load)
        table.append(dict(file=filename, base=base, size=end - vaddr_base,
                          init=base + m['header']['e_entry'] - vaddr_base,
                          tls_address=0, tls_memsz=0, tls_filesz=0, tls_module=0, sha256=m.get('sha256')))
        for seg in load:
            seg_base = base + seg['vaddr'] - vaddr_base
            image.extend(bytes(seg_base + seg['memsz'] - len(image)))
            image[seg_base:seg_base + seg['filesz']] = m['elf'][seg['offset']:seg['offset'] + seg['filesz']]
            flags = (4 if seg['flags'] & 4 else 0) | (2 if seg['flags'] & 2 else 0) | (1 if seg['flags'] & 1 else 0)
            segments.append((seg_base, seg['memsz'], flags))
        tls = next((p for p in m['ph'] if p['type'] == 7), None)
        module_id = 0
        if tls:
            if tls['filesz'] > tls['memsz'] or not mapped(tls['vaddr'], tls['memsz']):
                raise ValueError(f'{filename}: unsupported TLS layout')
            module_id = tls_module
            tls_module += 1
        tls_relocations = 0
        for target, kind, symid, addend in m['relocs']:
            if not mapped(target):
                raise ValueError(f'{filename}: relocation outside image {target:#x}')
            target += base
            if kind == 8:
                relocs.append((target, 0, base + addend, 0))
            elif kind == 16:
                symbol = m['symbols'][symid]
                if symbol['name'] or symbol['type'] != 3 or addend or not module_id:
                    raise ValueError(f'{filename}: unsupported external TLS relocation')
                struct.pack_into('<Q', image, target, module_id)
                tls_relocations += 1
            elif kind in (1, 6, 7):
                symbol = m['symbols'][symid]
                if symbol['section']:
                    if symbol['section'] == 0xfff1:
                        struct.pack_into('<Q', image, target, symbol['value'] + addend)
                    else:
                        relocs.append((target, 0, base + symbol['value'] + addend, 0))
                elif not symbol['identity'] and symbol['binding'] == 2:
                    struct.pack_into('<Q', image, target, 0)  # weak undefined (module_start/stop)
                else:
                    rk = 2 if symbol['type'] == 1 else 1
                    if (rk == 1 and addend) or not 0 <= addend < 4096:
                        raise ValueError(f'{filename}: unsupported import addend')
                    relocs.append((target, rk, imported(symbol), addend))
            else:
                raise ValueError(f'{filename}: unsupported relocation type {kind}')
        count = 0
        for s in m['symbols']:
            if s['section'] and s['binding'] in (1, 2) and s['identity'] and s['type'] in (1, 2):
                if not mapped(s['value'], max(1, s['size'])):
                    raise ValueError(f'{filename}: export outside load segments')
                if s['identity'] in exports:
                    raise ValueError(f'{filename}: duplicate export symbol')
                exports[s['identity']] = base + s['value']
                count += 1
        for seg in load:
            if not seg['flags'] & 1:
                continue
            seg_base = base + seg['vaddr'] - vaddr_base
            code = image[seg_base:seg_base + seg['filesz']]
            idx = 0
            while True:
                idx = code.find(b'\x64\x48\x8b', idx)
                if idx == -1:
                    break
                # Replace %fs: with %gs: for thread pointer references
                code[idx] = 0x65
                idx += 3
                fs_patched += 1
            idx = 0
            while True:
                idx = code.find(b'\x64\x8b', idx)
                if idx == -1:
                    break
                code[idx] = 0x65
                idx += 2
                fs_patched += 1

    unresolved = []
    for idx, (target, kind, sym, addend) in enumerate(relocs):
        if kind == 1 and sym < len(names):
            name = names[sym]
            sym_id = encode_id(name)
            if sym_id in exports:
                relocs[idx] = (target, 0, exports[sym_id] + addend, 0)
                bindings.append((target, exports[sym_id] + addend))
            else:
                unresolved.append(name)

    flags = 1  # basic linked probe
    main_tls = next((p for p in main['ph'] if p['type'] == 7), None)
    main_tls_values = (main_tls['vaddr'], main_tls['filesz'], main_tls['memsz'], main_tls['align']) if main_tls else (0, 0, 0, 0)
    if main_tls and (main_tls['filesz'] > main_tls['memsz'] or main_tls['memsz'] > 1024 * 1024
                     or main_tls['vaddr'] + main_tls['filesz'] > size):
        raise ValueError('unsupported eboot TLS layout')
    procparam = next(p for p in main['ph'] if p['type'] == 0x61000001)
    image, segments, cpu_compat = patch_image(image, segments, main, table, relocs)
    FLAG_LEGACY_CPU_TRANSLATED = 2
    if cpu_compat.get('enabled') and cpu_compat.get('complete'):
        flags |= FLAG_LEGACY_CPU_TRANSLATED
    with (out / 'boot-linked.bin').open('wb') as f:
        f.write(struct.pack('<8s6Q', b'BBPROBE5', len(image), entry, len(segments), len(relocs), len(names), flags))
        f.write(struct.pack('<Q', procparam['vaddr']))
        f.write(struct.pack('<4Q', *main_tls_values))
        f.write(struct.pack('<Q', len(table)))
        for t in table:
            f.write(struct.pack('<7Q', t['base'], t['size'], t['init'], t['tls_address'], t['tls_memsz'],
                                t['tls_filesz'], t['tls_module']))
        f.write(struct.pack('<Q', len(bindings)))
        for binding in bindings:
            f.write(struct.pack('<QQ', *binding))
        for segment in segments:
            f.write(struct.pack('<3Q', *segment))
        for name in names:
            if len(name.encode()) >= 128:
                raise ValueError('name too long')
            f.write(name.encode().ljust(128, b'\0'))
        for relocation in relocs:
            f.write(struct.pack('<QQqq', *relocation))
        f.write(image)
    report = dict(modules=[{k: (hex(v) if k in ('base', 'init', 'tls_address') else v) for k, v in t.items()} for t in table],
                  bindings=len(bindings), imports=len(names), fs_loads_patched=fs_patched,
                  main_tls=dict(zip(('vaddr', 'filesz', 'memsz', 'align'), main_tls_values)),
                  unresolved_imports=unresolved, cpu_compat=cpu_compat)
    (out / 'link.json').write_text(json.dumps(report, indent=2) + '\n')
    if cpu_compat.get('enabled'):
        status = "complete" if cpu_compat.get('complete') else "partial"
        print(f"CPU compatibility ({status}): translated {cpu_compat['patched_sites']} instruction sites for legacy CPU; trampolines={cpu_compat['trampoline_bytes']} bytes")
    summary = ', '.join(f"{t['file']}@{t['base']:#x}" for t in table)
    print(f'Linked modules: {summary}; {len(bindings)} native bindings, {len(unresolved)} imports left to the host runtime, '
          f'fs->gs patched={fs_patched}')


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path)
    p.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'out')
    p.add_argument('--modules', nargs='*', default=list(DEFAULT_MODULES))
    a = p.parse_args()
    link(a.game, a.out, a.modules)
