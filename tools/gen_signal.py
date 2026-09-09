#!/usr/bin/env python3
"""Generate a synthetic seismogram as raw little-endian int32 samples.

Background microseism (band-limited noise with a period of a few seconds) plus
a handful of events: decaying sinusoidal bursts with a sharp onset, the way a
local earthquake looks on a short-period sensor. Amplitudes are in ADC counts
for a 24-bit digitiser, so the differences between consecutive samples span
the whole range of Steim2 field widths.

    python3 tools/gen_signal.py --seconds 600 --rate 100 --seed 1 --out signal.i32

Pure Python, deterministic for a given seed.
"""

from __future__ import annotations

import argparse
import math
import random
import struct
import sys


def generate(seconds: float, rate: int, seed: int, events: int) -> list[int]:
    rng = random.Random(seed)
    n = int(seconds * rate)
    dt = 1.0 / rate

    # Microseism: white noise through two first-order low-pass stages, which
    # gives a smooth wander with most energy below ~0.5 Hz.
    alpha = math.exp(-2 * math.pi * 0.3 * dt)
    y1 = y2 = 0.0
    background = []
    for _ in range(n):
        y1 = alpha * y1 + (1 - alpha) * rng.gauss(0, 1)
        y2 = alpha * y2 + (1 - alpha) * y1
        background.append(y2)
    scale = 1500.0 / (max(abs(v) for v in background) or 1.0)
    signal = [v * scale + rng.gauss(0, 20) for v in background]

    # Events: onset, then a decaying wavelet at 2-8 Hz.
    for _ in range(events):
        onset = rng.randrange(int(n * 0.1), int(n * 0.9))
        freq = rng.uniform(2.0, 8.0)
        amp = rng.uniform(50_000, 800_000)
        decay = rng.uniform(1.5, 6.0)  # seconds
        for i in range(onset, min(n, onset + int(decay * 5 * rate))):
            t = (i - onset) * dt
            signal[i] += amp * math.exp(-t / decay) * math.sin(2 * math.pi * freq * t) * (1 - math.exp(-t * 20))

    limit = (1 << 23) - 1
    return [max(-limit, min(limit, int(round(v)))) for v in signal]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--seconds", type=float, default=600)
    ap.add_argument("--rate", type=int, default=100)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--events", type=int, default=3)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    samples = generate(args.seconds, args.rate, args.seed, args.events)
    with open(args.out, "wb") as f:
        f.write(struct.pack(f"<{len(samples)}i", *samples))
    peak = max(abs(s) for s in samples)
    diffs = [abs(b - a) for a, b in zip(samples, samples[1:])]
    print(f"{args.out}: {len(samples)} samples, {args.seconds:g} s at {args.rate} Hz, "
          f"peak {peak} counts, median |diff| {sorted(diffs)[len(diffs) // 2]}, "
          f"max |diff| {max(diffs)}", file=sys.stderr)


if __name__ == "__main__":
    main()
