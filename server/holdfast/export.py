"""Reassemble a station's samples from the database and, optionally, verify
them against the original signal byte for byte.

    python3 -m holdfast.export --db holdfast.db --station 1 --out got.i32 \
        --verify signal.i32

Exit status is 0 only when every sample matches. The JSON summary on stdout
is what the integration tests and the README table are built from.
"""

from __future__ import annotations

import argparse
import json
import sys

from .store import Store, pack_samples


def reassemble(store: Store, station_id: int) -> tuple[list[int], list[dict]]:
    """Samples in sequence order plus a list of discontinuities."""
    samples: list[int] = []
    problems: list[dict] = []
    prev_seq = None
    prev_end_ns = None
    for seq, t0_ns, period_us, s in store.packets(station_id):
        if prev_seq is not None and seq != prev_seq + 1:
            problems.append({"kind": "missing_seq", "from": prev_seq + 1, "to": seq - 1})
        if prev_end_ns is not None and t0_ns != prev_end_ns:
            problems.append({"kind": "time_jump", "seq": seq, "expected_ns": prev_end_ns,
                             "got_ns": t0_ns})
        samples.extend(s)
        prev_seq = seq
        prev_end_ns = t0_ns + len(s) * period_us * 1000
    return samples, problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--db", required=True)
    ap.add_argument("--station", type=int, required=True)
    ap.add_argument("--out", help="write samples as little-endian int32")
    ap.add_argument("--verify", help="source signal to compare against")
    args = ap.parse_args(argv)

    store = Store(args.db)
    samples, problems = reassemble(store, args.station)
    n_packets, _, wire_bytes = store.packet_stats(args.station)
    unrecoverable = store.unrecoverable(args.station)
    store.close()

    got = pack_samples(samples)
    if args.out:
        with open(args.out, "wb") as f:
            f.write(got)

    summary = {
        "station": args.station,
        "packets": n_packets,
        "samples": len(samples),
        "wire_bytes": wire_bytes,
        "bytes_per_sample": round(wire_bytes / len(samples), 3) if samples else None,
        "problems": problems,
        "unrecoverable_ranges": unrecoverable,
    }

    ok = not problems
    if args.verify:
        with open(args.verify, "rb") as f:
            src = f.read()
        n_src = len(src) // 4
        match = got == src[:len(got)]
        first_bad = None
        if not match:
            for i in range(0, min(len(got), len(src)), 4):
                if got[i:i + 4] != src[i:i + 4]:
                    first_bad = i // 4
                    break
        summary["verify"] = {
            "source_samples": n_src,
            "recovered_samples": len(samples),
            "recovered_pct": round(100.0 * len(samples) / n_src, 3) if n_src else None,
            "byte_for_byte": match,
            "first_mismatch_sample": first_bad,
        }
        ok = ok and match and len(samples) == n_src

    print(json.dumps(summary, indent=1))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
