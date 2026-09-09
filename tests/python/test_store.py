"""SQLite store: idempotent inserts, ordered export, restart recovery."""

import os
import tempfile
import unittest

from holdfast.store import Store, pack_samples, unpack_samples


class TestStore(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, "t.db")
        self.store = Store(self.path)

    def tearDown(self):
        self.store.close()
        self.dir.cleanup()

    def insert(self, seq, samples, late=False):
        return self.store.insert_packet(1, seq, 1, 1000 + seq * 10, 10, 1, 2, 5000,
                                        late, 100, samples)

    def test_samples_pack(self):
        s = [0, 1, -1, 2**31 - 1, -(2**31)]
        self.assertEqual(unpack_samples(pack_samples(s)), s)

    def test_insert_is_idempotent_and_ordered(self):
        self.assertTrue(self.insert(2, [3, 4]))
        self.assertTrue(self.insert(0, [1]))
        self.assertFalse(self.insert(2, [9, 9]))  # duplicate ignored
        self.assertTrue(self.insert(1, [2], late=True))
        self.store.commit()
        rows = list(self.store.packets(1))
        self.assertEqual([r[0] for r in rows], [0, 1, 2])
        self.assertEqual([r[3] for r in rows], [[1], [2], [3, 4]])
        self.assertEqual(self.store.packet_stats(1), (3, 4, 300))
        self.assertEqual(list(self.store.received_seqs(1)), [0, 1, 2])

    def test_station_state_survives_reopen(self):
        self.store.touch_station(1, 10, "10.0.0.1:5000")
        self.store.update_heartbeat(1, 50, 20, 2, 3600)
        self.store.record_unrecoverable(1, 5, 9, 11)
        self.insert(30, [1, 2, 3])
        self.store.close()

        again = Store(self.path)
        try:
            (row,) = again.stations()
            self.assertEqual((row["station_id"], row["next_seq"], row["oldest_seq"],
                              row["last_addr"]), (1, 50, 20, "10.0.0.1:5000"))
            self.assertEqual(again.unrecoverable(1), [(5, 9)])
            self.assertEqual(list(again.received_seqs(1)), [30])
        finally:
            again.close()

    def test_touch_keeps_address_when_none_given(self):
        self.store.touch_station(1, 10, "a:1")
        self.store.touch_station(1, 20, None)
        (row,) = self.store.stations()
        self.assertEqual((row["last_addr"], row["last_seen_ns"]), ("a:1", 20))


if __name__ == "__main__":
    unittest.main()
