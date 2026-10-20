# SPDX-License-Identifier: GPL-2.0-only
#
# EA SPORTS UFC 3 PS4 Port - Legacy CPU Compatibility Runtime Trampoline Injector
#
# Based on the Bloodborne PC Ivy Bridge patch by T0ug:
#   https://github.com/T0ug/bloodborne_pc_Ivy-Bridge
# and bloodborne_pc by Supermedo (deadinside28):
#   https://github.com/deadinside28/bloodborne_pc
#
# Adapted and developed for EA SPORTS UFC 3 (CUSA06534) by @deanzx06
# with AI-assisted engineering.

"""Legacy CPU compatibility (Intel Ivy Bridge / Sandy Bridge) runtime trampoline injector for UFC 3."""
import collections
import contextlib
import ctypes
import json
import lzma
import os
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parent.parent

PAGE_SIZE = 16384
MAX_TRAMPOLINE_SIZE = 16 * 1024 * 1024

SUPPORTED = {'andn', 'bextr', 'blsi', 'blsr', 'blsmsk', 'tzcnt', 'lzcnt', 'movbe'}

# SysV x86-64 baseline strlen implementation (zero AVX2/BMI1 dependencies)
# 14 bytes: xor eax, eax; cmp byte ptr [rdi + rax], 0; je done; inc rax; jmp loop; done: ret
STRLEN_SYSV_X64 = bytes.fromhex('31c0803c0700740548ffc0ebf5c3')

@contextlib.contextmanager
def atomic_output(path):
    path = Path(path)
    tmp = path.with_name(f"{path.name}.tmp.{os.getpid()}")
    try:
        with tmp.open('wb') as f:
            yield f
        tmp.replace(path)
    finally:
        if tmp.exists():
            try:
                tmp.unlink()
            except OSError:
                pass

def host_lacks_features():
    """Detect if host CPU lacks BMI1, LZCNT, or MOVBE."""
    # Check manual override first
    env = os.environ.get('BB_LEGACY_CPU')
    if env is not None:
        return env == '1'

    if sys.platform.startswith('linux'):
        try:
            with open('/proc/cpuinfo', 'r') as f:
                content = f.read()
            flags = set()
            for line in content.splitlines():
                if line.startswith('flags') and ':' in line:
                    flags.update(line.split(':', 1)[1].split())
            if flags:
                return not ({'bmi1', 'abm', 'movbe'}.issubset(flags))
        except Exception:
            pass

    if sys.platform == 'win32':
        try:
            code = bytes.fromhex(
                '53'                  # push rbx
                '4889d3'              # mov rbx, rdx
                '89c8'                # mov eax, ecx
                '31c9'                # xor ecx, ecx
                '0fa2'                # cpuid
                '8903'                # mov [rbx], eax
                '895b04'              # mov [rbx+4], ebx
                '894b08'              # mov [rbx+8], ecx
                '89530c'              # mov [rbx+12], edx
                '5b'                  # pop rbx
                'c3'                  # ret
            )
            valloc = ctypes.windll.kernel32.VirtualAlloc
            valloc.restype = ctypes.c_void_p
            vfree = ctypes.windll.kernel32.VirtualFree
            MEM_COMMIT, MEM_RESERVE, PAGE_EXECUTE_READWRITE = 0x1000, 0x2000, 0x40
            ptr = valloc(None, len(code), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)
            if ptr:
                ctypes.memmove(ptr, code, len(code))
                func = ctypes.CFUNCTYPE(None, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32 * 4))(ptr)
                regs1 = (ctypes.c_uint32 * 4)()
                func(1, ctypes.byref(regs1))
                leaf1_ecx = regs1[2]

                regs7 = (ctypes.c_uint32 * 4)()
                func(7, ctypes.byref(regs7))
                leaf7_ebx = regs7[1]

                regs_ext = (ctypes.c_uint32 * 4)()
                func(0x80000001, ctypes.byref(regs_ext))
                ext1_ecx = regs_ext[2]

                vfree(ptr, 0, 0x8000)

                has_bmi1 = bool(leaf7_ebx & (1 << 3))
                has_movbe = bool(leaf1_ecx & (1 << 22))
                has_lzcnt = bool(ext1_ecx & (1 << 5))

                if not (has_bmi1 and has_movbe and has_lzcnt):
                    return True
                return False
        except Exception:
            pass

    return False

def enabled():
    # 1. Environment variable override
    env = os.environ.get('BB_LEGACY_CPU')
    if env is not None:
        return env == '1'

    # 2. Config file check (bbport.ini in current directory or user data)
    for ini_path in [Path('bbport.ini'), ROOT / 'bbport.ini']:
        if ini_path.exists():
            try:
                for line in ini_path.read_text(encoding='utf-8', errors='ignore').splitlines():
                    if re.search(r'^\s*legacy_cpu\s*=\s*1\b', line):
                        return True
            except Exception:
                pass

    # 3. CPU feature check
    return host_lacks_features()

def load_plan():
    """Load the translation plan dictionary from Python module or JSON files."""
    try:
        import sys
        sys.path.insert(0, str(ROOT / 'tools'))
        import legacy_plan_data
        return legacy_plan_data.get_plan()
    except ImportError:
        pass

    for candidate in [ROOT / 'tools/legacy-cpu-plan.json.xz', ROOT / 'tools/legacy-cpu-plan.json']:
        if candidate.exists():
            try:
                if candidate.suffix == '.xz':
                    with lzma.open(candidate, 'rt', encoding='utf-8') as f:
                        return json.load(f)
                else:
                    with candidate.open('r', encoding='utf-8') as f:
                        return json.load(f)
            except Exception as e:
                print(f"WARNING: Failed to load {candidate}: {e}")
    return None

def patch_libc(libc_module, libc_elf):
    """Replace libc strlen with baseline SysV x86-64 implementation."""
    from prepare import nid
    symbols = libc_module.get('symbols', [])
    target_nid = nid('strlen')
    matches = [s for s in symbols if s['section'] and s['type'] == 2 and s['name'].split('#')[0] == target_nid]
    if not matches:
        return False

    sym = matches[0]
    seg = next((p for p in libc_module['ph'] if p['type'] == 1 and p['vaddr'] <= sym['value'] < p['vaddr'] + p['filesz']), None)
    if not seg:
        print("WARNING: Could not locate segment for strlen in libc.prx")
        return False

    offset = seg['offset'] + sym['value'] - seg['vaddr']

    if sym['size'] < len(STRLEN_SYSV_X64):
        print(f"WARNING: strlen size in libc.prx too small ({sym['size']} bytes)")
        return False

    padded = STRLEN_SYSV_X64 + b'\xcc' * (sym['size'] - len(STRLEN_SYSV_X64))
    libc_elf[offset:offset + sym['size']] = padded
    return True

def _relative(target, following, context):
    delta = target - following
    if not -0x80000000 <= delta <= 0x7fffffff:
        raise ValueError(f"branch out of range for {context}: {target:#x} - {following:#x} = {delta:#x}")
    return struct.pack('<i', delta)

def patch_image(image, segments, main, modules, relocations, *, plan=None, active=None):
    """Applies legacy CPU plan trampolines to the linked binary image."""
    is_active = enabled() if active is None else active
    report = dict(enabled=is_active, patched_sites=0, skipped_sites=0, unhandled_count=0,
                  trampoline_bytes=0, complete=False)
    if not is_active:
        return bytearray(image), list(segments), report

    if plan is None:
        plan = load_plan()

    if not plan or not plan.get('sites'):
        print("WARNING: No legacy CPU plan loaded; skipping instruction translation.")
        return bytearray(image), list(segments), report

    # 1. Validate module SHA-256 hashes against the plan
    locations = {m['file']: (m['base'], m['size'], m.get('sha256')) for m in modules}
    locations['eboot.bin'] = (0, min((m['base'] for m in modules), default=len(image)), main.get('sha256'))

    plan_modules = plan.get('modules', {})
    for mod_name, expected_hash in plan_modules.items():
        if mod_name in locations:
            actual_hash = locations[mod_name][2]
            if actual_hash and actual_hash != expected_hash:
                print(f"ERROR: Module '{mod_name}' SHA-256 mismatch!")
                print(f"   Expected: {expected_hash}")
                print(f"   Actual:   {actual_hash}")
                print("Refusing to apply partial or incompatible translation plan.")
                report['error'] = f"SHA-256 mismatch for {mod_name}"
                report['skipped_sites'] = len(plan.get('sites', []))
                return bytearray(image), list(segments), report

    arena = (len(image) + 65535) & ~65535
    cursor = arena
    operations = []
    counts = collections.Counter()
    skipped_count = 0

    for site in plan.get('sites', []):
        name = site['module']
        if name not in locations:
            skipped_count += 1
            continue

        base, _, _ = locations[name]
        vaddr = site['address']
        length = site['length']
        start = base + vaddr
        end = start + length

        if start < 0 or end > len(image):
            skipped_count += 1
            continue

        # In-place patch
        if site.get('patch'):
            patch_bytes = bytes.fromhex(site['patch'])
            if len(patch_bytes) != length:
                print(f"ERROR: In-place patch length mismatch at {name}+{hex(vaddr)}")
                skipped_count += 1
                continue
            operations.append((start, end, None, b'', patch_bytes))
            counts[site['instruction'].split()[0]] += 1
            continue

        # Trampoline jump replacement
        if length < 5:
            print(f"ERROR: Cannot replace site < 5 bytes with jump at {name}+{hex(vaddr)}")
            skipped_count += 1
            continue

        body_bytes = bytes.fromhex(site['body'])
        if cursor - arena + len(body_bytes) + 5 > MAX_TRAMPOLINE_SIZE:
            print("ERROR: Trampoline area exceeded maximum size")
            skipped_count += 1
            continue

        cursor = (cursor + 15) & ~15
        try:
            go = b'\xe9' + _relative(cursor, start + 5, f"{name}+{hex(vaddr)}")
            back = b'\xe9' + _relative(end, cursor + len(body_bytes) + 5, f"{name}+{hex(vaddr)}")
            replacement = go + b'\x90' * (length - 5)
            assert len(replacement) == length
            operations.append((start, end, cursor, bytes(body_bytes) + back, replacement))
            cursor += len(body_bytes) + 5
            counts[site['instruction'].split()[0]] += 1
        except Exception as e:
            print(f"ERROR: Failed to assemble trampoline for {name}+{hex(vaddr)}: {e}")
            skipped_count += 1

    result = bytearray(image)
    new_segments = list(segments)
    if operations:
        padded_end = (cursor + 4095) & ~4095
        result.extend(bytes(padded_end - len(result)))
        for start, end, target, t_body, jump in operations:
            result[start:end] = jump
            if target is not None and t_body:
                result[target:target + len(t_body)] = t_body
        new_segments.append((arena, padded_end - arena, 5))  # PROT_READ | PROT_EXEC

    patched_count = len(operations)
    is_complete = (skipped_count == 0 and patched_count > 0)
    report.update(patched_sites=patched_count, skipped_sites=skipped_count,
                  counts_by_instruction=dict(counts), trampoline_bytes=cursor - arena,
                  unhandled_count=len(plan.get('unhandled', [])), complete=is_complete)

    if skipped_count > 0:
        print(f"WARNING: {skipped_count} legacy CPU translation sites were skipped or failed.")

    return result, new_segments, report
