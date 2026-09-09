# Roadmap

Each milestone is a working thing, tagged as a release. Everything is
proven on virtual hardware first: the host build and the network simulator
today, Zephyr `native_sim` and Renode next, a real board last.

## Done — 0.1.0, host

- [x] Ring buffer, CRC-32, wire format, Steim2, rate limiter, station core
- [x] HAL boundary; POSIX implementation; host station binary
- [x] Sequence counter persisted across restarts
- [x] Ingest server: gap tracking, backfill with backoff, SQLite, status,
      export and byte-for-byte verification
- [x] Network simulator with five link profiles and NAT behaviour
- [x] Unit tests (C, Python), cross-implementation vectors, fuzzing,
      end-to-end suite; CI on gcc and clang with sanitizers
- [x] Protocol specification, design document, decision records

## M6 — Simulated microcontroller

Same C above the HAL, unchanged.

1. Zephyr `native_sim`: `firmware/hal/zephyr/` on Zephyr sockets and
   `k_uptime`; the core in its own thread fed by a message queue; an
   emulated SPI ADC through Zephyr's `emul` subsystem.
2. Renode with an STM32F4 platform. Drivers: USART console, SPI ADC
   (24-bit, DRDY interrupt, modelled as a Renode Python peripheral that
   replays a seismogram), GPIO/EXTI for a 1 Hz PPS, one I2C device,
   independent watchdog with a test that hangs the loop and proves the
   reset, a hard-fault handler that dumps the stacked registers.
3. GDB against Renode's GDB stub: breakpoints in the ISR, single-stepping
   the SPI transaction, walking a hard fault back to the line.
4. Renode Robot Framework tests in CI: boot, sample, push, cut the link,
   restore it, assert backfill completes.

## M7 — Retention in flash, time from PPS

- Flash-backed ring buffer behind the same `rb_*` interface: page erase,
  write-once slots, erase counting. File-backed on the host, the STM32
  flash model in Renode.
- Power-fail safety: kill the process mid-write, restart, prove the journal
  recovers with no corruption and no double send.
- PPS-disciplined clock with a drift model; clock quality set from the
  discipline state instead of `adjtimex`.

## M8 — Many stations, persistent server, dashboard

- Postgres or TimescaleDB behind the same store interface once SQLite
  starts to lock under twenty stations; record when that happened.
- Web dashboard: waveforms, per-station loss and heartbeat age, backfill
  progress, clock quality; updates over WebSocket.
- Twenty station processes with mixed link profiles, 24 hours, on a public
  server.

## M9 — Interoperability

- miniSEED export, read back with ObsPy and compared against the source
  sample by sample.
- SeedLink feed via `ringserver`, so standard seismology clients can
  subscribe to a station directly.
- Source signals pulled from an FDSN data centre instead of synthesised.

## M10 — Real hardware

When a Nucleo board is available: `west flash` the M6 binary, console over
the on-board USB serial, a logic analyzer capture of one SPI transaction
placed next to the Renode trace of the same transaction. No code above the
device tree changes.

## Always

- Small commits, tags per milestone, `CHANGELOG.md`.
- A decision record for every choice that shapes the system (`docs/adr/`).
- Static allocation only; `-Wconversion` and `-Werror` stay on; every
  pointer checked; every decoder fuzzed.
