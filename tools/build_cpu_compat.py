"""Generate Ivy Bridge compatibility plan for UFC 3 PS4 Port (CUSA06534)."""
import argparse
import base64
import collections
import hashlib
import json
import lzma
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))

try:
    import capstone
    import keystone
except ImportError:
    print("ERROR: python packages 'capstone' and 'keystone-engine' are required to generate the plan.")
    print("Run: pip install capstone keystone-engine")
    sys.exit(1)

from link_modules import module
from cpu_compat import atomic_output, SUPPORTED
from prepare import parse_self, nid

def function_ranges(mod):
    functions = set()
    header = next((p for p in mod['ph'] if p['type'] == 0x6474e550), None)
    if header:
        offset, addr = header['offset'], header['vaddr']
        count = struct.unpack_from('<I', mod['elf'], offset + 8)[0]
        for idx in range(count):
            start, fde = struct.unpack_from('<ii', mod['elf'], offset + 12 + idx * 8)
            fde_offset = next((p['offset'] + (addr + fde) - p['vaddr'] for p in mod['ph'] if p['type'] == 1 and p['vaddr'] <= addr + fde < p['vaddr'] + p['filesz']), None)
            if fde_offset:
                size = struct.unpack_from('<I', mod['elf'], fde_offset + 12)[0]
                if 0 < size < 4 * 1024**2:
                    functions.add((addr + start, size))
    for s in mod['symbols']:
        if s['type'] == 2 and s['section'] and s['size']:
            functions.add((s['value'], s['size']))
    return sorted(functions)

ASSEMBLER = keystone.Ks(keystone.KS_ARCH_X86, keystone.KS_MODE_64)

def canonical(reg):
    names = {'eax': 'rax', 'ebx': 'rbx', 'ecx': 'rcx', 'edx': 'rdx', 'esi': 'rsi',
             'edi': 'rdi', 'ebp': 'rbp', 'esp': 'rsp', 'ax': 'rax', 'bx': 'rbx',
             'cx': 'rcx', 'dx': 'rdx', 'si': 'rsi', 'di': 'rdi', 'bp': 'rbp', 'sp': 'rsp',
             'al': 'rax', 'bl': 'rbx', 'cl': 'rcx', 'dl': 'rdx', 'sil': 'rsi', 'dil': 'rdi',
             'bpl': 'rbp', 'spl': 'rsp', 'ah': 'rax', 'bh': 'rbx', 'ch': 'rcx', 'dh': 'rdx'}
    if reg in names:
        return names[reg]
    if reg.startswith('r') and reg[1:].isdigit():
        return reg
    if reg.startswith('r') and len(reg) >= 3 and reg[1:-1].isdigit() and reg[-1] in ('d', 'w', 'b'):
        return reg[:-1]
    return reg

def word(reg, width):
    if width == 64: return reg
    if width == 16:
        return {'rax': 'ax', 'rbx': 'bx', 'rcx': 'cx', 'rdx': 'dx', 'rsi': 'si',
                'rdi': 'di', 'rbp': 'bp', 'rsp': 'sp'}.get(reg, reg + 'w')
    return {'rax': 'eax', 'rbx': 'ebx', 'rcx': 'ecx', 'rdx': 'edx', 'rsi': 'esi',
            'rdi': 'edi', 'rbp': 'ebp', 'rsp': 'esp'}.get(reg, reg + 'd')

def movbe_body(operands):
    args = operands.split(', ')
    memory_idx = 0 if '[' in args[0] else 1
    memory = re.fullmatch(r'(word|dword|qword) ptr \[([a-z0-9x +*\-]+)\]', args[memory_idx])
    width = {'word': 16, 'dword': 32, 'qword': 64}[memory[1]]
    reg = canonical(args[1 - memory_idx])
    addr_regs = set(canonical(r) for r in re.findall(r'\b(?:r(?:1[0-5]|[8-9])[bwd]?|r(?:ax|bx|cx|dx|si|di|bp|sp)|e(?:ax|bx|cx|dx|si|di|bp|sp)|[a-d][xhl]|[sbd]pl|[sbd]il)\b', memory[2]))
    pool = ('r10', 'r11', 'r8', 'r9', 'rax', 'rdx', 'rsi', 'rdi', 'rbx', 'r12', 'r13', 'r14', 'r15', 'rbp')
    scratch = next(r for r in pool if r != reg and r not in addr_regs)
    temp = word(scratch, width)
    depth = 128 + 8 + (8 if width == 16 else 0)
    adjusted = list(args)
    adjusted[memory_idx] = re.sub(r'\brsp\b', f'rsp + {depth}', args[memory_idx])
    lines = ['lea rsp, [rsp-128]', f'push {scratch}']
    if width == 16: lines.append('pushfq')
    lines += [f'mov {temp}, {adjusted[1]}',
              f'ror {temp}, 8' if width == 16 else f'bswap {temp}',
              f'mov {adjusted[0]}, {temp}']
    if width == 16: lines.append('popfq')
    lines += [f'pop {scratch}', 'lea rsp, [rsp+128]']
    return bytes(ASSEMBLER.asm('; '.join(lines))[0])

def body(mnemonic, operands):
    if mnemonic == 'movbe':
        return movbe_body(operands)
    args = operands.split(', ')
    dest = canonical(args[0])
    width = next(w for w in (16, 32, 64) if args[0] == word(dest, w))
    all_regs = re.findall(r'\b(?:r(?:1[0-5]|[8-9])[bwd]?|r(?:ax|bx|cx|dx|si|di|bp|sp)|e(?:ax|bx|cx|dx|si|di|bp|sp)|[a-d][xhl]|[sbd]pl|[sbd]il)\b', operands)
    orig = {canonical(r) for r in all_regs}
    pool = ('r10', 'r11', 'r8', 'r9', 'rax', 'rdx', 'rsi', 'rdi', 'rbx', 'r12', 'r13', 'r14', 'r15', 'rbp')
    temp = [r for r in pool if r not in orig and r != 'rcx'][:3]
    first, control, mask = [word(r, width) for r in temp]
    saved = temp + (['rcx'] if mnemonic == 'bextr' and dest != 'rcx' else [])
    adjusted = [re.sub(r'\brsp\b', f'rsp + {128 + 8 * len(saved)}', arg) if '[' in arg else arg for arg in args]
    lines = ['lea rsp, [rsp-128]'] + [f'push {r}' for r in saved]
    lines += [f'mov {first}, {adjusted[1]}']

    if mnemonic == 'andn':
        lines += [f'not {first}', f'and {first}, {adjusted[2]}']
    elif mnemonic in ('blsr', 'blsi', 'blsmsk'):
        lines += [f'test {first}, {first}', 'jz zero']
        if mnemonic == 'blsr':
            lines += [f'lea {mask}, [{temp[0]}-1]', f'and {first}, {mask}']
        elif mnemonic == 'blsi':
            lines += [f'mov {mask}, {first}', f'neg {first}', f'and {first}, {mask}', 'stc']
        else:
            lines += [f'lea {mask}, [{temp[0]}-1]', f'xor {first}, {mask}']
        lines += ['jmp done', 'zero:', f'xor {first}, {first}']
    elif mnemonic == 'bextr':
        lines += [f'mov {word(temp[1], 32)}, {word(canonical(args[2]), 32)}',
                  f'mov ecx, {word(temp[1], 32)}', 'and ecx, 255', f'cmp ecx, {width}',
                  'jae zero', f'shr {first}, cl', f'mov ecx, {word(temp[1], 32)}',
                  'shr ecx, 8', 'and ecx, 255', f'cmp ecx, {width}', 'jae nomask',
                  f'mov {mask}, -1', f'shl {mask}, cl', f'not {mask}',
                  f'and {first}, {mask}', 'jmp done', 'nomask:',
                  f'test {first}, {first}', 'jmp done', 'zero:', f'xor {first}, {first}']
    elif mnemonic in ('tzcnt', 'lzcnt'):
        lines += [f"{'bsf ' if mnemonic == 'tzcnt' else 'bsr '} {mask}, {first}", 'jz zero']
        if mnemonic == 'tzcnt':
            lines += [f'mov {first}, {mask}']
        else:
            lines += [f'mov {first}, {width - 1}', f'sub {first}, {mask}']
        lines += [f'test {first}, {first}', 'jmp done', 'zero:',
                  f'mov {first}, {width}', f'test {first}, {first}', 'stc']
    lines += ['done:', f'mov {args[0]}, {first}']
    lines += [f'pop {r}' for r in reversed(saved)] + ['lea rsp, [rsp+128]']
    assembled, _ = ASSEMBLER.asm('; '.join(lines))
    return bytes(assembled)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path, help='Folder game UFC 3 (CUSA06534)')
    p.add_argument('--out', type=Path, default=ROOT / 'tools/legacy-cpu-plan.json')
    args = p.parse_args()

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    plan = {'version': 2, 'modules': {}, 'sites': [], 'unhandled': []}
    counts = collections.Counter()

    # 1. PRX Modules
    for name, rel_path in [('libSceFios2.prx', 'sce_module/libSceFios2.prx'),
                           ('libc.prx', 'sce_module/libc.prx')]:
        path = args.game / rel_path
        if not path.exists():
            continue
        mod = module(path)
        plan['modules'][name] = mod['sha256']
        elf, _, ph, _, _ = parse_self(path.read_bytes())

        # Determine strlen range for libc.prx to avoid duplicate patching
        strlen_range = None
        if name == 'libc.prx':
            target_nid = nid('strlen')
            matches = [s for s in mod['symbols'] if s['section'] and s['type'] == 2 and s['name'].split('#')[0] == target_nid]
            if matches:
                strlen_range = (matches[0]['value'], matches[0]['value'] + matches[0]['size'])

        for seg in ph:
            if seg['type'] == 1 and (seg['flags'] & 1):
                code = elf[seg['offset']:seg['offset'] + seg['filesz']]
                for insn in md.disasm(code, seg['vaddr']):
                    if strlen_range and (strlen_range[0] <= insn.address < strlen_range[1]):
                        continue
                    if insn.mnemonic in SUPPORTED:
                        addr = insn.address
                        length = insn.size
                        mnem = insn.mnemonic
                        op = insn.op_str
                        at = seg['offset'] + addr - seg['vaddr']
                        orig_bytes = bytes(elf[at:at+length])
                        try:
                            rep = body(mnem, op)
                            plan['sites'].append({
                                'module': name,
                                'address': addr,
                                'bytes': orig_bytes.hex(),
                                'body': rep.hex(),
                                'instruction': f'{mnem} {op}',
                                'rip_fixups': []
                            })
                            counts[mnem] += 1
                        except Exception as e:
                            plan['unhandled'].append({
                                'module': name,
                                'address': hex(addr),
                                'instruction': f'{mnem} {op}',
                                'reason': str(e)
                            })

    # 2. Main Executable (eboot.bin)
    eboot_path = args.game / 'eboot.bin'
    if eboot_path.exists():
        eboot_mod = module(eboot_path)
        plan['modules']['eboot.bin'] = eboot_mod['sha256']
        fns = function_ranges(eboot_mod)
        seg = next(p for p in eboot_mod['ph'] if p['type'] == 1 and (p['flags'] & 1))

        # Pass 1: Collect branch targets
        targets = set()
        for start, size in fns:
            if seg['vaddr'] <= start < seg['vaddr'] + seg['filesz']:
                off = seg['offset'] + start - seg['vaddr']
                for addr, insn_size, mnem, op in md.disasm_lite(bytes(eboot_mod['elf'][off:off+size]), start):
                    if (mnem.startswith('j') or mnem == 'call') and op.startswith('0x'):
                        try:
                            targets.add(int(op, 16))
                        except ValueError:
                            pass

        # Pass 2: Translate instructions
        seen_addrs = set()
        for start, size in fns:
            if seg['vaddr'] <= start < seg['vaddr'] + seg['filesz']:
                off = seg['offset'] + start - seg['vaddr']
                fn_insns = list(md.disasm_lite(bytes(eboot_mod['elf'][off:off+size]), start))
                for idx, (addr, insn_size, mnem, op) in enumerate(fn_insns):
                    if addr in seen_addrs or mnem not in SUPPORTED:
                        continue
                    at = seg['offset'] + addr - seg['vaddr']
                    orig_bytes = bytes(eboot_mod['elf'][at:at+insn_size])

                    # Standard >= 5 bytes
                    if insn_size >= 5:
                        try:
                            rep = body(mnem, op)
                            plan['sites'].append({
                                'module': 'eboot.bin',
                                'address': addr,
                                'bytes': orig_bytes.hex(),
                                'body': rep.hex(),
                                'instruction': f'{mnem} {op}',
                                'rip_fixups': []
                            })
                            counts[mnem] += 1
                            seen_addrs.add(addr)
                        except Exception as e:
                            plan['unhandled'].append({
                                'module': 'eboot.bin',
                                'address': hex(addr),
                                'instruction': f'{mnem} {op}',
                                'reason': str(e)
                            })
                        continue

                    # Special in-place patch for 0x17f4fdd: movbe eax, [rax] -> mov eax, [rax]; bswap eax
                    if addr == 0x17f4fdd:
                        plan['sites'].append({
                            'module': 'eboot.bin',
                            'address': addr,
                            'bytes': orig_bytes.hex(),
                            'patch': '8b000fc8',
                            'body': '',
                            'instruction': f'{mnem} {op}',
                            'rip_fixups': []
                        })
                        counts[mnem] += 1
                        seen_addrs.add(addr)
                        continue

                    # Spanning for short instructions (< 5 bytes)
                    next_insn = fn_insns[idx+1] if idx+1 < len(fn_insns) else None
                    prev_insn = fn_insns[idx-1] if idx > 0 else None
                    spanned = False

                    # Forward spanning
                    if next_insn:
                        n_addr, n_size, n_mnem, n_op = next_insn
                        if (insn_size + n_size >= 5 and n_addr not in targets and
                                not (n_mnem.startswith('j') or n_mnem == 'call') and
                                'rip' not in n_op and n_addr not in seen_addrs):
                            try:
                                rep = body(mnem, op)
                                next_bytes = bytes(eboot_mod['elf'][at + insn_size : at + insn_size + n_size])
                                spanned_bytes = bytes(eboot_mod['elf'][at : at + insn_size + n_size])
                                plan['sites'].append({
                                    'module': 'eboot.bin',
                                    'address': addr,
                                    'bytes': spanned_bytes.hex(),
                                    'body': (rep + next_bytes).hex(),
                                    'instruction': f'{mnem} {op} [+ {n_mnem} {n_op}]',
                                    'rip_fixups': []
                                })
                                counts[mnem] += 1
                                seen_addrs.add(addr)
                                seen_addrs.add(n_addr)
                                spanned = True
                            except Exception as e:
                                pass

                    # Backward spanning
                    if not spanned and prev_insn:
                        p_addr, p_size, p_mnem, p_op = prev_insn
                        if (insn_size + p_size >= 5 and p_addr not in targets and addr not in targets and
                                not (p_mnem.startswith('j') or p_mnem == 'call') and
                                'rip' not in p_op and p_addr not in seen_addrs):
                            try:
                                rep = body(mnem, op)
                                p_at = seg['offset'] + p_addr - seg['vaddr']
                                prev_bytes = bytes(eboot_mod['elf'][p_at : p_at + p_size])
                                spanned_bytes = bytes(eboot_mod['elf'][p_at : p_at + p_size + insn_size])
                                plan['sites'].append({
                                    'module': 'eboot.bin',
                                    'address': p_addr,
                                    'bytes': spanned_bytes.hex(),
                                    'body': (prev_bytes + rep).hex(),
                                    'instruction': f'{mnem} {op} [- {p_mnem} {p_op}]',
                                    'rip_fixups': []
                                })
                                counts[mnem] += 1
                                seen_addrs.add(p_addr)
                                seen_addrs.add(addr)
                                spanned = True
                            except Exception as e:
                                pass

                    if not spanned:
                        plan['unhandled'].append({
                            'module': 'eboot.bin',
                            'address': hex(addr),
                            'instruction': f'{mnem} {op}',
                            'reason': f'Short instruction ({insn_size}b) cannot be spanned'
                        })

    args.out.parent.mkdir(parents=True, exist_ok=True)
    serialized = json.dumps(plan, separators=(',', ':')).encode('utf-8')
    with atomic_output(args.out) as s:
        s.write(serialized)
    try:
        with atomic_output(args.out.with_name('legacy-cpu-plan.json.xz')) as s:
            s.write(lzma.compress(serialized))
    except Exception:
        pass
    
    # Also write legacy_plan_data.py
    xz_data = lzma.compress(serialized)
    b64_data = base64.b64encode(xz_data).decode('ascii')
    chunks = [f'    "{b64_data[i:i+100]}"' for i in range(0, len(b64_data), 100)]
    py_content = (
        '"""Pre-compiled Ivy Bridge compatibility translation plan for UFC 3 PS4 Port (CUSA06534).\n'
        f'Generated for {len(plan["sites"])} translated sites, {len(plan["unhandled"])} unhandled.\n'
        'Compressed using standard library LZMA + Base64 (zero dependencies required).\n'
        '"""\n'
        'import base64\n'
        'import json\n'
        'import lzma\n\n'
        '_PLAN_DATA = (\n' + '\n'.join(chunks) + '\n)\n\n'
        'def get_plan():\n'
        '    """Returns the decompressed compatibility plan dictionary."""\n'
        '    return json.loads(lzma.decompress(base64.b64decode(_PLAN_DATA.encode("ascii"))))\n'
    )
    with atomic_output(args.out.with_name('legacy_plan_data.py')) as s:
        s.write(py_content.encode('utf-8'))
        
    print(f"Legacy CPU Plan created: {len(plan['sites'])} sites translated, {len(plan['unhandled'])} unhandled.")
    print("Breakdown by instruction:", dict(counts))

if __name__ == '__main__':
    main()
