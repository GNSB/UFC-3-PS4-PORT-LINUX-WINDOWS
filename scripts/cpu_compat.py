"""Guarded local Ivy Bridge translations for UFC 3 PS4 Port (CUSA06534) guest images.

Translates BMI1, LZCNT, and MOVBE instructions using trampolines at link-time.
Original game files are never modified. Automatically activates on CPUs lacking
BMI1, LZCNT, or MOVBE (Intel Ivy Bridge / Sandy Bridge).
Override via bbport.ini (legacy_cpu=1/0) or env BB_LEGACY_CPU=1/0.
"""
import collections
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import tempfile
from prepare import nid

# SysV ABI baseline strlen without BMI1:
# xor eax, eax; loop: cmp byte [rdi+rax], 0; je done; inc rax; jmp loop; done: ret.
STRLEN_BASELINE = bytes.fromhex('31c0803c0700740548ffc0ebf5c3')
MAX_IMAGE_SIZE = 512 * 1024 * 1024
SUPPORTED = frozenset(('andn', 'bextr', 'blsr', 'blsi', 'blsmsk', 'tzcnt', 'lzcnt', 'movbe'))

# CPUID host extension requirements: (leaf, register index, bit)
HOST_EXTENSIONS = {
    'BMI1': (7, 1, 3),            # EBX bit 3
    'LZCNT': (0x80000001, 2, 5),   # ECX bit 5
    'MOVBE': (1, 2, 22)           # ECX bit 22
}

CPUID_CODE = bytes.fromhex(
    '5389c889d10fa241890041895804418948084189500c5bc3'
)

def _cpuid_windows():
    """Returns a callable cpuid(leaf, subleaf) -> (eax, ebx, ecx, edx) on Windows."""
    import ctypes
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.VirtualAlloc.restype = ctypes.c_void_p
    k32.VirtualAlloc.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32)
    k32.VirtualFree.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32)
    mem = k32.VirtualAlloc(None, len(CPUID_CODE), 0x3000, 0x40)  # MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE
    if not mem:
        return None
    ctypes.memmove(mem, CPUID_CODE, len(CPUID_CODE))
    fn = ctypes.CFUNCTYPE(None, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p)(mem)

    def cpuid(leaf, subleaf=0):
        regs = (ctypes.c_uint32 * 4)()
        fn(leaf, subleaf, ctypes.addressof(regs))
        return tuple(regs)

    return cpuid, lambda: k32.VirtualFree(mem, 0, 0x8000)

def missing_host_extensions():
    """Returns a list of missing extensions on the current CPU, or None if undetermined."""
    if os.name != 'nt' or struct.calcsize('P') != 8:
        try:
            flags = Path('/proc/cpuinfo').read_text().split('flags')[1].split('\n')[0]
            missing = []
            if 'bmi1' not in flags: missing.append('BMI1')
            if 'abm' not in flags and 'lzcnt' not in flags: missing.append('LZCNT')
            if 'movbe' not in flags: missing.append('MOVBE')
            return missing
        except Exception:
            return None

    try:
        found = _cpuid_windows()
        if not found:
            return None
        cpuid, cleanup = found
        try:
            top = cpuid(0)[0]
            extended = cpuid(0x80000000)[0]
            missing = []
            for name, (leaf, reg, bit) in HOST_EXTENSIONS.items():
                avail = top if leaf < 0x80000000 else extended
                if leaf > avail or not (cpuid(leaf)[reg] >> bit & 1):
                    missing.append(name)
            return missing
        finally:
            cleanup()
    except Exception:
        return None

def enabled():
    override = os.environ.get('BB_LEGACY_CPU')
    if override is not None:
        return override == '1'
    config = Path(os.environ.get('BB_CONFIG', Path(__file__).resolve().parent.parent / 'bbport.ini'))
    try:
        if config.exists():
            for line in config.read_text(encoding='utf-8').splitlines():
                line = line.strip()
                if line == 'legacy_cpu=1': return True
                if line == 'legacy_cpu=0': return False
    except OSError:
        pass
    missing = missing_host_extensions()
    return missing is None or bool(missing)

@contextmanager
def atomic_output(path):
    path = Path(path)
    tmp = None
    try:
        with tempfile.NamedTemporaryFile(mode='wb', prefix=path.name + '.', suffix='.tmp',
                                         dir=path.parent, delete=False) as s:
            tmp = Path(s.name)
            yield s
            s.flush()
            os.fsync(s.fileno())
        os.replace(tmp, path)
    finally:
        if tmp is not None:
            tmp.unlink(missing_ok=True)

def patch_libc(module):
    """Replaces BMI1 strlen in libc.prx with baseline x86-64 strlen."""
    if not enabled():
        return []
    target_nid = nid('strlen')
    matches = [s for s in module['symbols'] if s['section'] and s['type'] == 2
               and s['name'].split('#')[0] == target_nid]
    if not matches:
        return []
    symbol = matches[0]
    segment = next((p for p in module['ph'] if p['type'] == 1 and (p['flags'] & 1)
                   and p['vaddr'] <= symbol['value']
                   and symbol['value'] + symbol['size'] <= p['vaddr'] + p['filesz']), None)
    if not segment:
        return []
    offset = segment['offset'] + symbol['value'] - segment['vaddr']
    original = bytes(module['elf'][offset:offset + symbol['size']])
    
    # Overwrite with STRLEN_BASELINE + NOP padding
    module['elf'][offset:offset + symbol['size']] = STRLEN_BASELINE + b'\x90' * (symbol['size'] - len(STRLEN_BASELINE))
    print('CPU compatibility: replaced BMI1 strlen in generated libc image (Ivy Bridge support)')
    return [{'function': 'strlen', 'address': hex(symbol['value']),
             'original_sha256': hashlib.sha256(original).hexdigest(),
             'replacement': 'baseline x86-64'}]

def _relative(target, next_address, label):
    disp = target - next_address
    if not -(1 << 31) <= disp < (1 << 31):
        raise ValueError(f'Legacy CPU {label}: relative displacement out of range.')
    return struct.pack('<i', disp)

def patch_image(image, segments, main, modules, relocations, *, plan=None, active=None):
    """Applies legacy CPU plan trampolines to the linked binary image."""
    is_active = enabled() if active is None else active
    report = dict(enabled=is_active, patched_sites=0, unhandled_count=0, trampoline_bytes=0)
    if not is_active:
        return bytearray(image), list(segments), report

    plan_path = Path(__file__).resolve().parent.parent / 'tools/legacy-cpu-plan.json'
    plan_xz = Path(__file__).resolve().parent.parent / 'tools/legacy-cpu-plan.json.xz'
    if plan is None:
        try:
            if plan_path.exists():
                plan = json.loads(plan_path.read_bytes())
            elif plan_xz.exists():
                import lzma
                plan = json.loads(lzma.decompress(plan_xz.read_bytes()))
            else:
                try:
                    import sys
                    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
                    import legacy_plan_data
                    plan = legacy_plan_data.get_plan()
                except ImportError:
                    pass
        except Exception as e:
            print(f"Warning: Legacy CPU plan found but could not be read: {e}")
            plan = None

    if not plan or not plan.get('sites'):
        print("CPU compatibility: No legacy-cpu-plan.json found; skipping trampoline injection.")
        return bytearray(image), list(segments), report

    locations = {m['file']: (m['base'], m['size']) for m in modules}
    locations['eboot.bin'] = (0, min((m['base'] for m in modules), default=len(image)))

    arena = (len(image) + 65535) & ~65535
    cursor = arena
    operations = []
    counts = collections.Counter()

    for site in plan['sites']:
        name = site['module']
        if name not in locations:
            continue
        address = site['address']
        original = bytes.fromhex(site['bytes'])
        body = bytearray(bytes.fromhex(site['body']))
        base, _ = locations[name]
        start = base + address
        end = start + len(original)

        if image[start:end] != original:
            continue

        cursor = (cursor + 15) & ~15
        for fixup in site.get('rip_fixups', []):
            off = fixup['offset']
            following = fixup['next_offset']
            target = fixup['target']
            body[off:off + 4] = _relative(base + target, cursor + following, f"{name}+{address:#x}")

        if 'stub_address' in site:
            stub_addr = base + site['stub_address']
            stub_go = b'\xe9' + _relative(cursor, stub_addr + 5, f"{name}+{address:#x}")
            ret_addr = base + site['return_target']
            back = b'\xe9' + _relative(ret_addr, cursor + len(body) + 5, f"{name}+{address:#x}")
            patch_bytes = bytes.fromhex(site['patch'])
            operations.append((start, end, None, b'', patch_bytes))
            operations.append((stub_addr, stub_addr + len(stub_go), cursor, bytes(body) + back, stub_go))
        elif 'patch' in site:
            patch_bytes = bytes.fromhex(site['patch'])
            operations.append((start, end, None, b'', patch_bytes))
            counts[site['instruction'].split()[0]] += 1
            continue
        else:
            go = b'\xe9' + _relative(cursor, start + 5, f"{name}+{address:#x}")
            ret_addr = base + site['return_target'] if 'return_target' in site else end
            back = b'\xe9' + _relative(ret_addr, cursor + len(body) + 5, f"{name}+{address:#x}")
            operations.append((start, end, cursor, bytes(body) + back, go + b'\x90' * (len(original) - 5)))
        cursor += len(body) + 5
        counts[site['instruction'].split()[0]] += 1

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

    patched_count = len([op for op in operations if op[2] is not None or op[4]])
    report.update(patched_sites=patched_count, counts_by_instruction=dict(counts),
                  trampoline_bytes=cursor - arena, unhandled_count=len(plan.get('unhandled', [])))
    return result, new_segments, report
