#!/usr/bin/env python3
"""Verify release archives contain the executable and user-facing documents."""

import tempfile
import tarfile
import unittest
import zipfile
from pathlib import Path

from package_release import create_package


ROOT = Path(__file__).resolve().parents[1]


class ReleasePackageTests(unittest.TestCase):
    def test_platform_archives_include_required_files(self):
        required_files = {
            "logview",
            "README.md",
            "LICENSE",
            "CHANGELOG.md",
            "docs/USER_MANUAL.md",
        }

        build_directory = ROOT / "build"
        build_directory.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=build_directory) as directory:
            root = Path(directory)
            unix_binary = root / "logview"
            windows_binary = root / "logview.exe"
            unix_binary.write_bytes(b"unix executable fixture")
            windows_binary.write_bytes(b"windows executable fixture")

            for target_platform in ("linux", "macos"):
                archive = create_package(
                    unix_binary, root / target_platform, target_platform, "x86_64"
                )
                with tarfile.open(archive, "r:gz") as package:
                    self.assertEqual(set(package.getnames()), required_files)
                    self.assertNotEqual(package.getmember("logview").mode & 0o111, 0)
                    self.assertEqual(package.extractfile("logview").read(),
                                     b"unix executable fixture")

            archive = create_package(
                windows_binary, root / "windows", "windows", "AMD64"
            )
            expected_windows_files = required_files - {"logview"} | {"logview.exe"}
            with zipfile.ZipFile(archive) as package:
                self.assertEqual(set(package.namelist()), expected_windows_files)
                self.assertEqual(package.read("logview.exe"),
                                 b"windows executable fixture")


if __name__ == "__main__":
    unittest.main()
