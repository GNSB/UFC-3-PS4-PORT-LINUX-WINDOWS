# SPDX-License-Identifier: GPL-2.0-only
"""Pre-compiled Ivy Bridge compatibility translation plan for UFC 3 PS4 Port (CUSA06534).
Zero external dependencies (uses standard library LZMA + Base64).
"""
import base64
import json
import lzma
from legacy_plan_p1 import PART1
from legacy_plan_p2 import PART2
from legacy_plan_p3 import PART3

def get_plan():
    """Returns the decompressed compatibility plan dictionary."""
    combined = PART1 + PART2 + PART3
    return json.loads(lzma.decompress(base64.b64decode(combined.encode('ascii'))))
