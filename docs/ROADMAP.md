# Roadmap

Each milestone is a working thing, not a half-finished layer. Tag a release
when one lands. Anyone reading the repo should be able to see progress.

## M0 — Toolchain (tonight, ~2 hours)

- [x] Repo, Makefile, test harness, CI
- [ ] `make test` green — implement `firmware/src/ringbuf.c`
- [ ] `make test-asan` green
- [ ] CI green on GitHub, badge in README

The point is not the ring buffer. The point is that you now have the loop
every firmware team lives in: write test, watch it fail, make it pass, CI
proves it on someone else's machine.

## M1 — Packet format (week 1)

A fixed 256-byte packet, byte-for-byte defined in `docs/PROTOCOL.md`:
station id, stream id, sequence number, timestamp in nanoseconds, sample
count, payload, CRC-32.

Skills this teaches, all of which come up in interviews:
- struct packing and why `sizeof(struct)` is not the sum of its fields
- explicit serialisation instead of `memcpy`-ing a struct onto the wire
- endianness — the wire is big-endian, your x86 is little-endian
- CRC-32 over the header and payload

Test it with round-trip tests and a corpus of deliberately corrupted packets.

## M2 — Push over UDP (week 2)

Station opens an outbound UDP socket, sends packets, keeps them in the ring
buffer. Python server on Azure receives, checks CRC, logs gaps.

First end-to-end run: your Ubuntu box behind Rogers CGNAT, to your Azure VM.
Before writing any code, prove the problem exists: from Azure, try to open a
connection *in* to your home machine. It will fail. Screenshot that. It is the
justification for the entire architecture and it belongs in the README.

## M3 — Backfill (week 3)

Server tracks which sequence numbers it holds, periodically sends the station
a list of gaps, station answers from the ring buffer. Add a heartbeat so the
NAT mapping stays open.

Swap the hand-rolled test harness for Unity here. You will want fixtures.

## M4 — Hostile network (week 4)

Docker Compose, station and server in separate containers, `tc netem` between
them. Named profiles: `starlink`, `4g-mountain`, `dying-modem`.

The acceptance test that matters: run 10 minutes of signal through a profile
with 30% loss and a 60-second blackout, then assert the server's output
matches the source **byte for byte**. If it does not, the backfill logic is
wrong and you have just found the bug the same way a real firmware team does.

## M5 — Onto a simulated MCU (weeks 5–6)

Take the same C, unchanged, and run it on a simulated STM32 in Renode or as a
Zephyr `native_sim` binary. Wire the Renode ADC model to a real seismogram
downloaded from an FDSN data centre.

Add `antmicro/renode-test-action` to CI. Now your firmware is tested on
simulated hardware on every push, with no board on your desk.

## M6 — Dashboard and 20 stations (weeks 7–8)

Twenty station processes, one server, live web dashboard on Azure with a
public URL. Waveforms, per-station link quality, backfill progress, clock
quality. This is the part you are already good at — make it genuinely nice.

## M7 — Prove it is correct (week 8)

Export to miniSEED, read it back with ObsPy, compare against the source
signal. Independent verification by an established library beats any claim you
make about your own code.

---

## What goes in the README when you are done

- The CGNAT screenshot and why push beats pull on this network
- A table: loss profile → data recovered → bytes on the wire
- One paragraph on a bug you found and how the test caught it

That last one is what an interviewer will actually ask about.
