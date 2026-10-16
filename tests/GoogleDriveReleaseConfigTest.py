#!/usr/bin/env python3
"""Release credential gates with synthetic fixtures; no Google/network access."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GoogleDriveReleaseConfigTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="explorer-oauth-config-test-")
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        (self.source / "src/data").mkdir(parents=True)
        shutil.copy2(ROOT / "src/data/GoogleDriveBuildConfig.h.in", self.source / "src/data/GoogleDriveBuildConfig.h.in")
        (self.source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.19)\nproject(ConfigGate NONE)\n'
            + 'include("' + str(ROOT / "cmake/GoogleDriveOAuth.cmake") + '")\n')
        self.client = self.root / "synthetic-client.json"
        self.client.write_text(json.dumps({"installed": {
            "client_id": "123-synthetic.apps.googleusercontent.com",
            "client_secret": "synthetic-client-value"}}))

    def tearDown(self):
        self.temporary.cleanup()

    def configure(self, client=None, required=False):
        arguments = ["cmake", "-S", str(self.source), "-B", str(self.root / "build"),
                     "-DEXPLORER_REQUIRE_GOOGLE_DRIVE_CLIENT=" + ("ON" if required else "OFF")]
        if client is not None:
            arguments.append("-DEXPLORER_GOOGLE_DRIVE_CLIENT_JSON=" + str(client))
        return subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, timeout=20)

    def test_unconfigured_development_build_is_explicitly_unavailable(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout)
        header = (self.root / "build/generated/GoogleDriveBuildConfig.h").read_text()
        self.assertIn('ClientId[] = ""', header)
        self.assertIn('ClientSecret[] = ""', header)

    def test_enabled_release_rejects_missing_client(self):
        result = self.configure(required=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("maintainer-owned Desktop OAuth client", result.stdout)

    def test_valid_external_desktop_client_is_bundled_without_values_in_logs(self):
        result = self.configure(self.client, required=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        header = (self.root / "build/generated/GoogleDriveBuildConfig.h").read_text()
        self.assertIn("123-synthetic.apps.googleusercontent.com", header)
        self.assertIn("synthetic-client-value", header)
        self.assertNotIn("synthetic-client-value", result.stdout)
        self.assertNotIn("123-synthetic.apps.googleusercontent.com", result.stdout)

    def test_web_service_account_and_injected_values_are_rejected(self):
        documents = [
            {"web": {"client_id": "123.apps.googleusercontent.com", "client_secret": "fake"}},
            {"type": "service_account"},
            {"installed": {"client_id": "invalid", "client_secret": "fake"}},
            {"installed": {"client_id": "123.apps.googleusercontent.com", "client_secret": ""}},
            {"installed": {"client_id": "123.apps.googleusercontent.com", "client_secret": 'fake"; injected'}},
            {"installed": {"client_id": "123.apps.googleusercontent.com", "client_secret": "fake\nvalue"}},
        ]
        for document in documents:
            with self.subTest(document=document):
                self.client.write_text(json.dumps(document))
                result = self.configure(self.client, required=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn('fake"; injected', result.stdout)

    def test_missing_malformed_and_oversized_files_are_rejected(self):
        self.assertNotEqual(self.configure(self.root / "missing.json").returncode, 0)
        for content in ("not JSON", "x" * 65537):
            self.client.write_text(content)
            self.assertNotEqual(self.configure(self.client).returncode, 0)

    def test_client_in_source_or_symlinked_into_source_is_rejected(self):
        inside = self.source / "client.json"
        shutil.copy2(self.client, inside)
        self.assertNotEqual(self.configure(inside).returncode, 0)
        self.client.unlink()
        self.client.symlink_to(inside)
        self.assertNotEqual(self.configure(self.client).returncode, 0)


if __name__ == "__main__":
    unittest.main()
