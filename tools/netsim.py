#!/usr/bin/env python3
"""A hostile network in one process.

A UDP relay that sits between the station and the server and behaves like a
bad link: packet loss, delay with jitter (which reorders), duplication, and
scheduled blackouts. It also behaves like a NAT: the server's replies go to
whatever address the station last sent from, and nothing reaches the
station that it did not first solicit.

    python3 tools/netsim.py --listen 127.0.0.1:5001 --upstream 127.0.0.1:5000 \
        --profile dying-modem

Point the station at the listen address and the server sees the upstream
side. Profiles are named after the links they imitate; --time-scale shrinks
the blackout schedule for tests without changing per-packet behaviour.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import random
import signal
import sys
import time

PROFILES = {
    "clean": dict(loss=0.0, delay_ms=1, jitter_ms=0, dup=0.0,
                  blackout_every_s=0, blackout_len_s=0),
    "starlink": dict(loss=0.02, delay_ms=40, jitter_ms=20, dup=0.001,
                     blackout_every_s=600, blackout_len_s=30),
    "4g-mountain": dict(loss=0.15, delay_ms=300, jitter_ms=100, dup=0.01,
                        blackout_every_s=300, blackout_len_s=60),
    "dying-modem": dict(loss=0.30, delay_ms=500, jitter_ms=300, dup=0.02,
                        blackout_every_s=300, blackout_len_s=60),
    "satellite": dict(loss=0.05, delay_ms=600, jitter_ms=50, dup=0.0,
                      blackout_every_s=900, blackout_len_s=120),
}


class Link:
    """Impairment shared by both directions."""

    def __init__(self, profile: dict, time_scale: float, seed: int):
        self.p = profile
        self.time_scale = time_scale
        self.rng = random.Random(seed)
        self.blackout = False
        self.stats = {
            "up": {"in": 0, "forwarded": 0, "dropped": 0, "blackout": 0, "dup": 0, "bytes_in": 0, "bytes_out": 0},
            "down": {"in": 0, "forwarded": 0, "dropped": 0, "blackout": 0, "dup": 0, "bytes_in": 0, "bytes_out": 0},
            "blackouts": 0,
        }

    def impair(self, direction: str, data: bytes, send) -> None:
        s = self.stats[direction]
        s["in"] += 1
        s["bytes_in"] += len(data)
        if self.blackout:
            s["blackout"] += 1
            return
        if self.rng.random() < self.p["loss"]:
            s["dropped"] += 1
            return
        copies = 1 + (1 if self.rng.random() < self.p["dup"] else 0)
        for c in range(copies):
            delay = self.p["delay_ms"] + self.rng.uniform(-self.p["jitter_ms"], self.p["jitter_ms"])
            delay = max(0.0, delay) / 1000.0
            asyncio.get_running_loop().call_later(delay, self._deliver, direction, data, send)
            if c:
                s["dup"] += 1

    def _deliver(self, direction: str, data: bytes, send) -> None:
        s = self.stats[direction]
        try:
            send(data)
        except OSError:
            s["dropped"] += 1
            return
        s["forwarded"] += 1
        s["bytes_out"] += len(data)

    async def blackout_schedule(self) -> None:
        every = self.p["blackout_every_s"] * self.time_scale
        length = self.p["blackout_len_s"] * self.time_scale
        if every <= 0 or length <= 0:
            return
        while True:
            await asyncio.sleep(max(0.0, every - length))
            self.blackout = True
            self.stats["blackouts"] += 1
            print(json.dumps({"t": round(time.time(), 1), "blackout": "start"}), file=sys.stderr, flush=True)
            await asyncio.sleep(length)
            self.blackout = False
            print(json.dumps({"t": round(time.time(), 1), "blackout": "end"}), file=sys.stderr, flush=True)


class Downstream(asyncio.DatagramProtocol):
    """Faces the station. Remembers where the station last sent from."""

    def __init__(self, link: Link):
        self.link = link
        self.transport = None
        self.station_addr = None
        self.upstream: Upstream | None = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        self.station_addr = addr
        if self.upstream and self.upstream.transport:
            self.link.impair("up", data, self.upstream.transport.sendto)

    def send_to_station(self, data: bytes) -> None:
        if self.station_addr and self.transport:
            self.transport.sendto(data, self.station_addr)


class Upstream(asyncio.DatagramProtocol):
    """Faces the server over a connected socket."""

    def __init__(self, link: Link, downstream: Downstream):
        self.link = link
        self.downstream = downstream
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        self.link.impair("down", data, self.downstream.send_to_station)

    def error_received(self, exc):
        pass  # ICMP unreachable while the server is down: just loss


def parse_addr(s: str) -> tuple[str, int]:
    host, _, port = s.rpartition(":")
    return host, int(port)


async def run(args: argparse.Namespace) -> None:
    profile = dict(PROFILES[args.profile])
    for override in args.set:
        k, v = override.split("=")
        profile[k] = type(profile[k])(v)
    link = Link(profile, args.time_scale, args.seed)
    loop = asyncio.get_running_loop()

    down = Downstream(link)
    await loop.create_datagram_endpoint(lambda: down, local_addr=parse_addr(args.listen))
    up = Upstream(link, down)
    await loop.create_datagram_endpoint(lambda: up, remote_addr=parse_addr(args.upstream))
    down.upstream = up

    print(json.dumps({"profile": args.profile, "params": profile, "time_scale": args.time_scale}),
          file=sys.stderr, flush=True)

    stop = asyncio.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)
    sched = asyncio.create_task(link.blackout_schedule())

    async def report():
        while args.stats_every > 0:
            await asyncio.sleep(args.stats_every)
            print(json.dumps({"t": round(time.time(), 1), **link.stats}), file=sys.stderr, flush=True)

    rep = asyncio.create_task(report())
    await stop.wait()
    sched.cancel()
    rep.cancel()
    final = {"final": True, "profile": args.profile, **link.stats}
    print(json.dumps(final), file=sys.stderr, flush=True)
    if args.stats_file:
        with open(args.stats_file, "w") as f:
            json.dump(final, f, indent=1)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--listen", default="127.0.0.1:5001")
    ap.add_argument("--upstream", default="127.0.0.1:5000")
    ap.add_argument("--profile", choices=sorted(PROFILES), default="dying-modem")
    ap.add_argument("--set", action="append", default=[], metavar="KEY=VALUE",
                    help="override a profile parameter, e.g. loss=0.5")
    ap.add_argument("--time-scale", type=float, default=1.0,
                    help="multiply blackout period and length (tests use 0.05)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--stats-every", type=float, default=0)
    ap.add_argument("--stats-file", default="")
    asyncio.run(run(ap.parse_args()))


if __name__ == "__main__":
    main()
