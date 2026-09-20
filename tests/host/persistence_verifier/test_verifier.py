from __future__ import annotations

from copy import deepcopy
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from typing import Callable

from fixture_factory import (
    BASE_UTC,
    make_valid_fixture,
    rewrite_csv,
)
from persistence import oracle
from persistence.manifest import write_json_atomic
from persistence.verify import verify_run


def _read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def _write_jsonl(path: Path, rows: list[dict]) -> None:
    path.write_text(
        "".join(
            json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n"
            for row in rows
        ),
        encoding="utf-8",
    )


def _read_records(path: Path) -> list[dict[str, int]]:
    parsed = oracle.parse_csv_bytes(path.read_bytes(), "LOG/20260920/00000001.CSV")
    return [dict(zip(oracle.CSV_COLUMNS, record.values)) for record in parsed.records]


def _refresh_manifest(run_dir: Path, files_dir: Path, relative: str) -> None:
    absolute = files_dir / relative
    manifest_path = run_dir / "file_manifest.json"
    manifest = _read_json(manifest_path)
    for entry in manifest["files"]:
        if entry["path"] == relative:
            data = absolute.read_bytes()
            entry["size"] = len(data)
            entry["sha256"] = hashlib.sha256(data).hexdigest()
            break
    write_json_atomic(manifest_path, manifest)


def _mutate_manifest(
    run_dir: Path,
    callback: Callable[[dict], None],
) -> None:
    path = run_dir / "manifest.json"
    manifest = _read_json(path)
    callback(manifest)
    write_json_atomic(path, manifest)


def _mutate_records(
    run_dir: Path,
    files_dir: Path,
    callback: Callable[[list[dict[str, int]]], list[dict[str, int]]],
) -> None:
    relative = "LOG/20260920/00000001.CSV"
    path = files_dir / relative
    records = _read_records(path)
    records = callback(records)
    rewrite_csv(files_dir, relative, records)
    _refresh_manifest(run_dir, files_dir, relative)


class VerifierTests(unittest.TestCase):
    def _verify(
        self,
        root: Path,
        run_dir: Path,
        files_dir: Path,
    ):
        return verify_run(run_dir, files_dir, root / "verify")

    def test_valid_synthetic_fixture_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run_dir, files_dir = make_valid_fixture(root)
            report = self._verify(root, run_dir, files_dir)
            self.assertEqual(report.exit_code, 0, report.issues)
            self.assertEqual(report.overall, "PASS")

    def test_invalid_utc_uses_unset_path_and_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run_dir, files_dir = make_valid_fixture(root, utc_valid=0)
            report = self._verify(root, run_dir, files_dir)
            self.assertEqual(report.exit_code, 0, report.issues)
            self.assertTrue(
                (files_dir / "LOG/UNSET/00000001.CSV").is_file()
            )

    def test_explicit_historical_time_passes(self) -> None:
        # The device clock is user-set: a value that differs from the host
        # wall clock is legal as long as the records follow the SET_TIME
        # anchor and the sampling clock.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run_dir, files_dir = make_valid_fixture(
                root,
                utc_base=BASE_UTC - 3600,
            )
            report = self._verify(root, run_dir, files_dir)
            self.assertEqual(report.exit_code, 0, report.issues)
            self.assertEqual(report.overall, "PASS")

    def test_mid_run_set_time_segments_pass(self) -> None:
        # A legal second SET_TIME before the fourth record jumps the device
        # clock; the UTC check must segment on every successful SET_TIME
        # instead of comparing across the boundary or against the host clock.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run_dir, files_dir = make_valid_fixture(
                root,
                record_count=7,
                second_set_time=(3, BASE_UTC + 3600 + (3 * 10 - 5)),
            )
            report = self._verify(root, run_dir, files_dir)
            self.assertEqual(report.exit_code, 0, report.issues)
            self.assertEqual(report.overall, "PASS")

    def test_set_time_between_sample_and_snapshot(self) -> None:
        # seq3 is sampled before SET_TIME but first observed just after it.
        # Neither an old-clock nor a new-clock UTC proves segment membership.
        for shift, expected in ((0, 3), (3600, 3), (7200, 1)):
            with self.subTest(shift=shift), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                run_dir, files_dir = make_valid_fixture(
                    root, second_set_time=(3, BASE_UTC + 3625)
                )
                path = run_dir / "observations.jsonl"
                observations = [json.loads(line) for line in path.read_text().splitlines()]
                for observation in observations:
                    value = observation.get("value", {})
                    if value.get("requested_utc_seconds") == BASE_UTC + 3625:
                        value["requested_utc_seconds"] = BASE_UTC + 3620
                        observation["observed_monotonic_s"] = 120.5
                    if observation.get("kind") == "snapshot" and value["sequence"] == 3:
                        observation["observed_monotonic_s"] = 121.0
                timed = [row for row in observations if "observed_monotonic_s" in row]
                timed.sort(key=lambda row: row["observed_monotonic_s"])
                # Untimed config/storage evidence keeps its original position.
                iterator = iter(timed)
                observations = [next(iterator) if "observed_monotonic_s" in row else row
                                for row in observations]
                _write_jsonl(path, observations)
                if shift:
                    def shift_boundary(records):
                        records[2]["utc_s"] += shift
                        return records
                    _mutate_records(run_dir, files_dir, shift_boundary)
                report = self._verify(root, run_dir, files_dir)
                self.assertEqual(report.exit_code, expected, report.issues)
                codes = {issue["code"] for issue in report.issues}
                self.assertIn("utc_segment_evidence", codes)
                self.assertNotIn("utc_step", codes)
                if expected == 1:
                    self.assertIn("utc_host_mapping", codes)
                else:
                    self.assertNotIn("utc_host_mapping", codes)

    def test_set_time_boundary_does_not_hide_later_bad_step(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run_dir, files_dir = make_valid_fixture(
                root, second_set_time=(3, BASE_UTC + 3625)
            )
            def damage_step(records):
                # Within the new segment and within absolute UTC tolerance,
                # but inconsistent with the preceding sampling-clock delta.
                records[5]["utc_s"] += 2
                return records
            _mutate_records(run_dir, files_dir, damage_step)
            report = self._verify(root, run_dir, files_dir)
            self.assertEqual(report.exit_code, 1, report.issues)
            self.assertIn("utc_step", {issue["code"] for issue in report.issues})

    def test_u16_channel_wrap_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run_dir, files_dir = make_valid_fixture(
                root,
                seq_start=6555,
                record_count=4,
            )
            report = self._verify(root, run_dir, files_dir)
            self.assertEqual(report.exit_code, 0, report.issues)

    def test_u32_counter_wrap_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run_dir, files_dir = make_valid_fixture(
                root,
                record_count=7,
                seq_start=10,
                start_generated=0xFFFFFFFE,
                end_generated=5,
                end_synced=5,
            )
            report = self._verify(root, run_dir, files_dir)
            self.assertEqual(report.exit_code, 0, report.issues)

    def test_configuration_boundaries_pass(self) -> None:
        for period, mask, count in (
            (10, 1, 0),
            (10, 15, 65535),
            (3600, 1, 0),
            (3600, 15, 65535),
        ):
            with self.subTest(period=period, mask=mask, count=count):
                with tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    run_dir, files_dir = make_valid_fixture(
                        root,
                        period_s=period,
                        mask=mask,
                        sample_count=count,
                    )
                    report = self._verify(root, run_dir, files_dir)
                    self.assertEqual(report.exit_code, 0, report.issues)

    def test_repeated_poll_does_not_duplicate_records(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run_dir, files_dir = make_valid_fixture(
                root,
                record_count=2,
                include_repeated_poll=True,
            )
            observations = [
                json.loads(line)
                for line in (run_dir / "observations.jsonl").read_text().splitlines()
            ]
            snapshots = [
                row for row in observations if row["kind"] == "snapshot"
            ]
            self.assertGreater(len(snapshots), 2)
            report = self._verify(root, run_dir, files_dir)
            self.assertEqual(report.exit_code, 0, report.issues)

    def test_start_checkpoint_is_authoritative_over_initial_observation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run_dir, files_dir = make_valid_fixture(root)
            observations_path = run_dir / "observations.jsonl"
            rows = [
                json.loads(line)
                for line in observations_path.read_text(
                    encoding="utf-8"
                ).splitlines()
            ]
            first_storage = next(
                row for row in rows if row.get("kind") == "storage"
            )
            first_storage["value"].update(
                {
                    "generated": 100,
                    "synced": 100,
                    "last_synced_seq": 99,
                    "last_synced_file": 1,
                    "last_synced_date": 20260920,
                }
            )
            _write_jsonl(observations_path, rows)
            report = self._verify(root, run_dir, files_dir)
            self.assertEqual(report.exit_code, 0, report.issues)

    def test_all_single_point_mutations_are_detected(self) -> None:
        cases: list[tuple[str, int, set[str], Callable[[Path, Path], None]]] = []

        def delete_middle(run_dir: Path, files_dir: Path) -> None:
            _mutate_records(
                run_dir,
                files_dir,
                lambda records: records[:2] + records[3:],
            )

        def duplicate_row(run_dir: Path, files_dir: Path) -> None:
            _mutate_records(
                run_dir,
                files_dir,
                lambda records: records[:2] + [deepcopy(records[1])] + records[2:],
            )

        def swap_order(run_dir: Path, files_dir: Path) -> None:
            def mutate(records: list[dict[str, int]]) -> list[dict[str, int]]:
                records = deepcopy(records)
                records[1], records[2] = records[2], records[1]
                return records

            _mutate_records(run_dir, files_dir, mutate)

        def change_channel_value(run_dir: Path, files_dir: Path) -> None:
            def mutate(records: list[dict[str, int]]) -> list[dict[str, int]]:
                records[1]["v1"] = (records[1]["v1"] + 1) & 0xFFFF
                return records

            _mutate_records(run_dir, files_dir, mutate)

        def change_unit(run_dir: Path, files_dir: Path) -> None:
            def mutate(records: list[dict[str, int]]) -> list[dict[str, int]]:
                records[1]["u2"] = 2
                return records

            _mutate_records(run_dir, files_dir, mutate)

        def change_quality(run_dir: Path, files_dir: Path) -> None:
            def mutate(records: list[dict[str, int]]) -> list[dict[str, int]]:
                records[1]["q3"] = 0
                return records

            _mutate_records(run_dir, files_dir, mutate)

        def change_mask(run_dir: Path, files_dir: Path) -> None:
            def mutate(records: list[dict[str, int]]) -> list[dict[str, int]]:
                records[1]["mask"] = 7
                return records

            _mutate_records(run_dir, files_dir, mutate)

        def change_config_version(run_dir: Path, files_dir: Path) -> None:
            def mutate(records: list[dict[str, int]]) -> list[dict[str, int]]:
                records[1]["config_version"] = 99
                return records

            _mutate_records(run_dir, files_dir, mutate)

        def zero_sampling_times(run_dir: Path, files_dir: Path) -> None:
            def mutate(records: list[dict[str, int]]) -> list[dict[str, int]]:
                for record in records:
                    record["planned_ms"] = 0
                    record["actual_ms"] = 0
                return records

            _mutate_records(run_dir, files_dir, mutate)

        def utc_shift_same_day(run_dir: Path, files_dir: Path) -> None:
            def mutate(records: list[dict[str, int]]) -> list[dict[str, int]]:
                for record in records:
                    record["utc_s"] += 3600
                return records

            _mutate_records(run_dir, files_dir, mutate)

        def single_time_mismatch(run_dir: Path, files_dir: Path) -> None:
            def mutate(records: list[dict[str, int]]) -> list[dict[str, int]]:
                records[1]["trigger"] = 2
                return records

            _mutate_records(run_dir, files_dir, mutate)

        def utc_contradiction(run_dir: Path, files_dir: Path) -> None:
            def mutate(records: list[dict[str, int]]) -> list[dict[str, int]]:
                records[1]["utc_valid"] = 0
                return records

            _mutate_records(run_dir, files_dir, mutate)

        def wrong_date(run_dir: Path, files_dir: Path) -> None:
            def mutate(records: list[dict[str, int]]) -> list[dict[str, int]]:
                records[1]["file_date"] += 1
                return records

            _mutate_records(run_dir, files_dir, mutate)

        def wrong_id(run_dir: Path, files_dir: Path) -> None:
            def mutate(records: list[dict[str, int]]) -> list[dict[str, int]]:
                records[1]["file_id"] = 2
                return records

            _mutate_records(run_dir, files_dir, mutate)

        def same_seq_different_payload(run_dir: Path, files_dir: Path) -> None:
            def mutate(records: list[dict[str, int]]) -> list[dict[str, int]]:
                duplicate = deepcopy(records[2])
                duplicate["seq"] = records[1]["seq"]
                duplicate["v0"] = (duplicate["v0"] + 1) & 0xFFFF
                return records[:2] + [duplicate] + records[2:]

            _mutate_records(run_dir, files_dir, mutate)

        def truncated_tail(run_dir: Path, files_dir: Path) -> None:
            relative = "LOG/20260920/00000001.CSV"
            path = files_dir / relative
            path.write_bytes(path.read_bytes()[:-1])
            _refresh_manifest(run_dir, files_dir, relative)

        def wrong_column_count(run_dir: Path, files_dir: Path) -> None:
            relative = "LOG/20260920/00000001.CSV"
            path = files_dir / relative
            lines = path.read_text(encoding="ascii").splitlines()
            lines[1] = lines[1].rsplit(",", 1)[0]
            path.write_text("\n".join(lines) + "\n", encoding="ascii")
            _refresh_manifest(run_dir, files_dir, relative)

        def unknown_schema(run_dir: Path, files_dir: Path) -> None:
            def mutate(records: list[dict[str, int]]) -> list[dict[str, int]]:
                records[1]["schema"] = 2
                return records

            _mutate_records(run_dir, files_dir, mutate)

        def missing_file(run_dir: Path, files_dir: Path) -> None:
            (files_dir / "LOG/20260920/00000001.CSV").unlink()

        def hash_change(run_dir: Path, files_dir: Path) -> None:
            path = files_dir / "LOG/20260920/00000001.CSV"
            path.write_bytes(path.read_bytes() + b"\n")

        def missing_manifest_entry(run_dir: Path, files_dir: Path) -> None:
            source = files_dir / "LOG/20260920/00000001.CSV"
            target = files_dir / "LOG/20260920/00000002.CSV"
            shutil.copyfile(source, target)

        def conservation_mismatch(run_dir: Path, files_dir: Path) -> None:
            path = run_dir / "observations.jsonl"
            rows = [
                json.loads(line)
                for line in path.read_text(encoding="utf-8").splitlines()
            ]
            for row in rows:
                if row.get("kind") == "storage":
                    row["value"]["generated"] = 8
                    break
            _write_jsonl(path, rows)

        def sync_count_mismatch(run_dir: Path, files_dir: Path) -> None:
            path = run_dir / "observations.jsonl"
            rows = [
                json.loads(line)
                for line in path.read_text(encoding="utf-8").splitlines()
            ]
            last_storage = None
            for row in rows:
                if row.get("kind") == "storage":
                    last_storage = row
            assert last_storage is not None
            last_storage["value"]["generated"] = 7
            last_storage["value"]["synced"] = 6
            last_storage["value"]["queued"] = 1
            last_storage["value"]["queue_high_water"] = 1
            last_storage["value"]["last_synced_seq"] = 6
            _write_jsonl(path, rows)

        def half_run(run_dir: Path, files_dir: Path) -> None:
            _mutate_manifest(
                run_dir,
                lambda manifest: manifest.update(
                    {
                        "scenario": "baseline",
                        "expected_duration_s": 7200.0,
                        "observed_duration_s": 3600.0,
                        "completion_reason": "duration_elapsed",
                    }
                ),
            )

        cases.extend(
            [
                ("delete_middle", 1, {"generated_file_count", "sequence_order"}, delete_middle),
                ("duplicate_row", 1, {"duplicate_record", "generated_file_count"}, duplicate_row),
                ("swap_order", 1, {"sequence_order"}, swap_order),
                ("change_channel_value", 1, {"channel_value"}, change_channel_value),
                ("change_unit", 1, {"unit"}, change_unit),
                ("change_quality", 1, {"channel_quality"}, change_quality),
                ("change_mask", 1, {"config_mismatch"}, change_mask),
                (
                    "change_config_version",
                    3,
                    {"config_version_evidence"},
                    change_config_version,
                ),
                ("utc_contradiction", 1, {"utc_contradiction"}, utc_contradiction),
                ("wrong_date", 1, {"file_date_mismatch", "utc_file_date"}, wrong_date),
                ("wrong_id", 1, {"file_id_mismatch"}, wrong_id),
                (
                    "same_seq_different_payload",
                    1,
                    {"same_seq_payload_mismatch"},
                    same_seq_different_payload,
                ),
                ("truncated_tail", 1, {"truncated_tail"}, truncated_tail),
                ("wrong_column_count", 1, {"column_count"}, wrong_column_count),
                ("unknown_schema", 1, {"schema"}, unknown_schema),
                ("missing_file", 1, {"file_missing"}, missing_file),
                ("hash_change", 1, {"file_hash"}, hash_change),
                (
                    "missing_manifest_entry",
                    1,
                    {"file_manifest_missing_entry"},
                    missing_manifest_entry,
                ),
                ("conservation_mismatch", 1, {"conservation"}, conservation_mismatch),
                (
                    "sync_count_mismatch",
                    1,
                    {"sync_file_count_mismatch"},
                    sync_count_mismatch,
                ),
                ("half_run", 1, {"half_run_as_complete"}, half_run),
                ("zero_sampling_times", 1, {"planned_step"}, zero_sampling_times),
                ("utc_shift_same_day", 1, {"utc_host_mapping"}, utc_shift_same_day),
                ("single_time_mismatch", 1, {"single_time"}, single_time_mismatch),
            ]
        )

        for name, expected_exit, expected_codes, mutation in cases:
            with self.subTest(name=name):
                with tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    run_dir, files_dir = make_valid_fixture(root)
                    mutation(run_dir, files_dir)
                    report = self._verify(root, run_dir, files_dir)
                    codes = {issue["code"] for issue in report.issues}
                    self.assertEqual(
                        report.exit_code,
                        expected_exit,
                        f"{name}: {report.issues}",
                    )
                    self.assertTrue(
                        codes & expected_codes,
                        f"{name}: expected {expected_codes}, got {codes}",
                    )
                    print(
                        f"MUTATION {name}: exit={report.exit_code} "
                        f"codes={','.join(sorted(codes))}"
                    )


if __name__ == "__main__":
    unittest.main()
