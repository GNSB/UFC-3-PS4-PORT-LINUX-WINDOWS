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


if __name__ == '__main__':
    unittest.main()
