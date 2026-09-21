from __future__ import annotations

import unittest

from tools.modbus_client import registers


class Dht11RegisterTests(unittest.TestCase):
    def test_decode_extension_block(self) -> None:
        values = [
            1,
            2,
            0x0007,
            0,
            5,
            230,
            240,
            250,
            450,
            550,
            650,
            1,
            5,
            6,
            0,
            3,
            1,
            0,
            1000,
            0,
            2000,
            0,
            3000,
            1,
        ]

        decoded = registers.decode_dht11_snapshot(values)

        self.assertEqual(decoded["contract_revision"], 1)
        self.assertEqual(decoded["source"], "REAL_DHT11")
        self.assertEqual(decoded["sample_id"], 5)
        self.assertEqual(decoded["valid_mask"], 0x0007)
        self.assertEqual(decoded["sensors"][0]["temperature_x10"], 230)
        self.assertEqual(decoded["sensors"][2]["humidity_x10"], 650)
        self.assertEqual(decoded["sensors"][1]["quality"], "STALE")
        self.assertEqual(decoded["sensors"][1]["error"], "CHECKSUM")
        self.assertEqual(decoded["sensors"][2]["sample_time_ms"], 3000)

    def test_rejects_wrong_block_length(self) -> None:
        with self.assertRaises(ValueError):
            registers.decode_dht11_snapshot([1, 2, 3])


if __name__ == "__main__":
    unittest.main()
