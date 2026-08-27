import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).parents[2] / "mir-android2-platform-gud" / "tools" / "e4-t02-reference.py"
SPEC = importlib.util.spec_from_file_location("e4_t02_reference", MODULE_PATH)
REFERENCE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(REFERENCE)


class ReferencePatternTests(unittest.TestCase):
    def test_reference_is_deterministic_and_tightly_packed(self):
        first = REFERENCE.generate_rgb565()
        second = REFERENCE.generate_rgb565()
        self.assertEqual(first, second)
        self.assertEqual(len(first), 1280 * 720 * 2)
        self.assertEqual(first[0:2], REFERENCE.rgb565(REFERENCE.YELLOW).to_bytes(2, "little"))
        self.assertEqual(first[8 * 2 : 8 * 2 + 2], REFERENCE.rgb565(REFERENCE.GREEN).to_bytes(2, "little"))

    def test_reference_preserves_distinct_edges_and_corners(self):
        raw = REFERENCE.generate_rgb565()

        def pixel(x, y):
            offset = y * REFERENCE.PITCH + x * 2
            return int.from_bytes(raw[offset : offset + 2], "little")

        self.assertEqual(pixel(0, 0), REFERENCE.rgb565(REFERENCE.YELLOW))
        self.assertEqual(pixel(1279, 0), REFERENCE.rgb565(REFERENCE.CYAN))
        self.assertEqual(pixel(0, 719), REFERENCE.rgb565(REFERENCE.MAGENTA))
        self.assertEqual(pixel(1279, 719), REFERENCE.rgb565(REFERENCE.ORANGE))
        self.assertEqual(pixel(0, 8), REFERENCE.rgb565(REFERENCE.RED))
        self.assertEqual(pixel(1279, 8), REFERENCE.rgb565(REFERENCE.BLUE))
        self.assertEqual(pixel(8, 0), REFERENCE.rgb565(REFERENCE.GREEN))
        self.assertEqual(pixel(8, 719), REFERENCE.rgb565(REFERENCE.WHITE))
