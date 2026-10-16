#!/usr/bin/env python3
"""Create GLogViewer CLI and desktop GUI release archives."""

import argparse
import platform
import tarfile
import zipfile
from io import BytesIO
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PACKAGE_FILES = (
    "README.md",
    "LICENSE",
    "CHANGELOG.md",
    "docs/USER_MANUAL.md",
)
GUI_PACKAGE_FILES = PACKAGE_FILES + ("third_party/SDL3-LICENSE.txt",)


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


def create_gui_package(gui_binary, runtime_library, output_directory,
                       target_platform, architecture):
    gui_binary = Path(gui_binary).resolve()
    runtime_library = Path(runtime_library).resolve()
    if not gui_binary.is_file():
        raise FileNotFoundError("GUI binary does not exist: " + str(gui_binary))
    if not runtime_library.is_file():
        raise FileNotFoundError("GUI runtime library does not exist: " + str(runtime_library))

    output_directory = Path(output_directory).resolve()
    output_directory.mkdir(parents=True, exist_ok=True)
    architecture = normalize_architecture(architecture)
    windows = target_platform == "windows"
    extension = "zip" if windows else "tar.gz"
    archive = output_directory / (
        "logview-gui-{}-{}.{}".format(target_platform, architecture, extension)
    )
    gui_name = "logview-gui.exe" if windows else "logview-gui"
    files = [(gui_binary, gui_name)]
    if target_platform == "windows":
        files.append((runtime_library, "SDL3.dll"))
    elif target_platform == "linux":
        files.append((runtime_library, "lib/libSDL3.so.0"))
        files.append((None, "run-logview-gui.sh"))
    else:
        files.append((runtime_library, "lib/libSDL3.dylib"))
        files.append((None, "run-logview-gui.sh"))
    files.extend((ROOT / name, name) for name in GUI_PACKAGE_FILES)

    for source, _ in files:
        if source is not None and not source.is_file():
            raise FileNotFoundError("GUI package file does not exist: " + str(source))

    launcher = None
    if not windows:
        library_path = "DYLD_LIBRARY_PATH" if target_platform == "macos" else "LD_LIBRARY_PATH"
        launcher = (
            "#!/bin/sh\n"
            "set -eu\n"
            'script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
            + f'{library_path}="$script_dir/lib${{{library_path}:+:${library_path}}}"\n'
            + f"export {library_path}\n"
            + f'exec "$script_dir/{gui_name}" "$@"\n'
        )

    if windows:
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as package:
            for source, name in files:
                package.write(source, name)
    else:
        with tarfile.open(archive, "w:gz") as package:
            for source, name in files:
                if source is None:
                    content = launcher.encode("utf-8")
                    metadata = tarfile.TarInfo(name)
                    metadata.size = len(content)
                    metadata.mode = 0o755
                    package.addfile(metadata, BytesIO(content))
                    continue
                metadata = package.gettarinfo(str(source), arcname=name)
                if name == gui_name:
                    metadata.mode |= 0o111
                with source.open("rb") as content:
                    package.addfile(metadata, content)

    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", help="Path to the compiled logview executable")
    parser.add_argument("--platform", choices=("windows", "linux", "macos"), required=True)
    parser.add_argument("--architecture", default=platform.machine())
    parser.add_argument("--output-directory", default="dist")
    parser.add_argument("--gui-binary", help="Optional GUI executable to package separately")
    parser.add_argument("--gui-runtime", help="SDL3 runtime library or DLL for the GUI package")
    parser.add_argument("--gui-only", action="store_true",
                        help="Create only the GUI archive")
    arguments = parser.parse_args()

    if arguments.gui_binary or arguments.gui_runtime:
        if not arguments.gui_binary or not arguments.gui_runtime:
            parser.error("--gui-binary and --gui-runtime must be used together")
    if arguments.gui_only and not arguments.gui_binary:
        parser.error("--gui-only requires --gui-binary and --gui-runtime")

    if not arguments.gui_only:
        if not arguments.binary:
            parser.error("a CLI binary path is required unless --gui-only is set")
        archive = create_package(
            arguments.binary,
            arguments.output_directory,
            arguments.platform,
            arguments.architecture,
        )
        print("Created {}".format(archive))
    if arguments.gui_binary:
        gui_archive = create_gui_package(
            arguments.gui_binary,
            arguments.gui_runtime,
            arguments.output_directory,
            arguments.platform,
            arguments.architecture,
        )
        print("Created {}".format(gui_archive))


if __name__ == "__main__":
    main()
