# holdfast

[![ci](https://github.com/Ruslan0010/holdfast/actions/workflows/ci.yml/badge.svg)](https://github.com/Ruslan0010/holdfast/actions/workflows/ci.yml)

Telemetry for seismic field stations on networks that do not cooperate.

A remote seismic recorder has to ship a continuous sample stream to a
central server over whatever link it can get: satellite, cellular, a modem
that reboots twice a day. Bandwidth is the smaller problem. The larger one
is that the station usually has **no reachable address** (carrier-grade
NAT, a firewall that drops everything inbound), so the server cannot call
the station. The station has to call the server, meter itself to the
link, and keep enough history to fill in whatever was lost while the link
was down.

holdfast is that, end to end: firmware in C with no heap, a 256-byte
datagram protocol with Steim2 compression, a retransmit buffer, an ingest
server that tracks gaps and asks for them back, and a network simulator
that proves the whole path recovers every byte through loss, reordering,
duplication and minute-long blackouts.

## What it does today

- **Station** (`firmware/`): samples in, DATA packets out. Packets are
  Steim2-compressed, CRC-32 checked, numbered, stored in a fixed-slot ring
  buffer before they are sent, and metered through a token bucket. A
  heartbeat every 15 s keeps the NAT mapping alive and tells the server
  what is still recoverable. Backfill requests are served from the buffer
  byte for byte. The sequence counter survives a restart.
- **Server** (`server/`): one UDP port for every station, Python standard
  library only. Tracks received sequence numbers as intervals, requests
  what is missing with exponential backoff, records what has become
  unrecoverable, stores samples in SQLite, rebuilds its state after a
  restart, and exports a station's data with a byte-for-byte check against
  the source.
- **Hostile network** (`tools/netsim.py`): loss, delay, jitter, duplicates,
  scheduled blackouts, and NAT-style addressing, in one process. Five named
  profiles from `starlink` to `dying-modem`.
- **Everything above the HAL is portable C11.** The same core runs on the
  Linux host now and is built to move to Zephyr on an STM32 without
  modification; see the [roadmap](docs/ROADMAP.md).

## Results

`make integration` runs the station against the server through each link
profile at twenty times real time, waits until the server reports nothing
missing, reassembles the station's data from SQLite and compares it with
the source signal. Measured 2026-09-09 on a laptop, 240 s of a synthetic
100 Hz seismogram per run (120 s for the last two):

| run | link | encoding | recovered | packets | resent | dropped by link | bytes/sample | note |
|-----|------|----------|----------:|--------:|-------:|----------------:|-------------:|------|
| clean | 0% loss, 1±0 ms | steim2 | 100.0 % ✔ byte-for-byte | 174 | 0 | 0 | 1.70 |  |
| clean-raw | 0% loss, 1±0 ms | raw | 100.0 % ✔ byte-for-byte | 445 | 0 | 0 | 4.76 | no compression, for comparison |
| starlink | 2% loss, 40±20 ms, 30 s blackout / 600 s | steim2 | 100.0 % ✔ byte-for-byte | 174 | 1 | 1 | 1.71 |  |
| 4g-mountain | 15% loss, 300±100 ms, 60 s blackout / 300 s | steim2 | 100.0 % ✔ byte-for-byte | 174 | 49 | 54 | 2.18 |  |
| dying-modem | 30% loss, 500±300 ms, 60 s blackout / 300 s | steim2 | 100.0 % ✔ byte-for-byte | 174 | 122 | 124 | 2.91 |  |
| dying-modem-ratelimit | 30% loss, 500±300 ms, 60 s blackout / 300 s | steim2 | 100.0 % ✔ byte-for-byte | 174 | 162 | 156 | 3.16 | link capped at 6 kB/s, below the live rate |
| dying-modem-tiny-ring | 30% loss, 500±300 ms, 60 s blackout / 300 s | steim2 | 78.4 % | 94 | 10 | 44 | 2.09 | retention of 8 packets: loss is expected and reported |

- Every profile that keeps at least one copy of the data recovers **100 %,
  byte for byte**, including 30 % loss with three blackouts.
- Steim2 puts a 100 Hz channel on the wire at **1.7 bytes per sample**
  against 4.8 raw, header included. Retransmits raise it; the dying modem
  costs 2.9.
- When data is genuinely gone (a retention window shorter than a blackout,
  or a restart), the server records the exact range as unrecoverable, stops
  asking for it, and everything else still verifies.

## How it works

```
 STATION  (C, static allocation)
 ┌─────────┐   ┌─────────┐   ┌─────────┐   ┌──────────┐   ┌────────────┐
 │ ADC     │──►│ sampler │──►│ Steim2  │──►│ packet   │──►│ ring buffer│──► UDP push
 │         │   │         │   │ compress│   │ + CRC-32 │   │            │◄── backfill
 └─────────┘   └─────────┘   └─────────┘   └──────────┘   └────────────┘    heartbeat
                                                                                 │
                     carrier-grade NAT  /  satellite  /  tools/netsim.py  ◄──────┘
                                                                                 │
 SERVER  (Python)                                                                ▼
 ┌─────────┐   ┌─────────┐   ┌─────────┐   ┌──────────┐
 │ ingest  │──►│ gap     │──►│ SQLite  │──►│ export + │
 │ (UDP)   │   │ tracker │   │         │   │ verify   │
 └─────────┘   └─────────┘   └─────────┘   └──────────┘
```

Three rules shape the station:

1. **The ring buffer is the queue.** A packet is stored before it is sent.
   If the rate limiter says no, nothing else happens: the next heartbeat
   advertises the sequence number, the server sees the hole and asks. Link
   loss, rate limiting and server outages are one mechanism.
2. **Retransmits are byte-identical.** What is stored is the encoded
   datagram; a resend is a copy. Delivery is idempotent on the server side,
   so the station may resend freely.
3. **Heartbeats bypass the rate limiter.** They are the control channel and
   the NAT keepalive; 36 bytes every 15 s is not where the budget goes.

And four rules shape the code: no heap, fixed-size slots, every operation
O(1), and no undefined behaviour on any input. Bad arguments return errors;
they do not corrupt memory.

Details: [docs/DESIGN.md](docs/DESIGN.md), the wire format in
[docs/PROTOCOL.md](docs/PROTOCOL.md), and the reasoning behind the main
choices in [docs/adr/](docs/adr/).

## Quickstart

Needs gcc or clang, make, and Python 3.11+. Nothing to install.

```bash
git clone https://github.com/Ruslan0010/holdfast.git
cd holdfast
make test          # C unit tests
make test-asan     # the same under AddressSanitizer and UBSan
make pytest        # Python unit tests and C↔Python cross-check
make integration   # end to end through every link profile (about 4 minutes)
```

To watch it by hand, in three terminals:

```bash
# 1. the server
PYTHONPATH=server python3 -m holdfast.ingest --port 5000 --db demo.db --status status.json

# 2. a bad link in front of it
python3 tools/netsim.py --listen 127.0.0.1:5001 --upstream 127.0.0.1:5000 \
        --profile dying-modem --time-scale 0.1 --stats-every 5

# 3. a station, ten minutes of signal at 20x, through the bad link
python3 tools/gen_signal.py --seconds 600 --out signal.i32
make station
./build/station --server 127.0.0.1:5001 --signal signal.i32 --speedup 20 \
        --heartbeat 2000 --stats-every 5
```

`status.json` shows what the server has and what it is still asking for.
When it says `"complete": true`:

```bash
PYTHONPATH=server python3 -m holdfast.export --db demo.db --station 1 --verify signal.i32
```

## Testing

| level         | what                                         | how                                                       |
|---------------|----------------------------------------------|-----------------------------------------------------------|
| unit, C       | ring buffer, CRC-32, packets, Steim2, limiter | 71 tests, 40-line harness, gcc and clang, ASan and UBSan  |
| unit, C, core | the station state machine                     | recording mock HAL; every datagram decoded and checked     |
| fuzz          | packet and Steim2 decoders                    | libFuzzer with round-trip properties; corpus replayed under ASan on every build |
| cross-impl    | C encoders vs Python decoders                 | `make vectors`: C emits, Python must decode and re-encode identically |
| unit, Python  | protocol, Steim2, gap tracker, store          | 30 tests, `unittest`, no dependencies                      |
| integration   | station → netsim → server → SQLite → export   | 8 scenarios, byte-for-byte against the source              |
| static        | `-Wall -Wextra -Wconversion -Wshadow … -Werror`, cppcheck | every build                                     |

## Layout

```
firmware/include/       public headers: the contracts
firmware/include/hal/   the HAL: net, time, log, adc
firmware/src/           portable core: ringbuf, crc32, packet, steim2, ratelimit, station
firmware/hal/posix/     HAL for Linux
firmware/app/           host station binary
server/holdfast/        ingest server, protocol, Steim2, gap tracker, store, export
tools/                  signal generator, network simulator, results table
tests/                  C unit tests, mock HAL, fuzz harnesses, Python tests, integration
docs/                   PROTOCOL.md, DESIGN.md, ROADMAP.md, adr/
```

## Roadmap

Next is the same firmware on a simulated STM32 under Zephyr and Renode,
with SPI, UART, I2C, PPS and watchdog drivers and GDB over the emulator;
then flash-backed retention, a multi-station dashboard, and miniSEED and
SeedLink interoperability. See [docs/ROADMAP.md](docs/ROADMAP.md).

## Licence

MIT.
