"""Acceptance-rule tests for the P5-05 synthetic integration driver."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest


VERIFIER_ROOT = Path(__file__).resolve().parent
INTEGRATION_ROOT = Path(__file__).resolve().parents[2] / "integration"
for module_root in (VERIFIER_ROOT, INTEGRATION_ROOT):
    if str(module_root) not in sys.path:
        sys.path.insert(0, str(module_root))

from persistence.p4_software_integration import _utc_synthetic_limitation


class SyntheticUtcLimitationTests(unittest.TestCase):
    def test_anchored_mapping_codes_are_the_only_accepted_limitation(self) -> None:
        accepted, limitation, other = _utc_synthetic_limitation(
            "FAIL",
            ["utc_host_mapping", "utc_mapping_evidence"],
        )
        self.assertTrue(accepted)
        self.assertEqual(
            limitation,
            ["utc_host_mapping", "utc_mapping_evidence"],
        )
        self.assertEqual(other, [])

    def test_anchor_evidence_limitation_is_accepted(self) -> None:
        accepted, limitation, other = _utc_synthetic_limitation(
            "INCONCLUSIVE",
            ["utc_anchor_evidence"],
        )
        self.assertTrue(accepted)
        self.assertEqual(limitation, ["utc_anchor_evidence"])
        self.assertEqual(other, [])

    def test_other_failing_codes_are_not_accepted(self) -> None:
        accepted, limitation, other = _utc_synthetic_limitation(
            "FAIL",
            ["utc_host_mapping", "utc_step", "conservation"],
        )
        self.assertFalse(accepted)
        self.assertEqual(limitation, ["utc_host_mapping"])
        self.assertEqual(other, ["conservation", "utc_step"])

    def test_passing_utc_check_is_not_a_limitation(self) -> None:
        accepted, limitation, other = _utc_synthetic_limitation("PASS", [])
        self.assertFalse(accepted)
        self.assertEqual(limitation, [])
        self.assertEqual(other, [])


if __name__ == "__main__":
    unittest.main()
