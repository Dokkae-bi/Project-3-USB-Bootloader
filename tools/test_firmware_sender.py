"""Host tests for firmware_sender.py. No board or pyserial needed.

    python3 -m unittest tools/test_firmware_sender.py
"""

import os
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import firmware_sender as fs  # noqa: E402


class BuildFrame(unittest.TestCase):
    def test_header_matches_spec(self):
        payload = bytes(range(256)) * 3
        frame = fs.build_frame(payload)
        self.assertEqual(frame[:4], b"FWUP")
        self.assertEqual(frame[:4].hex(), "46575550")
        length, crc = struct.unpack("<II", frame[4:12])
        self.assertEqual(length, len(payload))
        self.assertEqual(crc, zlib.crc32(payload) & 0xFFFFFFFF)
        self.assertEqual(frame[12:], payload)
        self.assertEqual(len(frame), 12 + len(payload))

    def test_empty_rejected(self):
        with self.assertRaises(ValueError):
            fs.build_frame(b"")

    def test_max_size_accepted_and_one_over_rejected(self):
        fs.build_frame(b"\xAA" * fs.APP_BYTES)
        with self.assertRaises(ValueError):
            fs.build_frame(b"\xAA" * (fs.APP_BYTES + 1))

    def test_app_region_is_960_kb(self):
        self.assertEqual(fs.APP_BYTES, 983040)


class Main(unittest.TestCase):
    def _image(self, data):
        fh = tempfile.NamedTemporaryFile(delete=False)
        fh.write(data)
        fh.close()
        self.addCleanup(os.unlink, fh.name)
        return fh.name

    def test_dry_run_exits_zero(self):
        self.assertEqual(fs.main(["--dry-run", self._image(b"\x01" * 100)]), fs.EXIT_OK)

    def test_missing_file_is_error(self):
        self.assertEqual(fs.main(["--dry-run", "/nonexistent/app.bin"]), fs.EXIT_ERROR)

    def test_empty_file_is_error(self):
        self.assertEqual(fs.main(["--dry-run", self._image(b"")]), fs.EXIT_ERROR)

    def test_oversized_file_is_error(self):
        big = self._image(b"\x00" * (fs.APP_BYTES + 1))
        self.assertEqual(fs.main(["--dry-run", big]), fs.EXIT_ERROR)


if __name__ == "__main__":
    unittest.main()
