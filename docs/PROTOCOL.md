# holdfast wire protocol, version 1

This is the normative description. Two implementations exist and share no
code: `firmware/src/packet.c` (C) and `server/holdfast/protocol.py` (Python).
`make vectors` has the C side emit datagrams and `tests/python/test_cross.py`
checks that Python decodes them to the same fields and encodes the same
bytes.

## Transport

- UDP. One datagram is exactly one message; a datagram with trailing bytes
  is invalid.
- Maximum datagram size 256 bytes. This is also the retention slot size.
- The station sends to the server's fixed address. The server replies to
  the source address of the most recent datagram from that station, which
  is the mapping the station's NAT created. The server never initiates.
- All multi-byte integers are big-endian. Fields are serialised byte by
  byte; no struct layout is ever put on the wire.
- Every message ends in a CRC-32 over all bytes preceding it: IEEE 802.3,
  reflected polynomial `0xEDB88320`, initial value `0xFFFFFFFF`, final XOR
  `0xFFFFFFFF`. This is `zlib.crc32` and the check value of `"123456789"` is
  `0xCBF43926`. A receiver discards any datagram whose CRC does not match.

## Common prefix

| offset | size | field   | value                              |
|-------:|-----:|---------|------------------------------------|
| 0      | 2    | magic   | `0x48 0x46` ("HF")                 |
| 2      | 1    | version | 1                                  |
| 3      | 1    | type    | 1 DATA, 2 HEARTBEAT, 3 GAPS        |

A receiver checks magic, then version, then type, then the lengths, and only
then the CRC, so junk is rejected at the lowest possible cost.

## DATA (station to server)

| offset | size | field            | notes                                        |
|-------:|-----:|------------------|----------------------------------------------|
| 0      | 4    | common prefix    | type 1                                       |
| 4      | 4    | station_id       |                                              |
| 8      | 2    | stream_id        | channel within the station                   |
| 10     | 1    | flags            | bits 0-1 clock quality, bits 2-3 encoding    |
| 11     | 1    | reserved         | sent as 0, ignored on receipt                |
| 12     | 8    | seq              | sequence number, see below                   |
| 20     | 8    | t0_ns            | time of the first sample, ns since Unix epoch|
| 28     | 4    | sample_period_us | 10000 for 100 Hz                             |
| 32     | 2    | sample_count     | samples in this packet                       |
| 34     | 2    | payload_len      | 0..216                                       |
| 36     | n    | payload          |                                              |
| 36+n   | 4    | crc32            |                                              |

Clock quality: 0 free-running, 1 holdover (was disciplined, reference
lost), 2 locked. Values 3 is invalid.

Encoding 0, raw: `sample_count` big-endian two's-complement int32 values;
`payload_len` must equal `4 * sample_count`, so at most 54 samples.

Encoding 1, Steim2: `payload_len` is a multiple of 64 (one to three
frames); `payload_len` and `sample_count` are both zero or both non-zero.
The frame format is the one used by miniSEED (SEED manual, Appendix B) with
one convention fixed: the first difference of a packet is stored as 0, so a
packet decodes with no reference to the previous packet. After
reconstruction the last sample must equal the frame's XN word; a mismatch
means the payload is corrupt even though its CRC matched, and the packet is
rejected.

Encodings 2 and 3 are invalid.

The time of sample *i* is `t0_ns + i * sample_period_us * 1000`.

## HEARTBEAT (station to server)

| offset | size | field         | notes                                          |
|-------:|-----:|---------------|------------------------------------------------|
| 0      | 4    | common prefix | type 2                                         |
| 4      | 4    | station_id    |                                                |
| 8      | 8    | next_seq      | the next DATA packet will carry this seq       |
| 16     | 8    | oldest_seq    | lowest seq still retained; below it is gone    |
| 24     | 4    | uptime_s      | seconds since the station started              |
| 28     | 1    | flags         | bits 0-1 clock quality                         |
| 29     | 3    | reserved      | sent as 0                                      |
| 32     | 4    | crc32         |                                                |

Always 36 bytes. `oldest_seq <= next_seq`; `next_seq - oldest_seq` is the
number of packets the station can still resend. A station sends one at
start, one every `heartbeat_interval` (15 s by default; UDP NAT mappings
commonly expire after 30 s), and one immediately whenever it discovers that
the server asked for a packet that has been evicted.

## GAPS (server to station)

| offset | size | field         | notes                                          |
|-------:|-----:|---------------|------------------------------------------------|
| 0      | 4    | common prefix | type 3                                         |
| 4      | 4    | station_id    | must match; the station ignores others         |
| 8      | 1    | count         | 0..12                                          |
| 9      | 3    | reserved      | sent as 0                                      |
| 12     | 16·k | ranges        | k pairs of (from, to), 8 bytes each, inclusive |
| 12+16k | 4    | crc32         |                                                |

At most 208 bytes. `from <= to` in every range. A GAPS message replaces any
earlier one: the station serves the newest request only.

## Sequence numbers and retention

Each station numbers its DATA packets from 0, consecutively, per boot. The
station retains the last *N* packets (the ring buffer, 256 bytes per slot)
and can resend any of them, byte for byte, on request. Everything with
`seq < oldest_seq` is unrecoverable.

The server keeps, per station, the set of sequence numbers received. From
the latest heartbeat it knows `[oldest_seq, next_seq)`; every number in
that window it has not received is *missing and recoverable*. Every number
below `oldest_seq` it has not received is *unrecoverable* and is recorded
once as such.

## Backfill procedure

1. The server sends GAPS with up to 12 missing ranges, lowest first, no
   more often than `gap_interval` (2 s) while requests produce new packets.
   When a request produces nothing (the link is down) the interval doubles
   up to `gap_max_interval` (30 s).
2. The station walks the ranges in order and resends each retained packet
   as its rate limiter allows, at most 8 per tick so sampling is never
   starved. A retransmit is the stored datagram, unchanged.
3. A requested packet that is already evicted is skipped and the station
   sends a heartbeat at once, so the server learns the new `oldest_seq`
   and stops asking.
4. A requested packet that does not exist yet (`seq >= next_seq`) ends
   that range; the server's `next_seq` was stale.
5. The server treats every DATA packet the same way whether it is live or
   a retransmit: unknown `seq` is stored, known `seq` is counted as a
   duplicate and dropped. Delivery is therefore idempotent and the station
   may resend freely.

## Rate limiting

Everything the station sends except heartbeats passes through one token
bucket in bytes per second. A live packet that cannot be sent is not
queued anywhere special: it sits in the ring buffer like any other packet
and the server will ask for it once a heartbeat reveals the hole. The
retention buffer is the only queue.

## Sizes

256 bytes fits any link without fragmentation (IPv6 guarantees 1280; some
satellite modems cap at 512). Three Steim2 frames carry 100 to 300 samples
of a 100 Hz channel, so one packet is one to three seconds of data and a
lost packet costs at most that. The 36-byte header is 12 % of a full
packet; raw int32 payload would make the wire cost 4.7 bytes per sample,
Steim2 makes it about 1 to 1.5 on real seismic data.

## Versioning

`version` is bumped for any incompatible change to a layout above. A
receiver that does not speak the version drops the datagram. New message
types may be added within a version; receivers drop unknown types.
