# GLogViewer Changelog

## 1.5.0 — 2026-10-12

Adds native parsing for Kubernetes CRI container log files.

### Included

- Add the `cri` format for RFC 3339 Nano timestamp, `stdout`/`stderr` stream, tag, and message fields.
- Detect CRI records with `--format auto` and include timestamps in sorting and statistics.
- Add `--exclude-regex` to omit matching raw records in batch, export, statistics, follow, and TUI modes; save it in query profiles.
- Keep each physical CRI line intact; expose `P` and `F` tags for filtering without silently joining partial fragments.
- Test explicit parsing, field filters, auto-detection, timestamp statistics, invalid dates, and invalid streams.

### Validation status

- The project owner verified the complete test suite on Windows, Linux, and macOS.
- The tag-triggered release workflow runs the platform build/test matrix, Linux sanitizers, archive checks, and provenance generation before publishing the GitHub release.

## 1.4.0 (development)

Adds batch correlation across multiple timestamped files.

### Included

- Accept up to 16 input files and merge parseable timestamps in chronological order.
- Label text output with source path and source line; always include `source_file` in multi-file CSV exports.
- Define equal-timestamp ordering by command-line file order and source-line order.
- Place records without usable timestamps after timestamped records in stable file and line order.
- Reject duplicate files and timestamp-decreasing sources; allow independent format detection with `--format auto`.
- Aggregate multi-file statistics and add coverage for merging, labels, missing timestamps, ties, duplicates, and invalid source order.

### Validation status

- Windows C99 build and complete CLI/sample suite pass locally.
- Linux/macOS, sanitizer, and package release gates remain required before publication.

## 1.3.0 (development)

Adds repeatable query workflows and faster return to records of interest.

### Included

- Add versioned `GLOGVIEW_PROFILE 1` query profiles that can be loaded in batch or TUI mode; explicit CLI values take precedence.
- Refuse profile overwrite unless `--profile-overwrite` is supplied; reject unknown, duplicate, malformed, or unsupported profile settings.
- Add TUI source-line bookmarks with next-bookmark navigation and explicit clearing when filters or file identity changes.
- Add `--context-before` and `--context-after` plus interactive `+`/`-` controls for physical context size.
- Add CLI profile, override, invalid-profile, bookmark, and context coverage.

### Validation status

- Windows C99 build and complete CLI suite pass, including all regular files under `SAMPLES`.
- Linux/macOS pseudo-terminal tests, Linux sanitizer checks, and release package checks remain required before publication.

## 1.2.0 (development)

Adds readable structured-record inspection and ways to navigate from aggregate statistics to matching data.

### Included

- Pretty-print valid JSON in the TUI full-record view, with a bounded output size and raw-record fallback.
- Add `--stats-field FIELD` to focus frequent-value statistics on one parsed field.
- Mark frequent-value counts as exact when their error bound is zero and estimated otherwise.
- Add the TUI `v` field-value summary and `h` timestamp histogram, including navigation to the first record in a selected bucket.
- Add `--syslog-year` and `--syslog-timezone` so RFC 3164 timestamps can be included in CLI statistics and the TUI histogram with an explicit year and assumed fixed offset.
- Add deterministic CLI and POSIX pseudo-terminal coverage for the new behavior.

### Validation status

- Windows C99 build and complete CLI test suite pass, including all available files under `SAMPLES`.
- Linux and macOS pseudo-terminal tests and Linux sanitizer checks remain required release gates.

## 1.1.0

Adds date and value range filtering, bounded result windows, and full-record inspection from the command line.

### Included

- Add lexical field comparisons with `<`, `<=`, `>`, and `>=`, suitable for fixed-format date and timestamp values.
- Add `--top N` and `--bottom N` in batch mode and the TUI to inspect a finite result window.
- Add `--record first|last` to print the complete first or last matching record, including multiline JSON records.
- Disable source-line jump and TUI export when using a limited result window, keeping those actions consistent with the visible subset.
- Expand CLI, sample-based date-range, and POSIX TUI test coverage; document the new commands and limitations.

### Validation

- Windows build and complete CLI suite pass, including the six-record date interval from the crypto sample in `SAMPLES`.
- POSIX TUI tests cover top and bottom result windows in a pseudo-terminal; the release workflow gates publication on Linux/macOS suites and Linux sanitizers.

## 1.0.0

First stable release.

### Included

- Cross-platform C99 CLI and terminal interface for streaming large local log files.
- Raw text, JSON Lines and formatted JSON arrays, syslog RFC 3164/5424, Apache/NGINX access logs, and custom patterns.
- Field predicates, bounded regular-expression matching, interactive navigation, search, source-line jump, full-record zoom, context, and follow mode.
- Text/CSV export and record, level, timeline, and estimated frequent-value statistics.
- Windows, Linux, and macOS release archives with SHA-256 checksums and signed GitHub build-provenance attestations.

### Validation

- Windows, Linux, and macOS builds, CLI suites, and release archive checks pass in GitHub Actions.
- Linux AddressSanitizer and UndefinedBehaviorSanitizer checks pass.
- The 1 GiB unfiltered Windows reference benchmark passes the five-second and 64 MiB RSS gates; see [validation notes](docs/VALIDATION.md).
- Parser goldens cover syslog timestamps and structured data, Apache quoted fields, malformed records, and custom-pattern validation. Follow tests cover append, partial lines, truncation, rotation, and recovery after a temporarily missing rotated path.

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
