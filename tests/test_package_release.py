#!/usr/bin/env python3
"""Verify CLI and GUI release archives contain their required files."""

import os
import subprocess
import tempfile
import tarfile
import unittest
import zipfile
from pathlib import Path

from package_release import create_gui_package, create_package


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

    def test_gui_archives_bundle_sdl_runtime_and_launcher(self):
        build_directory = ROOT / "build"
        build_directory.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=build_directory) as directory:
            root = Path(directory)
            fixtures = {
                "windows": ("logview-gui.exe", "SDL3.dll", "zip"),
                "linux": ("logview-gui", "libSDL3.so.0", "tar.gz"),
                "macos": ("logview-gui", "libSDL3.dylib", "tar.gz"),
            }
            for target_platform, (binary_name, runtime_name, extension) in fixtures.items():
                gui_binary = root / (target_platform + "-gui")
                runtime_library = root / (target_platform + "-runtime")
                gui_binary.write_bytes((target_platform + " GUI fixture").encode())
                runtime_library.write_bytes((target_platform + " SDL3 fixture").encode())
                archive = create_gui_package(
                    gui_binary, runtime_library, root / target_platform,
                    target_platform, "AMD64" if target_platform != "macos" else "arm64"
                )
                expected_suffixes = [".tar", ".gz"] if extension == "tar.gz" else [".zip"]
                self.assertEqual(archive.suffixes, expected_suffixes)

                if target_platform == "windows":
                    with zipfile.ZipFile(archive) as package:
                        names = set(package.namelist())
                        self.assertIn(binary_name, names)
                        self.assertIn(runtime_name, names)
                        self.assertIn("third_party/SDL3-LICENSE.txt", names)
                        self.assertEqual(package.read(binary_name), b"windows GUI fixture")
                        self.assertEqual(package.read(runtime_name), b"windows SDL3 fixture")
                else:
                    with tarfile.open(archive, "r:gz") as package:
                        names = set(package.getnames())
                        self.assertIn(binary_name, names)
                        self.assertIn("lib/" + runtime_name, names)
                        self.assertIn("run-logview-gui.sh", names)
                        self.assertIn("third_party/SDL3-LICENSE.txt", names)
                        self.assertNotEqual(package.getmember(binary_name).mode & 0o111, 0)
                        self.assertNotEqual(package.getmember("run-logview-gui.sh").mode & 0o111, 0)
                        launcher = package.extractfile("run-logview-gui.sh").read().decode()
                        self.assertIn("exec \"$script_dir/logview-gui\" \"$@\"", launcher)
                        library_variable = (
                            "DYLD_LIBRARY_PATH" if target_platform == "macos"
                            else "LD_LIBRARY_PATH"
                        )
                        self.assertIn(
                            f'{library_variable}="$script_dir/lib${{{library_variable}:+:${library_variable}}}"',
                            launcher,
                        )
                        if os.name != "nt":
                            launcher_path = root / target_platform / "run-logview-gui.sh"
                            launcher_path.write_text(launcher, encoding="utf-8")
                            subprocess.run(["sh", "-n", str(launcher_path)], check=True)
                        self.assertIn(
                            "SDL3 fixture",
                            package.extractfile("lib/" + runtime_name).read().decode(),
                        )


if __name__ == "__main__":
    unittest.main()
