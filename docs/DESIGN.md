# Design

## The problem

A seismic station in the field produces a continuous sample stream and has
to deliver all of it to a server over a link that is slow, lossy, and
periodically absent. The station has no reachable address (carrier-grade
NAT, a firewall, a satellite terminal that only does outbound), so the
server cannot connect to it; the station must push, and must keep enough
history to fill in whatever the link lost while it was down.

## The system

```
 STATION  (C, static allocation, no OS assumptions)
 ┌─────────┐   ┌─────────┐   ┌─────────┐   ┌──────────┐   ┌────────────┐
 │ ADC     │──►│ sampler │──►│ Steim2  │──►│ packet   │──►│ ring buffer│──► UDP push
 │         │   │         │   │ compress│   │ + CRC-32 │   │            │◄── backfill
 └─────────┘   └─────────┘   └─────────┘   └──────────┘   └────────────┘    heartbeat
                                                                                 │
                     carrier-grade NAT  /  satellite  /  tools/netsim.py  ◄──────┘
                                                                                 │
 SERVER  (Python, stdlib only)                                                   ▼
 ┌─────────┐   ┌─────────┐   ┌─────────┐   ┌──────────┐
 │ ingest  │──►│ gap     │──►│ SQLite  │──►│ export + │
 │ (UDP)   │   │ tracker │   │         │   │ verify   │
 └─────────┘   └─────────┘   └─────────┘   └──────────┘
```

## Station

### Layers

| layer     | files                          | depends on                    |
|-----------|--------------------------------|-------------------------------|
| app       | `firmware/app/main_posix.c`    | everything below              |
| core      | `firmware/src/station.c`       | ringbuf, packet, steim2, ratelimit, `hal/*.h` |
| codecs    | `packet.c`, `steim2.c`, `crc32.c`, `wire.h` | nothing           |
| storage   | `ringbuf.c`, `ratelimit.c`     | nothing                       |
| HAL       | `firmware/include/hal/*.h`     | implemented per target        |

Everything above the HAL is C11 with no OS, socket, clock or allocator. It
compiles for the host, for Zephyr, and for bare metal without change. The
HAL is four small headers (`hal_net`, `hal_time`, `hal_log`, `hal_adc`) with
one implementation per target: `firmware/hal/posix/` today, `tests/mock/`
for unit tests, a Zephyr implementation next.

The core does not call `hal_time` at all: the application passes `now_ns`
into every call. That is what makes the state machine testable with a
scripted clock, and it removes one dependency from the port.

### The station loop

```
station_init(cfg, slots, lens, n_slots, now)
loop:
    samples ready?   → station_feed(samples, n, t_first, now)
                          pending += samples
                          while pending >= samples_per_packet:
                              encode payload (raw or Steim2)
                              seq = rb_next_seq()
                              encode DATA(seq, t0, payload) into txbuf
                              rb_push(txbuf)             # stored before sent
                              if rate limiter allows: hal_net_send(txbuf)
    datagram in?     → station_on_rx(buf, len, now)
                          GAPS for us? replace the range list
    always           → station_tick(now)
                          serve up to 8 backfill sends from the ring buffer
                          heartbeat if due or forced
```

Three rules give the design its shape:

1. **The ring buffer is the queue.** A packet is stored before it is sent.
   If the rate limiter refuses to send it now, nothing else happens: the
   next heartbeat advertises `next_seq`, the server sees the hole, and asks.
   Link loss, rate limiting, and a server outage are all handled by the
   same mechanism.
2. **Retransmits are byte-identical.** The stored bytes are the encoded
   datagram. A resend is a copy, not a re-encode, so nothing can differ
   from the first attempt and the server's idempotency is trivially safe.
3. **Heartbeats bypass the rate limiter.** 36 bytes every 15 s is the
   control channel and the NAT keepalive; starving it to save 2.4 bytes per
   second would take the station off the air.

### Memory

Everything is static. For the host build with 1024 slots:

| item                     | bytes     |
|--------------------------|-----------|
| ring buffer slots        | 262 144   |
| slot lengths             | 2 048     |
| `station_t` (pending samples, gap list, tx buffer, stats) | ≈ 4 700 |
| CRC table (flash)        | 1 024     |

On an STM32F4 with 192 KiB of RAM, 256 slots (64 KiB, about 8 minutes of
100 Hz data at three seconds a packet) leaves room for the RTOS. The flash-
backed buffer in the roadmap extends retention to hours without changing
the `rb_*` interface.

### Timing

`station_tick` is called from the application loop every 5 ms on the host
(the socket poll timeout). Backfill is capped at 8 sends per tick, so a
request for a thousand packets is served over 125 ticks while the ADC keeps
being read. On an RTOS the same core runs in a dedicated thread fed by a
message queue from the sampler interrupt.

### Timestamps and clock quality

Each packet carries the wall time of its first sample and the sample
period; the receiver derives the rest. The station stamps every packet
with its current clock quality (free / holdover / locked), which on the
host comes from `adjtimex` and on the MCU will come from the PPS
discipline. A sample with a doubtful timestamp is worse than no sample to a
seismologist, so the quality travels with the data rather than being
inferred later.

## Server

`server/holdfast/`, Python 3.12 standard library only.

- `protocol.py` decodes and validates datagrams, independently of the C.
- `gaps.py` keeps received sequence numbers as sorted disjoint intervals.
  Memory is proportional to the number of holes, not packets.
- `store.py` is SQLite in WAL mode. Packets are keyed by `(station, seq)`,
  so a duplicate delivery is a no-op in the database as well as in memory,
  and a server restart rebuilds its in-memory state from the tables.
- `ingest.py` is one asyncio UDP endpoint for all stations: decode, store,
  learn the retention window from heartbeats, ask for what is missing with
  exponential backoff while the link is down, record what has become
  unrecoverable, write a status file every second.
- `export.py` reassembles a station's samples in sequence order, reports
  every discontinuity, and compares against the source signal byte for
  byte.

## Failure modes

| event                            | what happens                                                       |
|----------------------------------|--------------------------------------------------------------------|
| datagram lost                    | server sees the hole at the next heartbeat or next packet, asks; station resends |
| datagram duplicated              | server counts a duplicate, database ignores it                     |
| datagrams reordered              | server stores them in any order; export orders by seq              |
| datagram corrupted               | CRC fails, dropped; Steim2 XN check catches a payload the CRC missed |
| link down for minutes            | packets accumulate in the ring buffer; backfill drains them at the rate limit when it returns |
| link down longer than retention  | oldest packets evicted; station reports the new `oldest_seq`; server records the range as unrecoverable and stops asking |
| link slower than the data rate   | live packets deferred, buffer fills, eviction as above: the newest data always wins |
| server restarts                  | state rebuilt from SQLite, including the station's last address; backfill resumes |
| server asks for a future seq     | station ends that range; harmless                                  |
| foreign or junk datagram         | dropped at magic / CRC / station id, counted                       |
| station restarts                 | the sequence counter is persisted (`--seq-file` on the host, a flash journal on the MCU); numbering resumes past the saved value plus a margin, and the margin is reported as unrecoverable rather than reused |

## Testing

| level         | what                                  | how                                                   |
|---------------|---------------------------------------|-------------------------------------------------------|
| unit, C       | ring buffer, CRC, packet, Steim2, rate limiter | `tests/test_*.c`, 40-line harness, gcc + clang, ASan + UBSan |
| unit, C, core | station state machine                 | recording mock HAL; every datagram decoded and checked |
| fuzz          | packet and Steim2 decoders            | libFuzzer under clang, round-trip properties; corpus replayed under gcc/ASan |
| cross-impl    | C encoders vs Python decoders         | `make vectors`, `tests/python/test_cross.py`           |
| unit, Python  | protocol, Steim2, gap tracker, store  | `unittest`, no dependencies                            |
| integration   | station → netsim → server → SQLite → export | `tests/integration/`, byte-for-byte against the source through each link profile |
| static        | cppcheck, `-Wall -Wextra -Wconversion ... -Werror` | every build                                    |
