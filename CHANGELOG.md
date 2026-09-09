# Changelog

## 0.1.0 — 2026-09-09

First working system, host only.

- Firmware core in C11 with no allocation: sequence-indexed ring buffer,
  wire format with CRC-32, Steim2 compression, token-bucket rate limiter,
  station state machine (packetise, push, heartbeat, backfill).
- HAL boundary with a POSIX implementation and a recording mock; host
  station binary that replays a signal file at any speed.
- Sequence counter persisted across station restarts; the restart gap is
  reported as unrecoverable rather than reused.
- Ingest server in Python (standard library only): gap tracking as
  intervals, backfill requests with backoff, SQLite storage, restart
  recovery, status file, export with byte-for-byte verification.
- Network simulator with loss, delay, jitter, duplication, blackouts and
  NAT-like addressing; five named link profiles.
- Tests: 71 C unit tests under gcc, clang, ASan and UBSan; 30 Python unit
  tests; cross-implementation vectors; libFuzzer harnesses with a replayable
  corpus; eight end-to-end scenarios through the simulator.
- Documentation: protocol specification, design, six decision records.
