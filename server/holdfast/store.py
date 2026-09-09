"""SQLite persistence for received packets and station state.

One file, WAL mode, batched commits from the ingest loop. Samples are stored
as little-endian int32 blobs, one row per packet, keyed by (station, seq):
the primary key makes duplicate delivery a no-op at the database level, not
just in memory, so a server restart cannot double-store a retransmit.
"""

from __future__ import annotations

import sqlite3
import struct
from collections.abc import Iterator

SCHEMA = """
CREATE TABLE IF NOT EXISTS stations (
    station_id    INTEGER PRIMARY KEY,
    first_seen_ns INTEGER NOT NULL,
    last_seen_ns  INTEGER NOT NULL,
    last_addr     TEXT,
    next_seq      INTEGER NOT NULL DEFAULT 0,
    oldest_seq    INTEGER NOT NULL DEFAULT 0,
    clock_quality INTEGER NOT NULL DEFAULT 0,
    uptime_s      INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS packets (
    station_id       INTEGER NOT NULL,
    seq              INTEGER NOT NULL,
    stream_id        INTEGER NOT NULL,
    t0_ns            INTEGER NOT NULL,
    sample_period_us INTEGER NOT NULL,
    sample_count     INTEGER NOT NULL,
    encoding         INTEGER NOT NULL,
    clock_quality    INTEGER NOT NULL,
    received_ns      INTEGER NOT NULL,
    late             INTEGER NOT NULL,   -- 1 if it arrived after a higher seq
    wire_len         INTEGER NOT NULL,
    samples          BLOB NOT NULL,      -- int32 little-endian
    PRIMARY KEY (station_id, seq)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS unrecoverable (
    station_id  INTEGER NOT NULL,
    seq_from    INTEGER NOT NULL,
    seq_to      INTEGER NOT NULL,
    recorded_ns INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS link_samples (
    station_id      INTEGER NOT NULL,
    at_ns           INTEGER NOT NULL,
    packets         INTEGER NOT NULL,
    late            INTEGER NOT NULL,
    duplicates      INTEGER NOT NULL,
    bytes           INTEGER NOT NULL,
    missing         INTEGER NOT NULL,
    heartbeat_age_s REAL
);
CREATE INDEX IF NOT EXISTS link_samples_by_station ON link_samples (station_id, at_ns);
CREATE TABLE IF NOT EXISTS events (
    at_ns      INTEGER NOT NULL,
    station_id INTEGER,
    kind       TEXT NOT NULL,
    detail     TEXT
);
"""


def pack_samples(samples: list[int]) -> bytes:
    return struct.pack(f"<{len(samples)}i", *samples)


def unpack_samples(blob: bytes) -> list[int]:
    return list(struct.unpack(f"<{len(blob) // 4}i", blob))


class Store:
    def __init__(self, path: str):
        self.conn = sqlite3.connect(path, isolation_level=None)
        self.conn.execute("PRAGMA journal_mode=WAL")
        self.conn.execute("PRAGMA synchronous=NORMAL")
        self.conn.executescript(SCHEMA)
        self._in_tx = False

    def begin(self) -> None:
        if not self._in_tx:
            self.conn.execute("BEGIN")
            self._in_tx = True

    def commit(self) -> None:
        if self._in_tx:
            self.conn.execute("COMMIT")
            self._in_tx = False

    def close(self) -> None:
        self.commit()
        self.conn.close()

    # --- stations ---------------------------------------------------------

    def touch_station(self, station_id: int, now_ns: int, addr: str | None) -> None:
        self.begin()
        self.conn.execute(
            "INSERT INTO stations (station_id, first_seen_ns, last_seen_ns, last_addr) "
            "VALUES (?, ?, ?, ?) ON CONFLICT(station_id) DO UPDATE SET "
            "last_seen_ns = excluded.last_seen_ns, "
            "last_addr = COALESCE(excluded.last_addr, stations.last_addr)",
            (station_id, now_ns, now_ns, addr))

    def update_heartbeat(self, station_id: int, next_seq: int, oldest_seq: int,
                         clock_quality: int, uptime_s: int) -> None:
        self.begin()
        self.conn.execute(
            "UPDATE stations SET next_seq = ?, oldest_seq = ?, clock_quality = ?, "
            "uptime_s = ? WHERE station_id = ?",
            (next_seq, oldest_seq, clock_quality, uptime_s, station_id))

    def stations(self) -> list[sqlite3.Row]:
        self.conn.row_factory = sqlite3.Row
        try:
            return self.conn.execute("SELECT * FROM stations ORDER BY station_id").fetchall()
        finally:
            self.conn.row_factory = None

    # --- packets ----------------------------------------------------------

    def insert_packet(self, station_id: int, seq: int, stream_id: int, t0_ns: int,
                      sample_period_us: int, encoding: int, clock_quality: int,
                      received_ns: int, late: bool, wire_len: int,
                      samples: list[int]) -> bool:
        """Returns True if the packet was new."""
        self.begin()
        cur = self.conn.execute(
            "INSERT OR IGNORE INTO packets VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (station_id, seq, stream_id, t0_ns, sample_period_us, len(samples),
             encoding, clock_quality, received_ns, int(late), wire_len,
             pack_samples(samples)))
        return cur.rowcount == 1

    def received_seqs(self, station_id: int) -> Iterator[int]:
        for (seq,) in self.conn.execute(
                "SELECT seq FROM packets WHERE station_id = ? ORDER BY seq", (station_id,)):
            yield seq

    def packets(self, station_id: int) -> Iterator[tuple[int, int, int, list[int]]]:
        """(seq, t0_ns, sample_period_us, samples) in sequence order."""
        for seq, t0, period, blob in self.conn.execute(
                "SELECT seq, t0_ns, sample_period_us, samples FROM packets "
                "WHERE station_id = ? ORDER BY seq", (station_id,)):
            yield seq, t0, period, unpack_samples(blob)

    def packet_stats(self, station_id: int) -> tuple[int, int, int]:
        """(packets, samples, bytes on the wire)."""
        row = self.conn.execute(
            "SELECT COUNT(*), COALESCE(SUM(sample_count), 0), COALESCE(SUM(wire_len), 0) "
            "FROM packets WHERE station_id = ?", (station_id,)).fetchone()
        return int(row[0]), int(row[1]), int(row[2])

    # --- gaps and diagnostics --------------------------------------------

    def record_unrecoverable(self, station_id: int, seq_from: int, seq_to: int,
                             now_ns: int) -> None:
        self.begin()
        self.conn.execute("INSERT INTO unrecoverable VALUES (?, ?, ?, ?)",
                          (station_id, seq_from, seq_to, now_ns))

    def unrecoverable(self, station_id: int) -> list[tuple[int, int]]:
        return [(a, b) for a, b in self.conn.execute(
            "SELECT seq_from, seq_to FROM unrecoverable WHERE station_id = ? "
            "ORDER BY seq_from", (station_id,))]

    def link_sample(self, station_id: int, at_ns: int, packets: int, late: int,
                    duplicates: int, nbytes: int, missing: int,
                    heartbeat_age_s: float | None) -> None:
        self.begin()
        self.conn.execute("INSERT INTO link_samples VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                          (station_id, at_ns, packets, late, duplicates, nbytes,
                           missing, heartbeat_age_s))

    def event(self, at_ns: int, station_id: int | None, kind: str, detail: str = "") -> None:
        self.begin()
        self.conn.execute("INSERT INTO events VALUES (?, ?, ?, ?)",
                          (at_ns, station_id, kind, detail))
