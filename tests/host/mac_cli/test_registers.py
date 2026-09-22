from __future__ import annotations

import unittest

from tools.modbus_client import registers


class Ds18b20RegisterTests(unittest.TestCase):
    def test_decode_extension_block(self) -> None:
        values = [
            2,
            3,
            0x0007,
            0,
            5,
            230,
            0xFF60,
            0,
            1,
            5,
            1,
            0,
            6,
            0,
            0,
            1000,
            0,
            2000,
            0,
            3000,
            2,
            0x1200,
            0x1201,
            0x1202,
        ]

        decoded = registers.decode_ds18b20_snapshot(values)

        self.assertEqual(decoded["contract_revision"], 2)
        self.assertEqual(decoded["source"], "REAL_DS18B20")
        self.assertEqual(decoded["sample_id"], 5)
        self.assertEqual(decoded["valid_mask"], 0x0007)
        self.assertEqual(decoded["sensors"][0]["temperature_x16"], 230)
        self.assertEqual(decoded["sensors"][1]["temperature_x16"], -160)
        self.assertEqual(decoded["sensors"][1]["quality"], "STALE")
        self.assertEqual(decoded["sensors"][1]["error"], "SCRATCHPAD_CRC")
        self.assertEqual(decoded["sensors"][2]["sample_time_ms"], 3000)
        self.assertEqual(decoded["sensors"][2]["rom_short"], 0x1202)
        self.assertEqual(decoded["sensor_type_code"], 2)

    def test_rejects_wrong_block_length(self) -> None:
        with self.assertRaises(ValueError):
            registers.decode_ds18b20_snapshot([1, 2, 3])


if __name__ == "__main__":
    unittest.main()
