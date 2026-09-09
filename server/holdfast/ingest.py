"""The ingest server.

Listens on one UDP port for every station. For each station it keeps the set
of sequence numbers received, learns the retention window from heartbeats,
and periodically asks for whatever is missing and still recoverable. Samples
go to SQLite as they arrive; a status file is rewritten every second for
dashboards and tests.

The server never opens a connection to a station. It cannot: the station is
behind carrier-grade NAT. It replies to the address the station's last
datagram came from, which is the NAT mapping the station's heartbeats keep
alive.

Run:  python3 -m holdfast.ingest --port 5000 --db holdfast.db --status status.json
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import signal
import time

from . import protocol
from .gaps import SeqTracker
from .store import Store

log = logging.getLogger("holdfast.ingest")


def now_ns() -> int:
    return time.time_ns()


class StationState:
    def __init__(self, station_id: int, gap_interval: float, at: float):
        self.id = station_id
        self.addr: tuple[str, int] | None = None
        self.tracker = SeqTracker()
        self.next_seq = 0
        self.oldest_seq = 0
        self.expect_from: int | None = None  # lowest seq ever worth wanting
        self.unrec_upto: int | None = None  # unrecoverable recorded below this
        self.unrecoverable = 0
        self.clock_quality = 0
        self.uptime_s = 0
        self.first_seen = at
        self.last_heartbeat: float | None = None
        self.last_data: float | None = None
        # counters
        self.packets = 0
        self.late = 0
        self.duplicates = 0
        self.bytes = 0
        self.bad_payload = 0
        self.gap_requests = 0
        # backfill scheduling
        self.gap_interval = gap_interval
        self.last_gap_request = 0.0
        self.packets_at_last_request = 0
        # link sample deltas
        self._ls = (0, 0, 0, 0)

    @property
    def lowest_wanted(self) -> int:
        base = self.expect_from if self.expect_from is not None else 0
        return max(base, self.oldest_seq)

    def missing(self) -> list[tuple[int, int]]:
        return self.tracker.missing(self.lowest_wanted, self.next_seq)

    def missing_count(self) -> int:
        return self.tracker.missing_count(self.lowest_wanted, self.next_seq)

    def complete(self) -> bool:
        return self.next_seq > 0 and self.missing_count() == 0

    def note_seq_bound(self, seq: int) -> None:
        if self.expect_from is None or seq < self.expect_from:
            self.expect_from = seq


class Ingest(asyncio.DatagramProtocol):
    def __init__(self, store: Store, args: argparse.Namespace):
        self.store = store
        self.args = args
        self.stations: dict[int, StationState] = {}
        self.transport: asyncio.DatagramTransport | None = None
        self.datagrams = 0
        self.bad = 0
        self.started = time.monotonic()
        self._restore()

    # --- persistence of state across restarts ---------------------------

    def _restore(self) -> None:
        for row in self.store.stations():
            st = StationState(row["station_id"], self.args.gap_interval, time.monotonic())
            st.next_seq = row["next_seq"]
            st.oldest_seq = row["oldest_seq"]
            st.clock_quality = row["clock_quality"]
            st.uptime_s = row["uptime_s"]
            if row["last_addr"]:
                host, _, port = row["last_addr"].rpartition(":")
                st.addr = (host, int(port))
            for seq in self.store.received_seqs(st.id):
                st.tracker.add(seq)
                st.note_seq_bound(seq)
            for a, b in self.store.unrecoverable(st.id):
                st.unrecoverable += b - a + 1
                st.unrec_upto = max(st.unrec_upto or 0, b + 1)
            self.stations[st.id] = st
            log.info("restored station %d: %d packets, next_seq %d, oldest_seq %d",
                     st.id, st.tracker.count, st.next_seq, st.oldest_seq)

    # --- receive ----------------------------------------------------------

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        self.transport = transport  # type: ignore[assignment]

    def datagram_received(self, data: bytes, addr: tuple[str, int]) -> None:
        self.datagrams += 1
        try:
            msg = protocol.decode(data)
        except protocol.ProtocolError as e:
            self.bad += 1
            if self.bad <= 10 or self.bad % 100 == 0:
                log.warning("bad datagram from %s: %s (%d bytes, %d so far)",
                            addr, e.reason, len(data), self.bad)
            return

        st = self._station(msg.station_id)
        st.addr = addr
        mono = time.monotonic()
        self.store.touch_station(st.id, now_ns(), f"{addr[0]}:{addr[1]}")

        if isinstance(msg, protocol.Data):
            self._on_data(st, msg, len(data), mono)
        elif isinstance(msg, protocol.Heartbeat):
            self._on_heartbeat(st, msg, mono)
        else:
            self.bad += 1
            log.warning("station %d sent a %s, ignoring", st.id, type(msg).__name__)

    def _station(self, station_id: int) -> StationState:
        st = self.stations.get(station_id)
        if st is None:
            st = StationState(station_id, self.args.gap_interval, time.monotonic())
            self.stations[station_id] = st
            self.store.event(now_ns(), station_id, "first_seen")
            log.info("station %d: first contact", station_id)
        return st

    def _on_data(self, st: StationState, d: protocol.Data, wire_len: int, mono: float) -> None:
        try:
            samples = protocol.decode_samples(d)
        except Exception as e:  # steim2 or struct errors
            st.bad_payload += 1
            self.store.event(now_ns(), st.id, "bad_payload", f"seq {d.seq}: {e}")
            log.warning("station %d seq %d: payload rejected: %s", st.id, d.seq, e)
            return

        st.note_seq_bound(d.seq)
        highest = st.tracker.highest
        late = highest is not None and d.seq < highest
        st.last_data = mono
        st.bytes += wire_len
        if d.seq + 1 > st.next_seq:
            st.next_seq = d.seq + 1

        if not st.tracker.add(d.seq):
            st.duplicates += 1
            return
        st.packets += 1
        if late:
            st.late += 1
        self.store.insert_packet(st.id, d.seq, d.stream_id, d.t0_ns, d.sample_period_us,
                                 d.encoding, d.clock_quality, now_ns(), late, wire_len,
                                 samples)

    def _on_heartbeat(self, st: StationState, h: protocol.Heartbeat, mono: float) -> None:
        first = st.last_heartbeat is None
        st.last_heartbeat = mono
        st.clock_quality = h.clock_quality
        st.uptime_s = h.uptime_s
        if h.next_seq > st.next_seq:
            st.next_seq = h.next_seq
        if h.oldest_seq != st.oldest_seq or first:
            st.oldest_seq = h.oldest_seq
            st.note_seq_bound(h.oldest_seq)
            self._account_unrecoverable(st)
        self.store.update_heartbeat(st.id, st.next_seq, st.oldest_seq, st.clock_quality,
                                    st.uptime_s)

    def _account_unrecoverable(self, st: StationState) -> None:
        """Anything below oldest_seq that never arrived is gone for good.
        Record it once so it stops counting as missing."""
        start = st.unrec_upto if st.unrec_upto is not None else st.expect_from
        if start is None or st.oldest_seq <= start:
            return
        for a, b in st.tracker.missing(start, st.oldest_seq):
            st.unrecoverable += b - a + 1
            self.store.record_unrecoverable(st.id, a, b, now_ns())
            self.store.event(now_ns(), st.id, "unrecoverable", f"{a}-{b}")
            log.warning("station %d: seq %d-%d evicted before it was received", st.id, a, b)
        st.unrec_upto = st.oldest_seq

    # --- periodic work ----------------------------------------------------

    def _request_gaps(self, st: StationState, mono: float) -> None:
        if st.addr is None or self.transport is None:
            return
        missing = st.missing()
        if not missing:
            st.gap_interval = self.args.gap_interval
            return
        if mono - st.last_gap_request < st.gap_interval:
            return

        # Back off while a request produces nothing (the link is down), so a
        # dead station is asked every gap_max_interval, not every second.
        if st.last_gap_request > 0 and st.tracker.count == st.packets_at_last_request:
            st.gap_interval = min(st.gap_interval * 2, self.args.gap_max_interval)
        else:
            st.gap_interval = self.args.gap_interval
        st.last_gap_request = mono
        st.packets_at_last_request = st.tracker.count
        st.gap_requests += 1

        ranges = tuple(missing[:protocol.GAPS_MAX_RANGES])
        try:
            self.transport.sendto(protocol.encode_gaps(protocol.Gaps(st.id, ranges)), st.addr)
        except OSError as e:
            log.warning("station %d: cannot send GAPS: %s", st.id, e)
            return
        log.debug("station %d: asked for %d ranges, %d seqs missing, next in %.1fs",
                  st.id, len(ranges), st.missing_count(), st.gap_interval)

    def _write_status(self, mono: float) -> None:
        stations = {}
        for st in self.stations.values():
            stations[str(st.id)] = {
                "addr": f"{st.addr[0]}:{st.addr[1]}" if st.addr else None,
                "received": st.tracker.count,
                "next_seq": st.next_seq,
                "oldest_seq": st.oldest_seq,
                "missing": st.missing_count(),
                "missing_ranges": st.missing()[:8],
                "unrecoverable": st.unrecoverable,
                "complete": st.complete(),
                "late": st.late,
                "duplicates": st.duplicates,
                "bad_payload": st.bad_payload,
                "bytes": st.bytes,
                "gap_requests": st.gap_requests,
                "clock_quality": st.clock_quality,
                "uptime_s": st.uptime_s,
                "heartbeat_age_s": None if st.last_heartbeat is None else round(mono - st.last_heartbeat, 1),
                "data_age_s": None if st.last_data is None else round(mono - st.last_data, 1),
            }
        doc = {
            "at": time.time(),
            "uptime_s": round(mono - self.started, 1),
            "datagrams": self.datagrams,
            "bad_datagrams": self.bad,
            "stations": stations,
        }
        tmp = self.args.status + ".tmp"
        with open(tmp, "w") as f:
            json.dump(doc, f, indent=1)
        os.replace(tmp, self.args.status)

    def _link_samples(self, mono: float) -> None:
        for st in self.stations.values():
            p, l, d, b = st.packets, st.late, st.duplicates, st.bytes
            dp, dl, dd, db = (p - st._ls[0], l - st._ls[1], d - st._ls[2], b - st._ls[3])
            st._ls = (p, l, d, b)
            age = None if st.last_heartbeat is None else mono - st.last_heartbeat
            self.store.link_sample(st.id, now_ns(), dp, dl, dd, db, st.missing_count(), age)

    async def run_periodic(self) -> None:
        last_status = 0.0
        last_link = time.monotonic()
        while True:
            await asyncio.sleep(self.args.tick)
            mono = time.monotonic()
            for st in self.stations.values():
                self._request_gaps(st, mono)
            if self.args.status and mono - last_status >= 1.0:
                self._write_status(mono)
                last_status = mono
            if mono - last_link >= self.args.link_sample_every:
                self._link_samples(mono)
                last_link = mono
            self.store.commit()


async def serve(args: argparse.Namespace) -> None:
    store = Store(args.db)
    loop = asyncio.get_running_loop()
    ingest = Ingest(store, args)
    transport, _ = await loop.create_datagram_endpoint(
        lambda: ingest, local_addr=(args.bind, args.port))
    log.info("listening on %s:%d, database %s", args.bind, args.port, args.db)

    stop = asyncio.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)

    periodic = asyncio.create_task(ingest.run_periodic())
    await stop.wait()
    periodic.cancel()
    if args.status:
        ingest._write_status(time.monotonic())
    transport.close()
    store.close()
    log.info("stopped: %d datagrams, %d bad", ingest.datagrams, ingest.bad)


def main(argv: list[str] | None = None) -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--db", default="holdfast.db")
    ap.add_argument("--status", default="", help="JSON status file, rewritten every second")
    ap.add_argument("--gap-interval", type=float, default=2.0,
                    help="seconds between backfill requests while they make progress")
    ap.add_argument("--gap-max-interval", type=float, default=30.0,
                    help="backoff ceiling while requests go unanswered")
    ap.add_argument("--tick", type=float, default=0.25)
    ap.add_argument("--link-sample-every", type=float, default=10.0)
    ap.add_argument("--log-level", default="INFO")
    args = ap.parse_args(argv)

    logging.basicConfig(level=args.log_level.upper(),
                        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s")
    asyncio.run(serve(args))


if __name__ == "__main__":
    main()
