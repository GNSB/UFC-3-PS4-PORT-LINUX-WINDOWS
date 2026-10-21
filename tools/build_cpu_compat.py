# SPDX-License-Identifier: GPL-2.0-only
#
# EA SPORTS UFC 3 PS4 Port - Legacy CPU Compatibility Plan Generator
#
# Based on the Bloodborne PC Ivy Bridge patch by T0ug:
#   https://github.com/T0ug/bloodborne_pc_Ivy-Bridge
# and bloodborne_pc by Supermedo (deadinside28):
#   https://github.com/deadinside28/bloodborne_pc
#
# Adapted and developed for EA SPORTS UFC 3 (CUSA06534) by @deanzx06
# with AI-assisted engineering.

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

ASSEMBLER = keystone.Ks(keystone.KS_ARCH_X86, keystone.KS_MODE_64)

LEGACY_32 = {'eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'}

def function_ranges(mod):
    functions = set()
    header = next((p for p in mod['ph'] if p['type'] == 0x6474e550), None)
    if header:
        offset, addr = header['offset'], header['vaddr']
        count = struct.unpack_from('<I', mod['elf'], offset + 8)[0]
        for idx in range(count):
            start, fde = struct.unpack_from('<ii', mod['elf'], offset + 12 + idx * 8)
            fde_offset = next((p['offset'] + (addr + fde) - p['vaddr']
                              for p in mod['ph']
                              if p['type'] == 1 and p['vaddr'] <= addr + fde < p['vaddr'] + p['filesz']), None)
            if fde_offset:
                size = struct.unpack_from('<I', mod['elf'], fde_offset + 12)[0]
                if 0 < size < 4 * 1024**2:
                    functions.add((addr + start, size))
    for s in mod['symbols']:
        if s['type'] == 2 and s['section'] and s['size']:
            functions.add((s['value'], s['size']))
    return sorted(functions)

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
    addr_regs = set(canonical(r) for r in re.findall(
        r'\b(?:r(?:1[0-5]|[8-9])[bwd]?|r(?:ax|bx|cx|dx|si|di|bp|sp)|e(?:ax|bx|cx|dx|si|di|bp|sp)|[a-d][xhl]|[sbd]pl|[sbd]il)\b',
        memory[2]))
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
    if 'rip' in operands:
        raise ValueError(f"RIP-relative operands not supported in trampoline: {mnemonic} {operands}")
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
            lines += [f'lea {mask}, [{temp[0]}-1]', f'and {first}, {mask}', 'jmp done',
                      'zero:', f'xor {first}, {first}', 'stc']
        elif mnemonic == 'blsi':
            lines += [f'mov {mask}, {first}', f'neg {first}', f'and {first}, {mask}', 'stc', 'jmp done',
                      'zero:', f'xor {first}, {first}']
        elif mnemonic == 'blsmsk':
            lines += [f'lea {mask}, [{temp[0]}-1]', f'xor {first}, {mask}', 'jmp done',
                      'zero:', f'mov {first}, -1', f'test {first}, {first}', 'stc']
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

def try_inplace_movbe(mnem, op, insn_size):
    """Generalized in-place replacement for 32-bit movbe loads."""
    if mnem != 'movbe':
        return None
    args = [x.strip() for x in op.split(',')]
    if len(args) == 2 and args[0] in LEGACY_32 and '[' in args[1] and 'dword ptr' in args[1] and 'rip' not in args[1]:
        asm_code = f'mov {args[0]}, {args[1]}; bswap {args[0]}'
        try:
            encoding, _ = ASSEMBLER.asm(asm_code)
            if len(encoding) == insn_size:
                return bytes(encoding)
        except Exception:
            pass
    return None

def collect_branch_targets(mod, seg, fns, md):
    """Collect branch targets from direct jumps, calls, RIP-relative references, function entries, and relocations."""
    targets = set()
    # 1. Direct branches and RIP-relative memory references
    for start, size in fns:
        if seg['vaddr'] <= start < seg['vaddr'] + seg['filesz']:
            # Protect function/FDE entry addresses
            targets.add(start)
            off = seg['offset'] + start - seg['vaddr']
            for addr, insn_size, mnem, op in md.disasm_lite(bytes(mod['elf'][off:off+size]), start):
                if (mnem.startswith('j') or mnem == 'call') and op.startswith('0x'):
                    try:
                        targets.add(int(op, 16))
                    except ValueError:
                        pass
                if 'rip +' in op or 'rip -' in op:
                    m = re.search(r'\[rip\s*([+-])\s*(0x[0-9a-fA-F]+|\d+)\]', op)
                    if m:
                        disp = int(m.group(2), 0)
                        if m.group(1) == '-': disp = -disp
                        target_addr = addr + insn_size + disp
                        if seg['vaddr'] <= target_addr < seg['vaddr'] + seg['filesz']:
                            targets.add(target_addr)

    # 2. Protect symbol entry points in the code segment
    for s in mod.get('symbols', []):
        val = s.get('value', 0)
        if seg['vaddr'] <= val < seg['vaddr'] + seg['filesz']:
            targets.add(val)

    # 3. Relocations: relocated destination pointer values and in-code write targets
    for r in mod.get('relocs', []):
        if isinstance(r, (tuple, list)) and len(r) >= 4:
            rtarget, kind, symid, addend = r[:4]
            # Write address (e.g., text relocation)
            if seg['vaddr'] <= rtarget < seg['vaddr'] + seg['filesz']:
                targets.add(rtarget)
            # Relocated pointer value
            if kind == 8:  # R_X86_64_RELATIVE
                if seg['vaddr'] <= addend < seg['vaddr'] + seg['filesz']:
                    targets.add(addend)
            elif kind in (1, 6, 7):  # R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT
                if 0 <= symid < len(mod['symbols']):
                    sym = mod['symbols'][symid]
                    if sym.get('section'):
                        dest = sym['value'] + addend
                        if seg['vaddr'] <= dest < seg['vaddr'] + seg['filesz']:
                            targets.add(dest)
        elif isinstance(r, dict):
            target = r.get('target', 0)
            if seg['vaddr'] <= target < seg['vaddr'] + seg['filesz']:
                targets.add(target)
    return targets

def scan_module_functions(mod_name, mod, seg, fns, md, plan, counts, strlen_range=None):
    targets = collect_branch_targets(mod, seg, fns, md)
    seen_addrs = set()

    for start, size in fns:
        if not (seg['vaddr'] <= start < seg['vaddr'] + seg['filesz']):
            continue
        off = seg['offset'] + start - seg['vaddr']
        fn_insns = list(md.disasm_lite(bytes(mod['elf'][off:off+size]), start))

        for idx, (addr, insn_size, mnem, op) in enumerate(fn_insns):
            if addr in seen_addrs:
                continue
            if strlen_range and (strlen_range[0] <= addr < strlen_range[1]):
                continue
            if mnem not in SUPPORTED:
                continue

            at = seg['offset'] + addr - seg['vaddr']

            # Guard against RIP-relative operands in the instruction
            if 'rip' in op:
                plan['unhandled'].append({
                    'module': mod_name,
                    'address': hex(addr),
                    'instruction': f'{mnem} {op}',
                    'reason': 'RIP-relative operand in translated instruction'
                })
                continue

            # 1. Try generalized in-place movbe patch
            inplace = try_inplace_movbe(mnem, op, insn_size)
            if inplace:
                plan['sites'].append({
                    'module': mod_name,
                    'address': addr,
                    'length': insn_size,
                    'patch': inplace.hex(),
                    'body': '',
                    'instruction': f'{mnem} {op}'
                })
                counts[mnem] += 1
                seen_addrs.add(addr)
                continue

            # 2. Standard >= 5 bytes trampoline
            if insn_size >= 5:
                try:
                    rep = body(mnem, op)
                    plan['sites'].append({
                        'module': mod_name,
                        'address': addr,
                        'length': insn_size,
                        'body': rep.hex(),
                        'instruction': f'{mnem} {op}'
                    })
                    counts[mnem] += 1
                    seen_addrs.add(addr)
                except Exception as e:
                    plan['unhandled'].append({
                        'module': mod_name,
                        'address': hex(addr),
                        'instruction': f'{mnem} {op}',
                        'reason': str(e)
                    })
                continue

            # 3. Short instruction (< 5 bytes): Spanning
            next_insn = fn_insns[idx+1] if idx+1 < len(fn_insns) else None
            prev_insn = fn_insns[idx-1] if idx > 0 else None
            spanned = False

            # Forward spanning
            if next_insn:
                n_addr, n_size, n_mnem, n_op = next_insn
                # Guards: neighbor cannot be branch/call, rip-relative, loop*, xbegin, or branch target
                if (insn_size + n_size >= 5 and n_addr not in targets and
                        not (n_mnem.startswith('j') or n_mnem == 'call') and
                        not (n_mnem.startswith('loop') or n_mnem == 'xbegin') and
                        'rip' not in n_op and n_addr not in seen_addrs):
                    try:
                        rep1 = body(mnem, op)
                        if n_mnem in SUPPORTED:
                            # Both instructions need translation!
                            rep2 = body(n_mnem, n_op)
                            combined_body = rep1 + rep2
                            combined_instr = f'{mnem} {op} [+ {n_mnem} {n_op}]'
                            counts[n_mnem] += 1
                            plan['sites'].append({
                                'module': mod_name,
                                'address': addr,
                                'length': insn_size + n_size,
                                'body': combined_body.hex(),
                                'instruction': combined_instr
                            })
                        else:
                            # Neighbor is normal instruction; DO NOT embed its original bytes in the plan!
                            # Store spanning metadata so neighbor bytes are loaded dynamically at link time.
                            combined_instr = f'{mnem} {op} [+ {n_mnem} {n_op}]'
                            plan['sites'].append({
                                'module': mod_name,
                                'address': addr,
                                'length': insn_size + n_size,
                                'body': rep1.hex(),
                                'span': {'pos': 'after', 'len': n_size},
                                'instruction': combined_instr
                            })
                        counts[mnem] += 1
                        seen_addrs.add(addr)
                        seen_addrs.add(n_addr)
                        spanned = True
                    except Exception:
                        pass

            # Backward spanning
            if not spanned and prev_insn:
                p_addr, p_size, p_mnem, p_op = prev_insn
                if (insn_size + p_size >= 5 and p_addr not in targets and addr not in targets and
                        not (p_mnem.startswith('j') or p_mnem == 'call') and
                        not (p_mnem.startswith('loop') or p_mnem == 'xbegin') and
                        'rip' not in p_op and p_addr not in seen_addrs and p_mnem not in SUPPORTED):
                    try:
                        rep = body(mnem, op)
                        plan['sites'].append({
                            'module': mod_name,
                            'address': p_addr,
                            'length': p_size + insn_size,
                            'body': rep.hex(),
                            'span': {'pos': 'before', 'len': p_size},
                            'instruction': f'{mnem} {op} [- {p_mnem} {p_op}]'
                        })
                        counts[mnem] += 1
                        seen_addrs.add(p_addr)
                        seen_addrs.add(addr)
                        spanned = True
                    except Exception:
                        pass

            if not spanned:
                plan['unhandled'].append({
                    'module': mod_name,
                    'address': hex(addr),
                    'instruction': f'{mnem} {op}',
                    'reason': f'Short instruction ({insn_size}b) cannot be safely spanned'
                })

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path, help='Folder game UFC 3 (CUSA06534)')
    p.add_argument('--out', type=Path, default=ROOT / 'tools/legacy-cpu-plan.json')
    args = p.parse_args()

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    plan = {'version': 3, 'modules': {}, 'sites': [], 'unhandled': []}
    counts = collections.Counter()

    # 1. PRX Modules (libc.prx and libSceFios2.prx) scanned with function ranges
    for name, rel_path in [('libc.prx', 'sce_module/libc.prx'),
                           ('libSceFios2.prx', 'sce_module/libSceFios2.prx')]:
        path = args.game / rel_path
        if not path.exists():
            continue
        mod = module(path)
        plan['modules'][name] = mod['sha256']
        fns = function_ranges(mod)
        seg = next(p for p in mod['ph'] if p['type'] == 1 and (p['flags'] & 1))

        strlen_range = None
        if name == 'libc.prx':
            target_nid = nid('strlen')
            matches = [s for s in mod['symbols'] if s['section'] and s['type'] == 2 and s['name'].split('#')[0] == target_nid]
            if matches:
                strlen_range = (matches[0]['value'], matches[0]['value'] + matches[0]['size'])

        scan_module_functions(name, mod, seg, fns, md, plan, counts, strlen_range=strlen_range)

    # 2. Main Executable (eboot.bin)
    eboot_path = args.game / 'eboot.bin'
    if eboot_path.exists():
        eboot_mod = module(eboot_path)
        plan['modules']['eboot.bin'] = eboot_mod['sha256']
        fns = function_ranges(eboot_mod)
        seg = next(p for p in eboot_mod['ph'] if p['type'] == 1 and (p['flags'] & 1))
        scan_module_functions('eboot.bin', eboot_mod, seg, fns, md, plan, counts)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    serialized = json.dumps(plan, separators=(',', ':')).encode('utf-8')
    with atomic_output(args.out) as s:
        s.write(serialized)
    try:
        with atomic_output(args.out.with_name('legacy-cpu-plan.json.xz')) as s:
            s.write(lzma.compress(serialized))
    except Exception:
        pass

    # Also write legacy_plan_p1.py, p2.py, p3.py and legacy_plan_data.py (Base64 + LZMA embedded compressed plan)
    xz_data = lzma.compress(serialized, preset=9)
    b64_data = base64.b64encode(xz_data).decode('ascii')
    n = len(b64_data)
    c1 = n // 3
    c2 = 2 * n // 3
    parts = [b64_data[:c1], b64_data[c1:c2], b64_data[c2:]]

    for idx, part in enumerate(parts, 1):
        pchunks = [f'    "{part[i:i+100]}"' for i in range(0, len(part), 100)]
        content = (
            "# SPDX-License-Identifier: GPL-2.0-only\n"
            f'"""Legacy CPU plan chunk {idx} of 3 for UFC 3 PS4 Port."""\n\n'
            f"PART{idx} = (\n" + "\n".join(pchunks) + "\n)\n"
        )
        with atomic_output(args.out.with_name(f"legacy_plan_p{idx}.py")) as s:
            s.write(content.encode('utf-8'))

    driver = (
        '# SPDX-License-Identifier: GPL-2.0-only\n'
        '"""Pre-compiled Ivy Bridge compatibility translation plan for UFC 3 PS4 Port (CUSA06534).\n'
        'Zero external dependencies (uses standard library LZMA + Base64).\n'
        '"""\n'
        'import base64\n'
        'import json\n'
        'import lzma\n'
        'from legacy_plan_p1 import PART1\n'
        'from legacy_plan_p2 import PART2\n'
        'from legacy_plan_p3 import PART3\n\n'
        'def get_plan():\n'
        '    """Returns the decompressed compatibility plan dictionary."""\n'
        '    combined = PART1 + PART2 + PART3\n'
        '    return json.loads(lzma.decompress(base64.b64decode(combined.encode("ascii"))))\n'
    )
    with atomic_output(args.out.with_name('legacy_plan_data.py')) as s:
        s.write(driver.encode('utf-8'))

    print(f"Legacy CPU Plan created: {len(plan['sites'])} sites translated, {len(plan['unhandled'])} unhandled.")
    print("Breakdown by instruction:", dict(counts))

if __name__ == '__main__':
    main()
