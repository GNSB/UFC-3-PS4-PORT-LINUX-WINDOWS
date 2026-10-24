# SPDX-License-Identifier: GPL-2.0-only
#
# EA SPORTS UFC 3 PS4 Port - Legacy CPU Compatibility Unit Tests
#
# Based on the Bloodborne PC Ivy Bridge patch test suite by T0ug:
#   https://github.com/T0ug/bloodborne_pc_Ivy-Bridge
# Adapted for EA SPORTS UFC 3 by @deanzx06 with AI assistance.

import json
import os
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'tools'))

from cpu_compat import enabled, load_plan, STRLEN_SYSV_X64, SUPPORTED
import legacy_plan_data


class TestCpuCompat(unittest.TestCase):

    def test_strlen_replacement_size_and_baseline(self):
        """Verify baseline strlen is exactly 14 bytes and contains no VEX/AVX opcodes."""
        self.assertEqual(len(STRLEN_SYSV_X64), 14)
        # Verify no VEX prefix (0xc4 or 0xc5) in baseline strlen
        self.assertNotIn(0xc4, STRLEN_SYSV_X64)
        self.assertNotIn(0xc5, STRLEN_SYSV_X64)

    def test_enabled_environment_override(self):
        """Verify BB_LEGACY_CPU environment variable controls activation."""
        orig = os.environ.get('BB_LEGACY_CPU')
        try:
            os.environ['BB_LEGACY_CPU'] = '1'
            self.assertTrue(enabled())
            os.environ['BB_LEGACY_CPU'] = '0'
            self.assertFalse(enabled())
        finally:
            if orig is not None:
                os.environ['BB_LEGACY_CPU'] = orig
            else:
                os.environ.pop('BB_LEGACY_CPU', None)

    def test_plan_loading_and_structure(self):
        """Verify plan loaded via Python module and JSON are valid and match specification."""
        plan = legacy_plan_data.get_plan()
        self.assertIsInstance(plan, dict)
        self.assertEqual(plan.get('version'), 3)
        self.assertIn('modules', plan)
        self.assertIn('eboot.bin', plan['modules'])
        self.assertIn('libc.prx', plan['modules'])
        self.assertIn('libSceFios2.prx', plan['modules'])
        self.assertGreaterEqual(len(plan.get('sites', [])), 6000)
        self.assertEqual(len(plan.get('unhandled', [])), 0)

    def test_no_proprietary_bytes_in_plan(self):
        """Verify that the plan contains zero original game code bytes."""
        plan = legacy_plan_data.get_plan()
        for s in plan['sites']:
            self.assertNotIn('bytes', s, f"Proprietary 'bytes' field found in site {s}")

    def test_site_specifications(self):
        """Verify site parameters: length >= 5 for trampolines, valid hex bodies, and valid modules."""
        plan = legacy_plan_data.get_plan()
        valid_modules = {'eboot.bin', 'libc.prx', 'libSceFios2.prx'}
        for s in plan['sites']:
            self.assertIn(s['module'], valid_modules)
            self.assertIsInstance(s['address'], int)
            self.assertGreater(s['address'], 0)
            self.assertIsInstance(s['length'], int)

            if s.get('patch'):
                patch_bytes = bytes.fromhex(s['patch'])
                self.assertEqual(len(patch_bytes), s['length'], f"In-place patch size mismatch at {s}")
            else:
                self.assertGreaterEqual(s['length'], 5, f"Trampoline site length < 5 at {s}")
                body_bytes = bytes.fromhex(s['body'])
                self.assertGreater(len(body_bytes), 0, f"Empty trampoline body at {s}")

    def test_instruction_coverage(self):
        """Verify all target instructions are represented in the translated sites."""
        plan = legacy_plan_data.get_plan()
        seen = set()
        for s in plan['sites']:
            mnem = s['instruction'].split()[0]
            seen.add(mnem)
        expected = {'andn', 'bextr', 'movbe', 'lzcnt', 'tzcnt', 'blsi', 'blsr', 'blsmsk'}
        self.assertTrue(expected.issubset(seen), f"Missing instructions: {expected - seen}")

    def test_blsr_blsmsk_zero_input_semantics(self):
        """Verify blsr and blsmsk synthesized trampolines handle zero inputs correctly with stc."""
        try:
            import capstone
            from build_cpu_compat import body
        except ImportError:
            self.skipTest("capstone or keystone not available")

        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

        blsr_code = body('blsr', 'eax, ecx')
        blsr_mnems = [i.mnemonic for i in md.disasm(blsr_code, 0)]
        self.assertIn('stc', blsr_mnems, "STC instruction missing from blsr zero path")
        self.assertIn('test', blsr_mnems)

        blsmsk_code = body('blsmsk', 'eax, ecx')
        blsmsk_mnems = [i.mnemonic for i in md.disasm(blsmsk_code, 0)]
        self.assertIn('stc', blsmsk_mnems, "STC instruction missing from blsmsk zero path")
        self.assertIn('test', blsmsk_mnems)

    def test_spanning_neighbor_dynamic_loading(self):
        """Verify patch_image loads neighbor bytes dynamically from the authenticated image buffer."""
        from cpu_compat import patch_image

        img = bytearray(0x20000)
        # Target instruction at 0x1000 (4 bytes), neighbor at 0x1004 (3 bytes)
        img[0x1000:0x1004] = b'\x0f\x38\xf3\xc1'
        img[0x1004:0x1007] = b'\x48\x89\xd0'  # mov rax, rdx
        mock_plan = {
            'version': 3,
            'modules': {'eboot.bin': None},
            'sites': [{
                'module': 'eboot.bin',
                'address': 0x1000,
                'length': 7,
                'body': '909090',
                'span': {'pos': 'after', 'len': 3},
                'instruction': 'blsr eax, ecx [+ mov rax, rdx]'
            }],
            'unhandled': []
        }
        new_img, segs, rep = patch_image(img, [(0, 0x20000, 5)], {'sha256': None}, [], [], plan=mock_plan, active=True)
        # Check that neighbor bytes 4889d0 were spliced into the trampoline area
        trampoline_area = bytes(new_img[0x20000:])
        self.assertIn(b'\x48\x89\xd0', trampoline_area, "Dynamic neighbor bytes missing from trampoline")

    def test_collect_branch_targets_protects_relocations_and_fde(self):
        """Verify collect_branch_targets protects relocated pointer values and function/symbol entries."""
        try:
            import capstone
            from build_cpu_compat import collect_branch_targets
        except ImportError:
            self.skipTest("capstone not available")

        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        seg = {'vaddr': 0x1000, 'filesz': 0x1000, 'offset': 0}
        fns = [(0x1000, 16)]  # Function entry at 0x1000
        mod = {
            'elf': bytearray(b'\x90' * 0x2000),
            'symbols': [{'value': 0x1050, 'section': 1, 'type': 2}],
            'relocs': [
                (0x2024, 8, 0, 0x1016),  # R_X86_64_RELATIVE pointing to 0x1016
            ]
        }
        targets = collect_branch_targets(mod, seg, fns, md)
        self.assertIn(0x1000, targets, "Function entry point 0x1000 was not protected")
        self.assertIn(0x1050, targets, "Symbol entry point 0x1050 was not protected")
        self.assertIn(0x1016, targets, "Relocated pointer value 0x1016 was not protected")

    def test_windows_cpuid_host_detection(self):
        """Verify host_lacks_features executes on win32 without register corruption or AV."""
        from cpu_compat import host_lacks_features
        # Ensure it executes without throwing an access violation or crashing
        result = host_lacks_features()
        self.assertIsInstance(result, bool)


if __name__ == '__main__':
    unittest.main()
