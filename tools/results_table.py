#!/usr/bin/env python3
"""Print the integration results as a Markdown table for the README.

    make integration
    python3 tools/results_table.py build/integration_results.json
"""

from __future__ import annotations

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from netsim import PROFILES  # noqa: E402

ORDER = ["clean", "clean-raw", "starlink", "4g-mountain", "dying-modem",
         "dying-modem-ratelimit", "dying-modem-tiny-ring", "restart"]

NOTES = {
    "clean": "",
    "clean-raw": "no compression, for comparison",
    "starlink": "",
    "4g-mountain": "",
    "dying-modem": "",
    "dying-modem-ratelimit": "link capped at 6 kB/s, below the live rate",
    "dying-modem-tiny-ring": "retention of 8 packets: loss is expected and reported",
    "restart": "station killed and restarted mid-stream with a persisted counter",
}


def link(profile: str) -> str:
    p = PROFILES[profile]
    s = f"{p['loss'] * 100:.0f}% loss, {p['delay_ms']}±{p['jitter_ms']} ms"
    if p["blackout_every_s"]:
        s += f", {p['blackout_len_s']} s blackout / {p['blackout_every_s']} s"
    return s


def main() -> None:
    path = sys.argv[1] if len(sys.argv) > 1 else "build/integration_results.json"
    results = {r["name"]: r for r in json.load(open(path))}
    print("| run | link | encoding | recovered | packets | resent | dropped by link | bytes/sample | note |")
    print("|-----|------|----------|----------:|--------:|-------:|----------------:|-------------:|------|")
    for name in ORDER:
        r = results.get(name)
        if r is None:
            continue
        rec = f"{r['recovered_pct']:.1f} %" + (" ✔ byte-for-byte" if r["byte_for_byte"] and r["recovered_pct"] == 100 else "")
        print(f"| {name} | {link(r['profile'])} | {r['encoding']} | {rec} | {r['packets_built']} | "
              f"{r['retransmits']} | {r['link_dropped']} | {r['bytes_per_sample_on_wire']:.2f} | {NOTES.get(name, '')} |")


if __name__ == "__main__":
    main()
