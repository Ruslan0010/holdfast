"""Wire format: round trips and rejection of every malformed shape."""

import struct
import unittest

from holdfast import protocol as p


def data(n=2, **kw):
    fields = dict(station_id=0xC0FFEE, stream_id=7, clock_quality=p.CLOCK_LOCKED,
                  encoding=p.ENC_RAW, seq=0x0102030405060708, t0_ns=1757400000123456789,
                  sample_period_us=10000, sample_count=n,
                  payload=struct.pack(f">{n}i", *range(n)))
    fields.update(kw)
    return p.Data(**fields)


class TestData(unittest.TestCase):
    def test_round_trip(self):
        d = data(54)
        buf = p.encode_data(d)
        self.assertEqual(len(buf), 256)
        self.assertEqual(p.decode(buf), d)

    def test_layout(self):
        buf = p.encode_data(data(2))
        self.assertEqual(buf[:4], b"HF\x01\x01")
        self.assertEqual(buf[4:8], b"\x00\xc0\xff\xee")
        self.assertEqual(buf[10], 0x02)
        self.assertEqual(buf[12:20], bytes(range(1, 9)))
        self.assertEqual(buf[34:36], b"\x00\x08")
        self.assertEqual(struct.unpack(">I", buf[-4:])[0], p.crc32(buf[:-4]))

    def test_empty_payload(self):
        d = data(0)
        self.assertEqual(p.decode(p.encode_data(d)), d)

    def test_field_validation(self):
        with self.assertRaises(p.ProtocolError):
            p.encode_data(data(2, clock_quality=3))
        with self.assertRaises(p.ProtocolError):
            p.encode_data(data(2, encoding=2))
        with self.assertRaises(p.ProtocolError):
            p.encode_data(data(2, payload=b"1234567"))
        with self.assertRaises(p.ProtocolError):
            p.encode_data(data(55))
        with self.assertRaises(p.ProtocolError):
            p.encode_data(data(3, encoding=p.ENC_STEIM2, payload=bytes(100)))
        with self.assertRaises(p.ProtocolError):
            p.encode_data(data(0, encoding=p.ENC_STEIM2, payload=bytes(64)))


class TestHeartbeatAndGaps(unittest.TestCase):
    def test_heartbeat_round_trip(self):
        h = p.Heartbeat(42, 1000, 900, 86400, p.CLOCK_HOLDOVER)
        buf = p.encode_heartbeat(h)
        self.assertEqual(len(buf), 36)
        self.assertEqual(p.decode(buf), h)

    def test_heartbeat_window_validation(self):
        with self.assertRaises(p.ProtocolError):
            p.encode_heartbeat(p.Heartbeat(1, 5, 6, 0, 0))

    def test_gaps_round_trip(self):
        g = p.Gaps(42, tuple((i * 100, i * 100 + 5) for i in range(12)))
        buf = p.encode_gaps(g)
        self.assertEqual(len(buf), 12 + 12 * 16 + 4)
        self.assertEqual(p.decode(buf), g)
        self.assertEqual(p.decode(p.encode_gaps(p.Gaps(1, ()))), p.Gaps(1, ()))

    def test_gaps_validation(self):
        with self.assertRaises(p.ProtocolError):
            p.encode_gaps(p.Gaps(1, tuple((i, i) for i in range(13))))
        with self.assertRaises(p.ProtocolError):
            p.encode_gaps(p.Gaps(1, ((5, 4),)))


class TestDecodeRejects(unittest.TestCase):
    def reason(self, buf):
        with self.assertRaises(p.ProtocolError) as cm:
            p.decode(buf)
        return cm.exception.reason

    def test_each_error(self):
        buf = bytearray(p.encode_data(data(3)))
        self.assertEqual(self.reason(buf[:3]), "short")
        self.assertEqual(self.reason(buf[:20]), "short")
        self.assertEqual(self.reason(b"XF" + buf[2:]), "magic")
        self.assertEqual(self.reason(buf[:2] + b"\x09" + buf[3:]), "version")
        self.assertEqual(self.reason(buf[:3] + b"\xc8" + buf[4:]), "type")
        self.assertEqual(self.reason(buf + b"\x00"), "len")
        bad_len = bytearray(buf)
        bad_len[34:36] = b"\x00\x64"
        self.assertEqual(self.reason(bad_len), "len")
        bad_crc = bytearray(buf)
        bad_crc[-1] ^= 1
        self.assertEqual(self.reason(bad_crc), "crc")
        bad_field = bytearray(buf)
        bad_field[10] = 0x03
        bad_field[-4:] = struct.pack(">I", p.crc32(bad_field[:-4]))
        self.assertEqual(self.reason(bad_field), "field")

    def test_every_bit_flip_rejected(self):
        buf = bytearray(p.encode_data(data(10)))
        for i in range(len(buf)):
            for bit in range(8):
                buf[i] ^= 1 << bit
                with self.assertRaises(p.ProtocolError):
                    p.decode(bytes(buf))
                buf[i] ^= 1 << bit
        p.decode(bytes(buf))

    def test_every_truncation_rejected(self):
        buf = p.encode_data(data(10))
        for n in range(len(buf)):
            with self.assertRaises(p.ProtocolError):
                p.decode(buf[:n])


if __name__ == "__main__":
    unittest.main()
