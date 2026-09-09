"""Steim2 in Python: round trips, capacity, corruption."""

import random
import struct
import unittest

from holdfast import steim2


class TestSteim2(unittest.TestCase):
    def test_constant_signal_capacity(self):
        frames, n = steim2.encode([1000] * 1000, 1)
        self.assertEqual(n, 13 * 7)
        self.assertEqual(steim2.decode(frames, n), [1000] * n)
        frames, n = steim2.encode([1000] * 1000, 3)
        self.assertEqual(n, 13 * 7 + 2 * 15 * 7)

    def test_single_sample(self):
        frames, n = steim2.encode([-123456], 3)
        self.assertEqual((n, len(frames)), (1, 64))
        self.assertEqual(steim2.decode(frames, 1), [-123456])

    def test_width_boundaries(self):
        edges = [7, -8, 15, -16, 31, -32, 127, -128, 511, -512, 16383, -16384, (1 << 29) - 1]
        x = [0]
        for e in edges:
            x.append(x[-1] + e)
            x.append(x[-1] - e)
        x.append(x[-1] - (1 << 29))
        x.append(x[-1] + (1 << 28))
        x.append(x[-1] + (1 << 28))
        frames, n = steim2.encode(x, 4)
        self.assertEqual(n, len(x))
        self.assertEqual(steim2.decode(frames, n), x)

    def test_random_widths_multi_frame(self):
        rng = random.Random(7)
        x = [0]
        while len(x) < 1500:
            bits = rng.randint(1, 29)
            v = x[-1] + rng.randint(-(1 << (bits - 1)), (1 << (bits - 1)) - 1)
            if not -(1 << 27) <= v < (1 << 27):
                v = -x[-1] // 2
            x.append(v)
        total = 0
        while total < len(x):
            frames, n = steim2.encode(x[total:], 3)
            self.assertGreaterEqual(n, 1)
            self.assertEqual(steim2.decode(frames, n), x[total:total + n])
            total += n

    def test_range_error(self):
        with self.assertRaises(steim2.Steim2Error):
            steim2.encode([0, 1 << 29], 1)

    def test_corruption(self):
        x = [i * i for i in range(50)]
        frames, n = steim2.encode(x, 3)
        bad = bytearray(frames)
        struct.pack_into(">I", bad, 8, 0xDEADBEEF)
        with self.assertRaises(steim2.Steim2Error):
            steim2.decode(bytes(bad), n)
        with self.assertRaises(steim2.Steim2Error):
            steim2.decode(frames, n + 1)
        with self.assertRaises(steim2.Steim2Error):
            steim2.decode(frames[:-1], n)
        self.assertEqual(steim2.decode(frames, 0), [])


if __name__ == "__main__":
    unittest.main()
