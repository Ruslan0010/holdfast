"""The holdfast wire format, independently of the C implementation.

Normative text: docs/PROTOCOL.md. This module and firmware/src/packet.c are
two implementations of it that share no code; the integration tests run one
against the other. CRC-32 is zlib's, which is the same IEEE variant the
firmware computes with its own table.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass

MAGIC = b"HF"
VERSION = 1
MAX_LEN = 256
CRC_LEN = 4

DATA, HEARTBEAT, GAPS = 1, 2, 3

ENC_RAW, ENC_STEIM2 = 0, 1
CLOCK_FREE, CLOCK_HOLDOVER, CLOCK_LOCKED = 0, 1, 2

# Big-endian throughout. Field order matches docs/PROTOCOL.md exactly.
_DATA_HDR = struct.Struct(">2sBBIHBBQQIHH")  # 36 bytes
_HEARTBEAT = struct.Struct(">2sBBIQQIB3x")  # 32 bytes + CRC
_GAPS_HDR = struct.Struct(">2sBBIB3x")  # 12 bytes
_RANGE = struct.Struct(">QQ")  # 16 bytes

DATA_HDR_LEN = _DATA_HDR.size
DATA_MAX_PAYLOAD = MAX_LEN - DATA_HDR_LEN - CRC_LEN  # 216
HEARTBEAT_LEN = _HEARTBEAT.size + CRC_LEN  # 36
GAPS_HDR_LEN = _GAPS_HDR.size
GAPS_MAX_RANGES = 12

assert DATA_HDR_LEN == 36 and HEARTBEAT_LEN == 36 and GAPS_HDR_LEN == 12


class ProtocolError(ValueError):
    """A datagram that is not a valid holdfast message. `reason` mirrors the
    error names in packet.h so logs on both sides read the same."""

    def __init__(self, reason: str):
        super().__init__(reason)
        self.reason = reason


@dataclass(frozen=True)
class Data:
    station_id: int
    stream_id: int
    clock_quality: int
    encoding: int
    seq: int
    t0_ns: int
    sample_period_us: int
    sample_count: int
    payload: bytes


@dataclass(frozen=True)
class Heartbeat:
    station_id: int
    next_seq: int
    oldest_seq: int
    uptime_s: int
    clock_quality: int


@dataclass(frozen=True)
class Gaps:
    station_id: int
    ranges: tuple[tuple[int, int], ...]  # inclusive (from, to)


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def _seal(body: bytes) -> bytes:
    return body + struct.pack(">I", crc32(body))


def _check_data_fields(d: Data) -> None:
    if d.clock_quality > CLOCK_LOCKED:
        raise ProtocolError("field")
    if d.encoding > ENC_STEIM2:
        raise ProtocolError("field")
    n = len(d.payload)
    if n > DATA_MAX_PAYLOAD:
        raise ProtocolError("field")
    if d.encoding == ENC_RAW and n != d.sample_count * 4:
        raise ProtocolError("field")
    if d.encoding == ENC_STEIM2:
        if n % 64 != 0 or (n == 0) != (d.sample_count == 0):
            raise ProtocolError("field")


def encode_data(d: Data) -> bytes:
    _check_data_fields(d)
    flags = (d.clock_quality & 3) | ((d.encoding & 3) << 2)
    hdr = _DATA_HDR.pack(MAGIC, VERSION, DATA, d.station_id, d.stream_id, flags, 0,
                         d.seq, d.t0_ns, d.sample_period_us, d.sample_count,
                         len(d.payload))
    return _seal(hdr + d.payload)


def encode_heartbeat(h: Heartbeat) -> bytes:
    if h.clock_quality > CLOCK_LOCKED or h.oldest_seq > h.next_seq:
        raise ProtocolError("field")
    return _seal(_HEARTBEAT.pack(MAGIC, VERSION, HEARTBEAT, h.station_id, h.next_seq,
                                 h.oldest_seq, h.uptime_s, h.clock_quality & 3))


def encode_gaps(g: Gaps) -> bytes:
    if len(g.ranges) > GAPS_MAX_RANGES:
        raise ProtocolError("field")
    for lo, hi in g.ranges:
        if lo > hi:
            raise ProtocolError("field")
    body = _GAPS_HDR.pack(MAGIC, VERSION, GAPS, g.station_id, len(g.ranges))
    body += b"".join(_RANGE.pack(lo, hi) for lo, hi in g.ranges)
    return _seal(body)


def _check_crc(buf: bytes) -> None:
    (got,) = struct.unpack(">I", buf[-CRC_LEN:])
    if got != crc32(buf[:-CRC_LEN]):
        raise ProtocolError("crc")


def decode(buf: bytes) -> Data | Heartbeat | Gaps:
    if len(buf) < 4:
        raise ProtocolError("short")
    if buf[:2] != MAGIC:
        raise ProtocolError("magic")
    if buf[2] != VERSION:
        raise ProtocolError("version")
    kind = buf[3]
    if kind == DATA:
        return _decode_data(buf)
    if kind == HEARTBEAT:
        return _decode_heartbeat(buf)
    if kind == GAPS:
        return _decode_gaps(buf)
    raise ProtocolError("type")


def _decode_data(buf: bytes) -> Data:
    if len(buf) < DATA_HDR_LEN + CRC_LEN:
        raise ProtocolError("short")
    (_, _, _, station_id, stream_id, flags, _, seq, t0_ns, period_us, count,
     payload_len) = _DATA_HDR.unpack_from(buf)
    if payload_len > DATA_MAX_PAYLOAD:
        raise ProtocolError("len")
    if len(buf) != DATA_HDR_LEN + payload_len + CRC_LEN:
        raise ProtocolError("len")
    _check_crc(buf)
    d = Data(station_id, stream_id, flags & 3, (flags >> 2) & 3, seq, t0_ns,
             period_us, count, bytes(buf[DATA_HDR_LEN:DATA_HDR_LEN + payload_len]))
    _check_data_fields(d)
    return d


def _decode_heartbeat(buf: bytes) -> Heartbeat:
    if len(buf) < HEARTBEAT_LEN:
        raise ProtocolError("short")
    if len(buf) != HEARTBEAT_LEN:
        raise ProtocolError("len")
    _check_crc(buf)
    _, _, _, station_id, next_seq, oldest_seq, uptime_s, flags = _HEARTBEAT.unpack_from(buf)
    h = Heartbeat(station_id, next_seq, oldest_seq, uptime_s, flags & 3)
    if h.clock_quality > CLOCK_LOCKED or h.oldest_seq > h.next_seq:
        raise ProtocolError("field")
    return h


def _decode_gaps(buf: bytes) -> Gaps:
    if len(buf) < GAPS_HDR_LEN + CRC_LEN:
        raise ProtocolError("short")
    count = buf[8]
    if count > GAPS_MAX_RANGES:
        raise ProtocolError("len")
    if len(buf) != GAPS_HDR_LEN + count * _RANGE.size + CRC_LEN:
        raise ProtocolError("len")
    _check_crc(buf)
    _, _, _, station_id, _ = _GAPS_HDR.unpack_from(buf)
    ranges = []
    for i in range(count):
        lo, hi = _RANGE.unpack_from(buf, GAPS_HDR_LEN + i * _RANGE.size)
        if lo > hi:
            raise ProtocolError("field")
        ranges.append((lo, hi))
    return Gaps(station_id, tuple(ranges))


def decode_samples(d: Data) -> list[int]:
    """Samples carried by a DATA packet, whatever its encoding."""
    if d.encoding == ENC_RAW:
        return list(struct.unpack(f">{d.sample_count}i", d.payload))
    from . import steim2  # local import keeps protocol.py dependency-free
    return steim2.decode(d.payload, d.sample_count)
