"""Cross-implementation check: datagrams and frames produced by the C code
must decode in Python to the values the C code was given, and Python must
produce the identical bytes for the same input.

Reads build/vectors.txt, written by tests/vectors/gen_vectors.c via
`make vectors`.
"""

import os
import struct
import unittest

from holdfast import protocol as p
from holdfast import steim2

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
VECTORS = os.path.join(ROOT, "build", "vectors.txt")


def load():
    out = {}
    with open(VECTORS) as f:
        for line in f:
            name, value = line.split()
            out[name] = value
    return out


@unittest.skipUnless(os.path.exists(VECTORS), "run `make vectors` first")
class TestCrossImplementation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.v = load()

    def test_data(self):
        buf = bytes.fromhex(self.v["DATA"])
        d = p.decode(buf)
        want = p.Data(0x00C0FFEE, 7, p.CLOCK_LOCKED, p.ENC_RAW, 0x0102030405060708,
                      1757400000123456789, 10000, 2, bytes((i * 13 + 1) & 0xFF for i in range(8)))
        self.assertEqual(d, want)
        self.assertEqual(p.encode_data(want), buf)

    def test_heartbeat(self):
        buf = bytes.fromhex(self.v["HEARTBEAT"])
        want = p.Heartbeat(42, 1000, 900, 86400, p.CLOCK_HOLDOVER)
        self.assertEqual(p.decode(buf), want)
        self.assertEqual(p.encode_heartbeat(want), buf)

    def test_gaps(self):
        buf = bytes.fromhex(self.v["GAPS"])
        want = p.Gaps(42, ((10, 20), (100, 100)))
        self.assertEqual(p.decode(buf), want)
        self.assertEqual(p.encode_gaps(want), buf)

    def test_steim2_frames(self):
        x = [(i * i) % 1000 - 500 + (40000 * (i % 3 - 1) if i % 37 == 0 else 0)
             for i in range(400)]
        n = int(self.v["STEIM2_CONSUMED"])
        frames = bytes.fromhex(self.v["STEIM2"])
        self.assertEqual(steim2.decode(frames, n), x[:n])
        py_frames, py_n = steim2.encode(x, 3)
        self.assertEqual(py_n, n)
        self.assertEqual(py_frames, frames)

    def test_steim2_packet_end_to_end(self):
        d = p.decode(bytes.fromhex(self.v["DATA_STEIM2"]))
        self.assertEqual(d.encoding, p.ENC_STEIM2)
        x = [(i * i) % 1000 - 500 + (40000 * (i % 3 - 1) if i % 37 == 0 else 0)
             for i in range(400)]
        self.assertEqual(p.decode_samples(d), x[:d.sample_count])


if __name__ == "__main__":
    unittest.main()
