"""Steim2 codec in pure Python, written from the format description and not
from the C code, so the two check each other. See firmware/include/steim2.h
for the frame layout.

The encoder exists for tests and tooling; the server only needs decode().
"""

from __future__ import annotations

import struct

FRAME_LEN = 64
_WORDS = 16

# (count, bits, nibble, dnib), densest first. nibble 1 has no dnib.
_PACKINGS = (
    (7, 4, 3, 2),
    (6, 5, 3, 1),
    (5, 6, 3, 0),
    (4, 8, 1, 0),
    (3, 10, 2, 3),
    (2, 15, 2, 2),
    (1, 30, 2, 1),
)


class Steim2Error(ValueError):
    pass


def _fits(d: int, bits: int) -> bool:
    lim = 1 << (bits - 1)
    return -lim <= d < lim


def _sext(v: int, bits: int) -> int:
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def encode(samples: list[int], max_frames: int) -> tuple[bytes, int]:
    """Encode as many samples as fit in up to max_frames frames.

    Returns (frames, n_consumed). The first difference is stored as 0 so each
    result is self-contained.
    """
    if max_frames < 1:
        raise Steim2Error("size")
    n = len(samples)
    if n == 0:
        return b"", 0

    def diff(i: int) -> int:
        return 0 if i == 0 else samples[i] - samples[i - 1]

    out = bytearray()
    i = 0
    frame = 0
    while frame < max_frames and i < n:
        words = [0] * _WORDS
        nibbles = 0
        w = 3 if frame == 0 else 1
        while w < _WORDS and i < n:
            for k, bits, nibble, dnib in _PACKINGS:
                if i + k <= n and all(_fits(diff(i + j), bits) for j in range(k)):
                    break
            else:
                raise Steim2Error("range")
            mask = (1 << bits) - 1
            word = 0
            for j in range(k):
                word = (word << bits) | (diff(i + j) & mask)
            if nibble != 1:
                word |= dnib << 30
            words[w] = word & 0xFFFFFFFF
            nibbles |= nibble << (2 * (15 - w))
            i += k
            w += 1
        words[0] = nibbles
        out += struct.pack(">16I", *words)
        frame += 1

    struct.pack_into(">II", out, 4, samples[0] & 0xFFFFFFFF, samples[i - 1] & 0xFFFFFFFF)
    return bytes(out), i


def _layout(nibble: int, word: int) -> tuple[int, int]:
    dnib = word >> 30
    if nibble == 1:
        return 4, 8
    if nibble == 2:
        return {1: (1, 30), 2: (2, 15), 3: (3, 10)}.get(dnib, (0, 0))
    if nibble == 3:
        return {0: (5, 6), 1: (6, 5), 2: (7, 4)}.get(dnib, (0, 0))
    return 0, 0


def decode(frames: bytes, n_expected: int) -> list[int]:
    if len(frames) % FRAME_LEN != 0:
        raise Steim2Error("format")
    # Zero samples occupy zero frames: decode() must invert encode() at the
    # empty case too. Mirrors firmware/src/steim2.c.
    if n_expected == 0:
        return []
    if not frames:
        raise Steim2Error("short")

    x0, xn = struct.unpack_from(">ii", frames, 4)
    out: list[int] = []
    cur = 0
    for f in range(len(frames) // FRAME_LEN):
        words = struct.unpack_from(">16I", frames, f * FRAME_LEN)
        nibbles = words[0]
        for w in range(3 if f == 0 else 1, _WORDS):
            if len(out) >= n_expected:
                break
            nibble = (nibbles >> (2 * (15 - w))) & 3
            if nibble == 0:
                continue
            k, bits = _layout(nibble, words[w])
            if k == 0:
                raise Steim2Error("format")
            for j in range(k):
                if len(out) >= n_expected:
                    break
                d = _sext(words[w] >> (bits * (k - 1 - j)), bits)
                cur = x0 if not out else cur + d
                if not -(1 << 31) <= cur < (1 << 31):
                    raise Steim2Error("format")
                out.append(cur)
    if len(out) < n_expected:
        raise Steim2Error("short")
    if cur != xn:
        raise Steim2Error("integrity")
    return out
