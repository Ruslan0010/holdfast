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
- Tests: 66 C unit tests under gcc, clang, ASan and UBSan; 31 Python unit
  tests; cross-implementation vectors; libFuzzer harnesses with a replayable
  corpus; eight end-to-end scenarios through the simulator.
- Fixed: `steim2_decode` rejected the zero-length input that
  `steim2_encode` produces for an empty run, so the two disagreed at the
  empty case. Found by libFuzzer on the first CI run; the crash input is
  committed as a regression seed. The Python implementation had the same
  bug and the same fix.
- Documentation: protocol specification, design, six decision records, and
  a README write-up of the fuzzer finding.
