"""Artificial values only; Git fixtures are created outside the repository."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from check_repository import PUBLIC_EMAIL, PUBLIC_NAME, blob_findings, check_identity, scan_history, scan_staged, text_findings


class ContentChecks(unittest.TestCase):
    def test_public_identity_and_fictional_email_are_allowed(self):
        self.assertEqual(text_findings(f"{PUBLIC_NAME} <{PUBLIC_EMAIL}> sample@example.test"), [])
        self.assertEqual(text_findings(f"`{PUBLIC_EMAIL}` '{PUBLIC_EMAIL}'"), [])
        self.assertEqual(blob_findings(".githooks/commit-msg", b"#!/bin/sh\n", "100755"), [])

    def test_synthetic_credentials_and_private_paths_are_detected(self):
        synthetic = [
            "gh" + "p_" + "A" * 30,
            "AK" + "IA" + "A" * 16,
            "sk" + "-" + "A" * 30,
            "-----BEGIN " + "PRIVATE KEY-----",
            "password = " + chr(34) + "sample-value-only" + chr(34),
            "/" + "home" + "/" + "sample-user" + "/notes",
            "sample-user" + "@" + "example.invalid",
            "https://" + "sample:sample" + "@example.test",
        ]
        for text in synthetic:
            with self.subTest(category="synthetic value"):
                self.assertTrue(text_findings(text))

    def test_dangerous_files_and_binary_are_blocked(self):
        for name, data, mode in [(".env", b"sample", "100644"), ("private.key", b"sample", "100644"), ("work.mlex-project.json", b"{}", "100644"), ("photo.png", b"\0data", "100644"), ("link.md", b"target", "120000"), ("src.cpp", b"\xff", "100644")]:
            with self.subTest(name=name):
                self.assertTrue(blob_findings(name, data, mode))


class GitContentChecks(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="mlex-git-")
        self.repo = Path(self.directory.name)
        self.environment = dict(os.environ)
        for key in list(self.environment):
            if key.startswith("GIT_"):
                self.environment.pop(key)
        self.environment["GIT_CONFIG_NOSYSTEM"] = "1"
        self.environment["GIT_CONFIG_GLOBAL"] = os.devnull
        self.run_git("init", "--quiet", "--initial-branch=main")
        self.run_git("config", "user.name", PUBLIC_NAME)
        self.run_git("config", "user.email", PUBLIC_EMAIL)
        self.run_git("config", "commit.gpgsign", "false")
        self.run_git("config", "core.hooksPath", str(self.repo / "empty-hooks"))
        self.file = self.repo / "README.md"

    def tearDown(self):
        self.directory.cleanup()

    def run_git(self, *args):
        return subprocess.run(["git", "-C", str(self.repo), *args], check=True, capture_output=True, env=self.environment)

    def test_index_is_checked_even_when_worktree_was_cleaned(self):
        self.file.write_text("gh" + "p_" + "A" * 30)
        self.run_git("add", "README.md")
        self.file.write_text("# Public sample\n")
        findings, count = scan_staged(self.repo)
        self.assertEqual(count, 1)
        self.assertTrue(findings)
        self.run_git("add", "README.md")
        self.assertEqual(scan_staged(self.repo)[0], [])

    def test_removed_secret_remains_detectable_in_history(self):
        self.file.write_text("gh" + "p_" + "B" * 30)
        self.run_git("add", "README.md")
        self.run_git("commit", "--quiet", "-m", "Synthetic test fixture")
        self.file.write_text("# Public sample\n")
        self.run_git("add", "README.md")
        self.run_git("commit", "--quiet", "-m", "Remove synthetic fixture")
        self.assertEqual(scan_staged(self.repo)[0], [])
        self.assertTrue(scan_history(self.repo)[0])

    def test_private_commit_message_and_identity_are_detected(self):
        self.assertEqual(check_identity(self.repo), [])
        self.file.write_text("# Public sample\n")
        self.run_git("add", "README.md")
        message = "sample-user" + "@" + "example.invalid"
        self.run_git("commit", "--quiet", "-m", message)
        self.assertTrue(scan_history(self.repo)[0])
        self.run_git("config", "user.name", "sample-other")
        self.assertTrue(check_identity(self.repo))

    def test_tagged_non_commit_object_is_blocked(self):
        self.file.write_text("# Public sample\n")
        self.run_git("add", "README.md")
        self.run_git("commit", "--quiet", "-m", "Public sample")
        blob = self.run_git("rev-parse", "HEAD:README.md").stdout.decode().strip()
        self.run_git("tag", "sample-blob", blob)
        self.assertTrue(scan_history(self.repo)[0])

    def test_unreferenced_annotated_tag_is_checked_when_pushed(self):
        self.file.write_text("# Public sample\n")
        self.run_git("add", "README.md")
        self.run_git("commit", "--quiet", "-m", "Public sample")
        self.run_git("tag", "-a", "sample-tag", "-m", "gh" + "p_" + "C" * 30)
        tip = self.run_git("rev-parse", "sample-tag").stdout.decode().strip()
        self.run_git("tag", "-d", "sample-tag")
        self.assertEqual(scan_history(self.repo)[0], [])
        self.assertTrue(scan_history(self.repo, [tip])[0])


if __name__ == "__main__":
    unittest.main()
