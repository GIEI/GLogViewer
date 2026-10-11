#!/usr/bin/env python3
"""Create a self-contained GLogViewer release archive."""

import argparse
import platform
import tarfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PACKAGE_FILES = (
    "README.md",
    "LICENSE",
    "CHANGELOG.md",
    "docs/USER_MANUAL.md",
)


def normalize_architecture(value):
    value = value.lower()
    if value in ("amd64", "x86_64", "x64"):
        return "x86_64"
    if value in ("aarch64", "arm64"):
        return "arm64"
    raise ValueError("Unsupported release architecture: " + value)


def create_package(binary, output_directory, target_platform, architecture):
    binary = Path(binary).resolve()
    if not binary.is_file():
        raise FileNotFoundError("Release binary does not exist: " + str(binary))

    output_directory = Path(output_directory).resolve()
    output_directory.mkdir(parents=True, exist_ok=True)
    architecture = normalize_architecture(architecture)
    executable_name = "logview.exe" if target_platform == "windows" else "logview"
    extension = "zip" if target_platform == "windows" else "tar.gz"
    archive = output_directory / (
        "logview-{}-{}.{}".format(target_platform, architecture, extension)
    )

    files = [(binary, executable_name)]
    files.extend((ROOT / name, name) for name in PACKAGE_FILES)
    for source, _ in files:
        if not source.is_file():
            raise FileNotFoundError("Release package file does not exist: " + str(source))

    if target_platform == "windows":
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as package:
            for source, name in files:
                package.write(source, name)
    else:
        with tarfile.open(archive, "w:gz") as package:
            for source, name in files:
                metadata = package.gettarinfo(str(source), arcname=name)
                if name == executable_name:
                    metadata.mode |= 0o111
                with source.open("rb") as content:
                    package.addfile(metadata, content)

    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", help="Path to the compiled logview executable")
    parser.add_argument("--platform", choices=("windows", "linux", "macos"), required=True)
    parser.add_argument("--architecture", default=platform.machine())
    parser.add_argument("--output-directory", default="dist")
    arguments = parser.parse_args()

    archive = create_package(
        arguments.binary,
        arguments.output_directory,
        arguments.platform,
        arguments.architecture,
    )
    print("Created {}".format(archive))


if __name__ == "__main__":
    main()
