# holdfast

[![firmware](https://github.com/Ruslan0010/holdfast/actions/workflows/ci.yml/badge.svg)](https://github.com/Ruslan0010/holdfast/actions/workflows/ci.yml)

Telemetry for seismic field stations on networks that do not cooperate.

A remote seismic recorder has to ship a continuous sample stream to a central
server over whatever link it can get — satellite, cellular, a modem that
reboots twice a day. The hard constraint is not bandwidth. It is that the
station usually has **no reachable address**: carrier-grade NAT, a dynamic IP,
a firewall that drops everything inbound. The server cannot call the station,
so the station has to call the server, and it has to keep enough history to
fill in whatever was lost while the link was down.

This repo builds that end to end: firmware in C, a retransmit buffer, a push
protocol over UDP, a server, and a test rig that runs the whole thing over a
deliberately broken network.

## The test network is real

The station runs behind a Rogers 5G home modem, which hands out addresses via
carrier-grade NAT. The server is a public Azure VM. This is not a simulated
constraint — inbound connections to the station genuinely cannot be
established, which is exactly the situation a station on a satellite link is
in.

## Quickstart

```bash
git clone https://github.com/Ruslan0010/holdfast.git
cd holdfast
make test
```

Thirteen tests run and twelve fail. That is correct: `firmware/src/ringbuf.c`
is a set of stubs (the one that passes only checks that garbage is rejected,
which stubs do by accident). Your job is to make them all pass.

```bash
make test        # build and run
make test-asan   # same, under AddressSanitizer + UBSan
make clean
```

The build uses `-Werror` with a wide warning set. Warnings you would ignore in
another language are the actual bug in C, so they are errors here.

## Where to start

1. Read `firmware/include/ringbuf.h` — the contract and the reasoning.
2. Read `tests/test_ringbuf.c` — the specification, as executable tests. Each
   test's comment says which real-world failure it represents.
3. Implement `firmware/src/ringbuf.c`. One test at a time.
4. When `make test` and `make test-asan` are both green, push. CI runs it under
   gcc, clang, both sanitizers, and cppcheck.

Do not read a reference implementation first. The ring buffer is about twenty
lines and the whole value is in deriving those twenty lines yourself — the
modulo arithmetic, the eviction condition, the two distinct kinds of read
failure. If you skip that, you will not be able to answer questions about your
own repository.

## Layout

```
firmware/include/   public headers — the contracts
firmware/src/       implementation
tests/              unit tests and a 40-line test harness
server/             ingest server (Milestone 2)
docs/               protocol spec and roadmap
```

## Roadmap

See `docs/ROADMAP.md`. Eleven milestones, from tonight's ring buffer to twenty
simulated stations reporting to a live dashboard, with the firmware running on
a simulated STM32 in CI. Nothing before the last milestone needs a board on
your desk.

## Licence

MIT.
