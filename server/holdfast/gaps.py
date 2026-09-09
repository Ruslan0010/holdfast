"""Which sequence numbers have arrived, and which have not.

A set of received sequence numbers kept as sorted, disjoint, inclusive
intervals. Data arrives mostly in order with bursts of loss, so the interval
list stays short even after millions of packets; memory is proportional to
the number of holes, not the number of packets.
"""

from __future__ import annotations

import bisect


class SeqTracker:
    def __init__(self) -> None:
        self._starts: list[int] = []
        self._ends: list[int] = []  # inclusive
        self.count = 0

    def __contains__(self, seq: int) -> bool:
        i = bisect.bisect_right(self._starts, seq) - 1
        return i >= 0 and self._ends[i] >= seq

    def add(self, seq: int) -> bool:
        """Record seq. Returns True if it was new."""
        if seq in self:
            return False
        i = bisect.bisect_right(self._starts, seq)
        # Merge with the interval on the left and/or right if adjacent.
        left = i - 1 if i > 0 and self._ends[i - 1] + 1 == seq else None
        right = i if i < len(self._starts) and self._starts[i] == seq + 1 else None
        if left is not None and right is not None:
            self._ends[left] = self._ends[right]
            del self._starts[right]
            del self._ends[right]
        elif left is not None:
            self._ends[left] = seq
        elif right is not None:
            self._starts[right] = seq
        else:
            self._starts.insert(i, seq)
            self._ends.insert(i, seq)
        self.count += 1
        return True

    def missing(self, lo: int, hi: int) -> list[tuple[int, int]]:
        """Inclusive ranges within [lo, hi) that have not been received."""
        if hi <= lo:
            return []
        out: list[tuple[int, int]] = []
        cursor = lo
        i = max(bisect.bisect_right(self._starts, lo) - 1, 0)
        while i < len(self._starts) and cursor < hi:
            s, e = self._starts[i], self._ends[i]
            if e < cursor:
                i += 1
                continue
            if s > cursor:
                out.append((cursor, min(s - 1, hi - 1)))
            cursor = max(cursor, e + 1)
            i += 1
        if cursor < hi:
            out.append((cursor, hi - 1))
        return out

    def missing_count(self, lo: int, hi: int) -> int:
        return sum(b - a + 1 for a, b in self.missing(lo, hi))

    def intervals(self) -> list[tuple[int, int]]:
        return list(zip(self._starts, self._ends))

    @property
    def highest(self) -> int | None:
        return self._ends[-1] if self._ends else None
