"""Sequence tracking as intervals."""

import random
import unittest

from holdfast.gaps import SeqTracker


class TestSeqTracker(unittest.TestCase):
    def test_in_order(self):
        t = SeqTracker()
        for i in range(10):
            self.assertTrue(t.add(i))
        self.assertEqual(t.intervals(), [(0, 9)])
        self.assertEqual(t.missing(0, 10), [])
        self.assertEqual(t.missing(0, 15), [(10, 14)])
        self.assertEqual(t.count, 10)
        self.assertEqual(t.highest, 9)

    def test_holes_and_merging(self):
        t = SeqTracker()
        for s in (0, 1, 2, 5, 6, 9):
            t.add(s)
        self.assertEqual(t.missing(0, 12), [(3, 4), (7, 8), (10, 11)])
        self.assertEqual(t.missing_count(0, 12), 6)
        self.assertEqual(t.missing(4, 9), [(4, 4), (7, 8)])
        self.assertEqual(t.missing(20, 25), [(20, 24)])
        self.assertEqual(t.missing(5, 5), [])
        self.assertFalse(t.add(5))  # duplicate
        t.add(3)
        t.add(4)
        self.assertEqual(t.intervals(), [(0, 6), (9, 9)])
        t.add(7)
        t.add(8)
        self.assertEqual(t.intervals(), [(0, 9)])

    def test_random_against_set(self):
        rng = random.Random(3)
        t = SeqTracker()
        ref = set()
        for _ in range(3000):
            s = rng.randrange(0, 500)
            self.assertEqual(t.add(s), s not in ref)
            ref.add(s)
            self.assertEqual(s in t, True)
        lo, hi = 37, 480
        want = []
        cur = None
        for s in range(lo, hi):
            if s not in ref:
                if cur is None:
                    cur = s
            elif cur is not None:
                want.append((cur, s - 1))
                cur = None
        if cur is not None:
            want.append((cur, hi - 1))
        self.assertEqual(t.missing(lo, hi), want)
        self.assertEqual(t.count, len(ref))
        # Intervals are disjoint, sorted, and cover exactly the set.
        iv = t.intervals()
        for (a, b), (c, d) in zip(iv, iv[1:]):
            self.assertLess(b + 1, c)
        self.assertEqual(sum(b - a + 1 for a, b in iv), len(ref))

    def test_empty(self):
        t = SeqTracker()
        self.assertIsNone(t.highest)
        self.assertEqual(t.missing(0, 5), [(0, 4)])
        self.assertNotIn(0, t)


if __name__ == "__main__":
    unittest.main()
