# GLogViewer Changelog

## 0.5.0-alpha.2

Release process validation pre-release.

### Included

- Automated Windows, Linux, and macOS release archives containing the executable and user documentation.
- SHA-256 checksums for release assets and automatic prerelease labeling for version tags with a suffix.
- Expanded Unicode validation for JSON escapes, UTF-8 input, surrogate pairs, and CSV output.

### Validation

- Windows, Linux, and macOS CI suites pass, including release archive checks.
- Linux AddressSanitizer and UndefinedBehaviorSanitizer checks pass.
- Windows 1 GiB warm-cache benchmark median: 0.679 seconds; peak RSS: 4.14 MiB.
- The tagged release workflow published Windows, Linux, and macOS archives with a SHA-256 manifest.

## 0.5.0-alpha.1

First public pre-release of GLogViewer.

### Included

- Streaming CLI support for raw text, JSON Lines and formatted JSON object arrays, syslog RFC 3164/5424, Apache/Nginx access logs, and custom patterns.
- Field filters with equality, inequality, regex, `and`, `or`, and parentheses, plus whole-record and field-specific regex search.
- Text and CSV export, exact record and level counts, timestamp timelines, estimated frequent field values, and follow mode with truncation and rotation handling.
- Terminal UI with bounded rendering, search and navigation, source-line jump, severity jumps, context lines, full-record zoom, interactive filter editing, export, and live follow.
- Dependency-free C implementation with Windows batch build and Unix Makefile.

### Build and test

Windows (PowerShell, Visual Studio C++ Build Tools, and Python 3):

```powershell
.\build.bat
python tests\test_cli.py build\logview.exe
```

Linux and macOS (a C compiler, Make, and Python 3):

```sh
make
make test
```

### Pre-release status

This is an alpha release. The CLI test suite has been reported passing on Windows, Linux, and macOS. The formal release benchmark, broader platform coverage, and final release hardening remain planned before version 1.0. Performance varies with storage, cache state, operating system, and compiler; sample timings are not a formal benchmark.

No sample datasets are included in the release package. Build and test instructions run against any regular files that a user places under `SAMPLES`.
