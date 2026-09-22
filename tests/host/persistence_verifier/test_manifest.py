from __future__ import annotations

import tempfile
from pathlib import Path
import unittest

from fixture_factory import valid_identity
from persistence.manifest import (
    ManifestError,
    RunArtifacts,
    validate_identity,
)


class ManifestTests(unittest.TestCase):
    def test_valid_identity_passes(self) -> None:
        self.assertEqual(validate_identity(valid_identity())["contract"]["csv_schema"], 2)

    def test_missing_candidate_identity_is_rejected(self) -> None:
        identity = valid_identity()
        del identity["candidate_sha"]
        del identity["candidate_diff_sha256"]
        with self.assertRaises(ManifestError):
            validate_identity(identity)

    def test_output_directory_is_exclusive(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "run"
            artifacts = RunArtifacts.paths(output)
            artifacts.create({"format": "test"})
            with self.assertRaises(ManifestError):
                RunArtifacts.paths(output).create({"format": "test"})

    def test_missing_formal_contract_field_is_rejected(self) -> None:
        identity = valid_identity()
        identity["contract"].pop("protocol_version")
        with self.assertRaises(ManifestError):
            validate_identity(identity)


if __name__ == "__main__":
    unittest.main()
