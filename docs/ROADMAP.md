# Roadmap

Each milestone is a working thing, not a half-finished layer. Tag a release
when one lands (`git tag -a m1 -m "Packet format"`) and write a one-paragraph
GitHub Release note. Anyone reading the repo should be able to see progress.

**Virtual first.** Nothing below needs a board on your desk. The firmware runs
on the host as a POSIX binary, then on Zephyr `native_sim`, then on a
simulated STM32 in Renode, all of it in CI on every push. The real Nucleo is
the last milestone and it is optional: by the time it arrives the code will
have been running "on hardware" for weeks, just hardware that lives in a
container. GDB against Renode is the same workflow as ST-Link against the
board, and Renode's peripheral trace is a logic analyzer you do not have to
buy.

## The system

```
 STATION  (C, static allocation, no OS assumptions)
 ┌─────────┐   ┌─────────┐   ┌─────────┐   ┌──────────┐   ┌────────────┐
 │ ADC/SPI │──►│ sampler │──►│ Steim2  │──►│ packet   │──►│ ring buffer│──► UDP push
 │ (ISR)   │   │ + clock │   │ compress│   │ + CRC-32 │   │ + flash    │◄── backfill
 └─────────┘   └────▲────┘   └─────────┘   └──────────┘   └────────────┘    heartbeat
                    │ 1 Hz PPS (GPIO)                                            │
                                                                                 │
                     carrier-grade NAT  /  satellite  /  tc netem  ◄─────────────┘
                                                                                 │
 SERVER  (Python)                                                                ▼
 ┌─────────┐   ┌─────────┐   ┌─────────┐   ┌──────────┐   ┌────────────┐
 │ ingest  │──►│ gap     │──►│ SQLite/ │──►│ dashboard│   │ miniSEED   │──► ObsPy
 │ (UDP)   │   │ tracker │   │ Postgres│   │ (web)    │   │ + SeedLink │    verifies
 └─────────┘   └─────────┘   └─────────┘   └──────────┘   └────────────┘
```

Layers, bottom up: HAL → drivers → sample pipeline → protocol → server →
independent verification. Everything above the HAL is the same C on the host,
on `native_sim`, in Renode, and on the board.

## M0 — Toolchain (tonight)

- [x] Repo, Makefile, test harness, CI
- [ ] First push, CI badge in README, LICENSE file
- [ ] `make test` green — implement `firmware/src/ringbuf.c`
- [ ] `make test-asan` green
- [ ] Install `cppcheck` and `clang-format` locally so you catch what CI
      catches before you push; add `make format` and a format check job

The point is not the ring buffer. The point is that you now have the loop
every firmware team lives in: write test, watch it fail, make it pass, CI
proves it on someone else's machine.

## M1 — Packet format (week 1)

A fixed 256-byte packet, byte-for-byte defined in `docs/PROTOCOL.md`:
station id, stream id, sequence number, timestamp in nanoseconds, clock
quality flag, sample count, payload, CRC-32.

- `firmware/src/crc32.c` — table-driven, IEEE polynomial, test vectors from
  the standard ("123456789" → `0xCBF43926`)
- `firmware/src/packet.c` — `pkt_encode` / `pkt_decode`, explicit
  serialisation byte by byte, big-endian on the wire, never `memcpy` a struct
- Tests: round trip; a corpus of deliberately corrupted packets — every header
  field flipped, truncated at every length, CRC off by one bit
- Fuzzing: `tests/fuzz_packet.c` with libFuzzer
  (`clang -fsanitize=fuzzer,address,undefined`), 60 seconds in CI, seed
  corpus committed

Skills this teaches, all of which come up in interviews: struct packing and
why `sizeof(struct)` is not the sum of its fields; endianness; CRC; what a
fuzzer finds that your tests did not.

## M2 — Push over UDP, and a HAL (week 2)

- `firmware/include/hal/` — `hal_net.h`, `hal_time.h`, `hal_adc.h`,
  `hal_log.h`. Small, C, no vendor types.
- `firmware/hal/posix/` — sockets, `clock_gettime`, a file-backed ADC that
  replays raw `int32` samples, `fprintf` logging
- `station` binary: sample → packetise → ring buffer → UDP push. Source
  signal: a real seismogram pulled from an FDSN data centre with ObsPy and
  saved as raw samples (`tools/fetch_trace.py`)
- `server/ingest.py`: asyncio UDP server, CRC check, per-station sequence
  tracking, gap log to stdout and to JSONL

The HAL boundary is the whole reason M6 is a port and not a rewrite. Draw it
before you write it.

Before writing any protocol code, prove the problem exists: from the Azure
VM, try to open a TCP connection *in* to your home machine. It will fail.
Screenshot that. It is the justification for the entire architecture and it
belongs in the README.

## M3 — Backfill, heartbeat, rate limit (week 3)

- Protocol: `HEARTBEAT` station→server every N seconds (keeps the NAT mapping
  alive, carries `next_seq` and `oldest_seq`); `GAPS` server→station with a
  list of `[from, to]` ranges; station answers from the ring buffer;
  `RB_ERR_EVICTED` becomes a `GAP_UNRECOVERABLE` report so the server stops
  asking
- Server: gap tracker as an interval set, exponential backoff on repeated
  requests
- Station: token-bucket rate limiter in bytes per second. A satellite modem
  is 2.4 kbit/s; the station must never exceed the link
- Swap the hand-rolled harness for Unity. You will want fixtures.

Skills: sliding windows, idempotent retransmit, keepalives, backoff.

## M4 — Hostile network (week 4)

Docker Compose, station and server in separate containers, `tc netem` in a
third "link" container between them. Named profiles:

| profile        | loss | delay / jitter | reorder | blackout            |
|----------------|------|----------------|---------|---------------------|
| `starlink`     | 2 %  | 40 ms / 20 ms  | 1 %     | 30 s every 10 min   |
| `4g-mountain`  | 15 % | 300 ms / 100 ms| 5 %     | 60 s every 5 min    |
| `dying-modem`  | 30 % | 500 ms / 300 ms| 25 %    | 60 s, reset 5 min   |

The acceptance test that matters (`tests/integration/test_profiles.py`):
run 10 minutes of signal through a profile, then assert the server's output
matches the source **byte for byte**. A 60-second variant runs in CI, the
10-minute variant nightly. If it does not match, the backfill logic is wrong
and you have just found the bug the same way a real firmware team does.

README gets a table: profile → data recovered → bytes on the wire → overhead
ratio. And one paragraph on the first bug this test caught.

**Apply for jobs from here.** From M4 the repo demonstrates "bandwidth-limited
telemetry for edge environments" on its own. Do not wait for M9.

## M5 — Compression (week 5)

Steim2 is the compression miniSEED uses; every seismic network speaks it.
Encode in C on the station, decode in Python on the server and in C for
round-trip tests. Test vectors generated with ObsPy so an independent
implementation checks yours. Re-run the M4 table with compression on; that is
the bandwidth number for the README headline.

Skills: bit packing, delta coding, working from a spec (SEED manual,
Appendix B).

## M6 — Onto a simulated MCU (weeks 6–7)

Same C, unchanged above the HAL.

1. **Zephyr `native_sim`.** `firmware/hal/zephyr/` using Zephyr sockets,
   `k_uptime_ticks`, and an emulated SPI ADC through Zephyr's `emul`
   subsystem. Firmware becomes threads: sampler (high priority, fed by the
   ADC interrupt through a message queue), uplink, backfill, watchdog feed.
2. **Renode on STM32.** Pick the Renode platform that matches the Nucleo you
   ordered; if there is no exact model, `stm32f4_discovery` is the same
   family and the port later is a device-tree change. Peripherals to drive:
   - **USART** — console and log; watch it in Renode's UART analyzer
   - **SPI** — ADC driver (ADS1256-class: 24-bit, DRDY line, command set).
     Model the chip as a Renode Python peripheral that replays the seismogram
   - **GPIO / EXTI** — GPS PPS as a 1 Hz interrupt driving clock discipline
   - **I2C** — an RTC or temperature sensor Renode already models. One I2C
     transaction you wrote and can explain on a whiteboard is the goal
   - **IWDG** — independent watchdog, with a test that hangs the main loop
     and proves the reset
   - **Hard fault handler** that dumps the stacked registers over USART, and
     a test that provokes one (null function pointer)
3. **GDB.** `machine StartGdbServer 3333`, `arm-none-eabi-gdb`, break in the
   ISR, single-step the SPI transaction, walk a hard fault back to the line.
   This is JTAG debugging without JTAG; the workflow is identical to ST-Link
   on the real board.
4. **Renode Robot Framework tests in CI** via `antmicro/renode-test-action`:
   boot, sample, push, cut the link, restore it, assert backfill completes.
   On every push, with no board anywhere.

Skills: RTOS (threads, ISR-to-thread handoff, queues, priorities, priority
inversion), SPI/I2C/UART, interrupts, watchdog, fault handling, GDB, memory
map, linker script, what `volatile` is for.

## M7 — Retention and time (week 8)

- Flash-backed ring buffer behind the same `rb_*` API: page erase, write-once
  slots, erase-count accounting. Host: file-backed with simulated erase
  counts. Renode: the STM32's own flash model
- Power-fail safety: `kill -9` the host station mid-write, restart, prove the
  journal recovers with no corruption and no double-send. In Renode: pause
  and reset mid-write
- Time: PPS-disciplined clock with a drift model; per-packet clock quality
  flag (locked / holdover N s / free-running); server stores it, dashboard
  shows it. A seismic sample with a wrong timestamp is worse than no sample

Skills: flash semantics, journaling, crash consistency, timekeeping.

## M8 — Server persistence and dashboard (weeks 9–10)

- SQLite first. Schema: `stations`, `streams`, `packets` (indexed on
  `station, seq`), `gaps`, `link_samples`. Move to Postgres or TimescaleDB
  when twenty stations make SQLite lock; write down when that happened
- Dashboard on Azure with a public URL: waveforms (last five minutes), per
  station RTT / loss / heartbeat age, backfill progress, clock quality.
  Updates pushed over WebSocket
- Twenty station processes with mixed netem profiles, 24 hours, screenshot

This is the part you are already good at. Make it genuinely nice.

## M9 — Prove it is correct (week 11)

- Export to miniSEED (`pymseed` or libmseed bindings), read it back with
  ObsPy, compare sample by sample against the FDSN source trace
- Run EarthScope's `ringserver`, feed it, connect ObsPy's SeedLink client.
  Now any seismologist's tooling can subscribe to your station

Independent verification by an established library beats any claim you make
about your own code.

## M10 — Real hardware (when the Nucleo arrives)

- `west flash` the M6 binary with the on-board ST-Link; same Zephyr board
  target, zero code changes above the device tree
- USART over the board's virtual COM port
- A cheap 8-channel logic analyzer and PulseView on the SPI bus: screenshot of
  one ADC transaction, decoded bytes shown next to the Renode trace of the
  same transaction. That one picture answers the "oscilloscopes and logic
  analyzers" question better than any paragraph
- Optional: a potentiometer on the on-board ADC to show a real signal end to
  end. A real geophone is a later luxury

## Cross-cutting, from day one

- **Commits**: small, one change each, imperative subject, body says *why*.
  The history will be read by whoever evaluates the repo
- **Tags and Releases** per milestone, `CHANGELOG.md`
- **`docs/adr/`**: one-page decision records — why UDP not TCP, why 256
  bytes, why push not pull, why Steim2, why a HAL
- **`docs/DESIGN.md`**: the diagram above plus state machines for the uplink
  and backfill
- **CI gates**: gcc and clang, ASan/UBSan, cppcheck, clang-tidy, format
  check, gcov/lcov with a coverage floor on `firmware/`
- **Rules that never relax**: static allocation only, `-Wconversion` stays on,
  fixed-width integer types, every pointer checked before use

## Coverage map

| Posting line                                   | Where in this repo                          |
|------------------------------------------------|---------------------------------------------|
| Firmware for embedded systems                  | M0, M6, M7, M10                             |
| C/C++                                          | all of `firmware/`                          |
| TCP/IP, bandwidth-limited telemetry for edge   | M2–M5                                       |
| Python, Bash                                   | `server/`, `tools/`, test rig               |
| Git and associated methodologies               | commit history, tags, releases, ADRs        |
| Unit tests                                     | M0 onward, Unity from M3, coverage gate     |
| Software to automate firmware testing          | M4 acceptance rig, M6 Renode tests in CI    |
| Microcontrollers, RTOS                         | M6 (Zephyr on STM32), M10                   |
| Oscilloscopes, logic analyzers, JTAG           | M6 GDB over Renode; M10 PulseView, ST-Link  |
| I2C, SPI, UART                                 | M6 drivers                                  |
| Database programming                           | M8                                          |
| GUI design                                     | M8 dashboard                                |
| Virtualization (Docker, …)                     | M4 Compose, M6 Renode                       |
| Documentation of designs, processes, tests     | `PROTOCOL.md`, `DESIGN.md`, ADRs, README    |

## What goes in the README when you are done

- The CGNAT screenshot and why push beats pull on this network
- A table: loss profile → data recovered → bytes on the wire, with and
  without compression
- One paragraph on a bug you found and how the test caught it
- The Renode trace next to the logic analyzer capture

Before you send the link anywhere, rewrite the README as a product, not an
exercise. Today it says "your job is to make them pass". An employer should
read what the thing is, what it survives, and how you proved it.

That bug paragraph is what an interviewer will actually ask about.
