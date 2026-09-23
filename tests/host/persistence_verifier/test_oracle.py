from __future__ import annotations

import unittest

from fixture_factory import csv_bytes, record_values, storage_words
from persistence import oracle


class OracleTests(unittest.TestCase):
    def test_u16_test_value_wraps(self) -> None:
        self.assertEqual(oracle.expected_test_value(6555, 3), 17)
        self.assertEqual(oracle.expected_test_value(65536, 0), 0)

    def test_config_boundaries(self) -> None:
        valid = (
            (10, 1, 0),
            (10, 15, 65535),
            (3600, 1, 0),
            (3600, 15, 65535),
        )
        for period, mask, count in valid:
            with self.subTest(period=period, mask=mask, count=count):
                self.assertTrue(oracle.config_is_valid(period, mask, count))
        for period, mask, count in (
            (9, 1, 0),
            (3601, 1, 0),
            (10, 0, 0),
            (10, 16, 0),
            (10, 1, -1),
        ):
            with self.subTest(period=period, mask=mask, count=count):
                self.assertFalse(oracle.config_is_valid(period, mask, count))

    def test_u32_delta_wraps(self) -> None:
        self.assertEqual(oracle.u32_delta(5, 0xFFFFFFFE), 7)
        self.assertEqual(oracle.u32_delta(0, 0xFFFFFFFF), 1)

    def test_schema1_header_remains_parseable(self) -> None:
        values = record_values(
            seq=1,
            session=1,
            trigger=1,
            period_s=10,
            mask=1,
            sample_count=0,
            config_version=1,
            utc_valid=0,
            utc_s=0,
            file_id=1,
            file_date=0,
            schema=1,
        )
        parsed = oracle.parse_csv_bytes(
            csv_bytes([values]),
            "LOG/UNSET/00000001.CSV",
        )
        self.assertEqual(len(parsed.records), 1)
        self.assertEqual(parsed.records[0].schema, 1)

    def test_storage_conservation_accepts_wrap(self) -> None:
        storage = oracle.decode_storage_block(
            storage_words(
                generated=5,
                synced=0xFFFFFFFE,
                uncertain=7,
                last_synced_seq=1,
                last_synced_file=1,
            )
        )
        self.assertTrue(oracle.storage_conservation_holds(storage))

    def test_invalid_utc_dictionary_is_a_contract_issue(self) -> None:
        record = oracle.CsvRecord(
            **{
                "schema": 1,
                "session": 1,
                "seq": 1,
                "trigger": 1,
                "planned_ms": 1,
                "actual_ms": 1,
                "utc_valid": 0,
                "utc_s": 123,
                "config_version": 1,
                "period_s": 10,
                "mask": 1,
                "sample_count": 0,
                "source": 1,
                "v0": 10,
                "v1": 0,
                "v2": 0,
                "v3": 0,
                "u0": 1,
                "u1": 1,
                "u2": 1,
                "u3": 1,
                "q0": 1,
                "q1": 0,
                "q2": 0,
                "q3": 0,
                "file_id": 1,
                "file_date": 0,
                "reserved": 0,
            }
        )
        codes = {
            issue["code"]
            for issue in oracle.validate_record_contract(
                record,
                config_versions={1: {"period_s": 10, "mask": 1, "sample_count": 0}},
            )
        }
        self.assertIn("utc_contradiction", codes)

    def test_schema2_real_dht11_record_is_validated(self) -> None:
        values = record_values(
            seq=1,
            session=1,
            trigger=1,
            period_s=10,
            mask=1,
            sample_count=0,
            config_version=1,
            utc_valid=0,
            utc_s=0,
            file_id=1,
            file_date=0,
        )
        values["source"] = 2
        for index in range(4):
            values[f"v{index}"] = 0
            values[f"q{index}"] = 0
        values.update(
            {
                "dht_valid_mask": 7,
                "dht_sample_id": 99,
                "dht0_temp_x10": 230,
                "dht1_temp_x10": 240,
                "dht2_temp_x10": 250,
                "dht0_humidity_x10": 450,
                "dht1_humidity_x10": 550,
                "dht2_humidity_x10": 650,
                "dht0_quality": 1,
                "dht1_quality": 5,
                "dht2_quality": 1,
                "dht0_error": 0,
                "dht1_error": 3,
                "dht2_error": 0,
                "dht0_sample_ms": 1000,
                "dht1_sample_ms": 2000,
                "dht2_sample_ms": 3000,
            }
        )

        record = oracle.CsvRecord(**values)
        self.assertEqual(
            oracle.validate_record_contract(
                record,
                config_versions={1: {"period_s": 10, "mask": 1, "sample_count": 0}},
            ),
            [],
        )

        values["dht1_error"] = 0
        invalid = oracle.CsvRecord(**values)
        codes = {
            issue["code"]
            for issue in oracle.validate_record_contract(
                invalid,
                config_versions=None,
            )
        }
        self.assertIn("dht11_stale_error", codes)

    def test_schema3_real_ds18b20_record_is_validated(self) -> None:
        values = record_values(
            seq=1,
            session=1,
            trigger=1,
            period_s=10,
            mask=1,
            sample_count=0,
            config_version=1,
            utc_valid=0,
            utc_s=0,
            file_id=1,
            file_date=0,
            schema=3,
        )
        values["source"] = 3
        for index in range(4):
            values[f"v{index}"] = 0
            values[f"u{index}"] = 2
            values[f"q{index}"] = 0
        values.update(
            {
                "ds18b20_valid_mask": 7,
                "ds18b20_sample_id": 42,
                "ds18b20_0_temp_x16": 230,
                "ds18b20_1_temp_x16": -160,
                "ds18b20_2_temp_x16": 0,
                "ds18b20_0_quality": 1,
                "ds18b20_1_quality": 5,
                "ds18b20_2_quality": 1,
                "ds18b20_0_error": 0,
                "ds18b20_1_error": 6,
                "ds18b20_2_error": 0,
                "ds18b20_0_rom_short": 0x1200,
                "ds18b20_1_rom_short": 0x1201,
                "ds18b20_2_rom_short": 0x1202,
                "ds18b20_0_sample_ms": 1000,
                "ds18b20_1_sample_ms": 2000,
                "ds18b20_2_sample_ms": 3000,
            }
        )

        record = oracle.CsvRecord(**values)
        self.assertEqual(
            oracle.validate_record_contract(
                record,
                config_versions={1: {"period_s": 10, "mask": 1, "sample_count": 0}},
            ),
            [],
        )

        values["ds18b20_1_error"] = 0
        invalid = oracle.CsvRecord(**values)
        codes = {
            issue["code"]
            for issue in oracle.validate_record_contract(
                invalid,
                config_versions=None,
            )
        }
        self.assertIn("ds18b20_error_mismatch", codes)

    def test_schema4_event_row_is_validated(self) -> None:
        values = record_values(
            seq=42,
            session=1,
            trigger=0,
            period_s=30,
            mask=1,
            sample_count=0,
            config_version=1,
            utc_valid=0,
            utc_s=0,
            file_id=1,
            file_date=0,
            schema=4,
            planned_ms=1000,
            actual_ms=1000,
        )
        values["source"] = 3
        for index in range(4):
            values[f"v{index}"] = 0
            values[f"u{index}"] = 2
            values[f"q{index}"] = 0
        values.update(
            {
                "ds18b20_valid_mask": 6,
                "ds18b20_sample_id": 42,
                "ds18b20_0_temp_x16": 0,
                "ds18b20_1_temp_x16": 250,
                "ds18b20_2_temp_x16": -100,
                "ds18b20_0_quality": 3,
                "ds18b20_1_quality": 1,
                "ds18b20_2_quality": 5,
                "ds18b20_0_error": 0,
                "ds18b20_1_error": 0,
                "ds18b20_2_error": 0,
                "ds18b20_0_rom_short": 0,
                "ds18b20_1_rom_short": 0,
                "ds18b20_2_rom_short": 0,
                "ds18b20_0_sample_ms": 0,
                "ds18b20_1_sample_ms": 0,
                "ds18b20_2_sample_ms": 0,
                "event_id": 7,
                "event_phase": 2,
                "event_level": 2,
                "event_reason": 2,
                "event_trigger_phase": 1,
                "event_max_delta_x16": 350,
                "event_delta_valid": 1,
                "event_flags": 0x201,
            }
        )

        record = oracle.CsvRecord(**values)
        self.assertEqual(
            oracle.validate_record_contract(
                record,
                config_versions={1: {"period_s": 30, "mask": 1, "sample_count": 0}},
            ),
            [],
        )

        values["event_phase"] = 6
        invalid = oracle.CsvRecord(**values)
        codes = {
            issue["code"]
            for issue in oracle.validate_record_contract(
                invalid,
                config_versions=None,
            )
        }
        self.assertIn("event_phase", codes)

        values["event_phase"] = 2
        values["event_flags"] = 0x400
        invalid = oracle.CsvRecord(**values)
        codes = {
            issue["code"]
            for issue in oracle.validate_record_contract(
                invalid,
                config_versions=None,
            )
        }
        self.assertIn("event_flags", codes)

        values["event_phase"] = 2
        values["event_flags"] = 0
        values["ds18b20_sample_id"] = 99
        invalid = oracle.CsvRecord(**values)
        codes = {
            issue["code"]
            for issue in oracle.validate_record_contract(
                invalid,
                config_versions=None,
            )
        }
        self.assertIn("event_sample_id", codes)

        values["ds18b20_sample_id"] = 42
        values["ds18b20_0_error"] = 1
        invalid = oracle.CsvRecord(**values)
        codes = {
            issue["code"]
            for issue in oracle.validate_record_contract(
                invalid,
                config_versions=None,
            )
        }
        self.assertIn("event_aux_fields", codes)

    def test_event_timeline_allows_historical_pre_rows(self) -> None:
        normal = oracle.CsvRecord(
            **record_values(
                seq=1,
                session=1,
                trigger=1,
                period_s=30,
                mask=1,
                sample_count=0,
                config_version=1,
                utc_valid=0,
                utc_s=0,
                file_id=1,
                file_date=0,
                planned_ms=30_000,
                actual_ms=30_000,
                schema=4,
            )
        )
        event_values = record_values(
            seq=2,
            session=1,
            trigger=0,
            period_s=30,
            mask=1,
            sample_count=0,
            config_version=1,
            utc_valid=0,
            utc_s=0,
            file_id=1,
            file_date=0,
            planned_ms=20_000,
            actual_ms=20_000,
            schema=4,
        )
        event_values["source"] = 3
        event_values.update(
            {
                "ds18b20_sample_id": 2,
                "ds18b20_valid_mask": 7,
                "ds18b20_0_temp_x16": 400,
                "ds18b20_1_temp_x16": 400,
                "ds18b20_2_temp_x16": 400,
                "ds18b20_0_quality": 1,
                "ds18b20_1_quality": 1,
                "ds18b20_2_quality": 1,
                "event_id": 1,
                "event_phase": 1,
                "event_flags": 0,
            }
        )
        pre = oracle.CsvRecord(**event_values)
        event_values.update(
            {
                "planned_ms": 31_000,
                "actual_ms": 31_000,
                "event_phase": 2,
            }
        )
        trigger = oracle.CsvRecord(**event_values)
        self.assertEqual(
            oracle.validate_sampling_timeline([normal, pre, trigger]),
            [],
        )

        event_values.update(
            {
                "planned_ms": 19_000,
                "actual_ms": 19_000,
                "event_phase": 2,
            }
        )
        bad_trigger = oracle.CsvRecord(**event_values)
        codes = {
            issue["code"]
            for issue in oracle.validate_sampling_timeline(
                [normal, pre, bad_trigger]
            )
        }
        self.assertIn("actual_order", codes)


if __name__ == "__main__":
    unittest.main()
