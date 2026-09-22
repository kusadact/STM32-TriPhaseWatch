from __future__ import annotations

import unittest

from tools.modbus_client import registers


def thermal_alarm_values() -> list[int]:
    return [
        1,
        0,
        0,
        registers.ALARM_FLAG_VALID,
        0,
        0,
        0,
        320,
        1,
        320,
        320,
        320,
        1,
        1,
        1,
        0,
        7,
        0,
        9,
        0,
        0,
        1,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    ]


def alarm_config_values() -> list[int]:
    return [
        50 * 16,
        55 * 16,
        75 * 16,
        5 * 16,
        10 * 16,
        15 * 16,
        5 * 16,
        10 * 16,
        20 * 16,
        3,
        5,
        2 * 16,
        4,
        0,
        1000,
        1,
    ]


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


class ThermalAlarmRegisterTests(unittest.TestCase):
    def test_decode_normal_block_and_signed_zero(self) -> None:
        values = thermal_alarm_values()
        values[9:12] = [(-160) & 0xFFFF, 0, 160]
        values[7] = 160
        values[8] = 3
        values[5] = 1
        values[6] = 320

        decoded = registers.decode_thermal_alarm(values)

        self.assertEqual(decoded["contract_revision"], 1)
        self.assertEqual(decoded["level"], "NORMAL")
        self.assertEqual(decoded["event_id"], 7)
        self.assertEqual(decoded["alarm_sample_id"], 9)
        self.assertEqual(
            [phase["temperature_x16"] for phase in decoded["phases"]],
            [-160, 0, 160],
        )
        self.assertEqual(decoded["maximum_delta_x16"], 320)

    def test_fault_never_publishes_delta(self) -> None:
        values = thermal_alarm_values()
        values[1] = 4
        values[2] = 6
        values[3] = (
            registers.ALARM_FLAG_VALID
            | registers.ALARM_FLAG_LATCHED
            | registers.ALARM_FLAG_BUZZER_ACTIVE
        )
        values[4] = 2
        values[12:15] = [1, 3, 1]

        decoded = registers.decode_thermal_alarm(values)

        self.assertEqual(decoded["level"], "SENSOR_FAULT")
        self.assertEqual(decoded["reason"], "SENSOR_CRC_ERROR")
        self.assertFalse(decoded["delta_valid"])
        self.assertIsNone(decoded["maximum_delta_x16"])
        self.assertIsNone(decoded["phases"][1]["temperature_x16"])

        values[5] = 1
        values[6] = 0
        values[11] = 336
        with self.assertRaises(ValueError):
            registers.decode_thermal_alarm(values)

    def test_unknown_alarm_hides_zero_quality_and_temperature(self) -> None:
        values = thermal_alarm_values()
        values[1] = 5
        values[2] = 0
        values[3] = 0
        values[4] = 0
        values[5] = 0
        values[6] = 0
        values[7] = 0
        values[8] = 0
        values[9:12] = [0, 0, 0]
        values[12:15] = [0, 0, 0]

        decoded = registers.decode_thermal_alarm(values)

        self.assertFalse(decoded["valid"])
        self.assertEqual(decoded["level"], "UNKNOWN")
        self.assertTrue(
            all(phase["temperature_x16"] is None for phase in decoded["phases"])
        )
        self.assertTrue(
            all(phase["quality"] == "UNKNOWN(0)" for phase in decoded["phases"])
        )

    def test_rejects_invalid_revision_length_flags_and_quality(self) -> None:
        cases = []

        values = thermal_alarm_values()
        values[0] = 2
        cases.append(values)

        values = thermal_alarm_values()
        values[3] = 0x80 | registers.ALARM_FLAG_VALID
        cases.append(values)

        values = thermal_alarm_values()
        values[12] = 9
        cases.append(values)

        for values in cases:
            with self.subTest(values=values):
                with self.assertRaises(ValueError):
                    registers.decode_thermal_alarm(values)

        with self.assertRaises(ValueError):
            registers.decode_thermal_alarm([1, 2, 3])

    def test_rejects_inconsistent_delta_and_phase_temperature(self) -> None:
        values = thermal_alarm_values()
        values[1] = 1
        values[2] = 2
        values[3] |= registers.ALARM_FLAG_LATCHED
        values[4] = 1
        values[5] = 1
        values[6] = 16
        values[7] = 320
        values[9] = 320
        values[10] = 320
        values[11] = 320

        with self.assertRaises(ValueError):
            registers.decode_thermal_alarm(values)

        values[6] = 0
        values[11] = 336
        with self.assertRaises(ValueError):
            registers.decode_thermal_alarm(values)

    def test_decode_alarm_config(self) -> None:
        decoded = registers.decode_thermal_alarm_config(alarm_config_values())

        self.assertEqual(decoded["phase_warning_x16"], 55 * 16)
        self.assertEqual(decoded["delta_notice_x16"], 5 * 16)
        self.assertEqual(decoded["assert_samples"], 3)
        self.assertEqual(decoded["clear_samples"], 5)
        self.assertEqual(decoded["rise_window_min_ms"], 1000)
        self.assertTrue(decoded["buzzer_enable"])

    def test_rejects_invalid_alarm_config(self) -> None:
        values = alarm_config_values()
        values[1] = values[0]
        with self.assertRaises(ValueError):
            registers.decode_thermal_alarm_config(values)

        values = alarm_config_values()
        values[15] = 2
        with self.assertRaises(ValueError):
            registers.decode_thermal_alarm_config(values)


if __name__ == "__main__":
    unittest.main()
