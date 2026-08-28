import struct
import sys
import unittest
from pathlib import Path


UTILS = Path(__file__).resolve().parents[1] / "src" / "utils"
sys.path.insert(0, str(UTILS))

from h264_frame_protocol import (  # noqa: E402
    HEADER_SIZE,
    MAGIC,
    MAX_PAYLOAD_SIZE,
    frame_access_unit,
    make_header,
)


class H264FrameProtocolTests(unittest.TestCase):
    def test_header_matches_receiver_layout(self):
        header = make_header(0x0102030405060708, 123456789, 4096)

        self.assertEqual(len(header), HEADER_SIZE)
        self.assertEqual(header[:8], MAGIC)
        self.assertEqual(header[8:12], b"\x01\x00\x00\x00")
        self.assertEqual(
            struct.unpack(">QQI", header[12:]),
            (0x0102030405060708, 123456789, 4096),
        )

    def test_frames_access_unit_without_modifying_it(self):
        access_unit = b"\x00\x00\x00\x01\x65payload"
        framed = frame_access_unit(7, 9, access_unit)

        self.assertEqual(framed[HEADER_SIZE:], access_unit)
        self.assertEqual(struct.unpack(">I", framed[28:32])[0], len(access_unit))

    def test_rejects_invalid_ranges(self):
        for arguments in [(-1, 1, 1), (1, -1, 1), (1, 1, 0)]:
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                make_header(*arguments)
        with self.assertRaises(ValueError):
            make_header(1, 1, MAX_PAYLOAD_SIZE + 1)


if __name__ == "__main__":
    unittest.main()
