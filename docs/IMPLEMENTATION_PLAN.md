# GLogViewer Implementation Plan

**Status:** The 1.5.0 development scope, including Kubernetes CRI parsing and raw-line regex exclusion, is implemented locally on Windows. The full Windows CLI suite passes across all 16 available sample files; Windows packaging checks pass. Linux/macOS pseudo-terminal runs, Linux sanitizer checks, and cross-platform package validation remain publication gates. All project content and code comments follow the English-only policy in [AGENTS.md](../AGENTS.md).

## Product goals

1. Make long structured records understandable in the terminal while preserving a way to inspect the exact source record.
2. Let users move from an aggregate signal, such as an error spike or frequent field value, to the records that produced it.
3. Make useful investigations repeatable without requiring users to reconstruct command-line options.
4. Keep local-file streaming, bounded memory, cross-platform C99 builds, and the no-runtime-dependency design as constraints.

## Current baseline

Version 1.1 provides bounded streaming, JSON and text formats, named parsers, field and range filters, regex, CLI/TUI navigation, full-record zoom, context view, follow, export, exact record/level counts, a batch timeline, and bounded estimates of frequent field values. Version 1.2 adds readable JSON zoom, focused field summaries, a navigable TUI histogram, and explicit year/time-zone configuration for RFC 3164 timestamps. The behavior and limits are documented in the [technical specification](TECHNICAL_SPECIFICATION.md) and [user manual](USER_MANUAL.md).

## Release 1.2: structured inspection

### Implemented work

1. **Define structured display behavior.** Extend the existing full-record zoom to show formatted JSON with indentation and readable nested paths. Keep the original raw record available, do not alter export output, and handle malformed or oversized JSON with a clear fallback.
2. **Make field summaries actionable.** Expose field/value counts and top values through a clear CLI interface and a TUI view. Reuse the bounded heavy-hitter approach for high-cardinality fields; label estimates and include the number of scanned/selected records. Provide exact counts only where the implementation can guarantee them within its memory limits.
3. **Add a TUI time histogram.** Reuse the timestamp normalization and bucketing rules from batch statistics. Allow choosing a bucket to jump to its first matching record, and report missing or ambiguous timestamps rather than silently assigning them.
4. **Validate and document.** Deterministic fixtures, large sample scans, CLI examples, and POSIX pseudo-terminal interaction checks are included. Windows validation has completed locally; Linux/macOS and sanitizer coverage remain to be run by the release matrix.

### Acceptance criteria

- A nested JSON record is readable in the TUI at narrow and wide terminal sizes, and the user can inspect its complete original text.
- Pretty display is a presentation-only operation: filtering, searching, and exports continue to use the source record and parsed fields.
- Frequent field summaries identify exact versus estimated counts and obey a documented memory cap.
- Histogram buckets use documented timestamp/time-zone rules; keyboard navigation lands on a record inside the selected bucket.
- RFC 3164 dates remain unknown unless the user supplies a year; the fixed UTC offset is explicit, validated, and applied consistently in CLI and TUI statistics.
- Large-file scans remain bounded in memory, and the TUI stays responsive while summary or timeline work is in progress.
- Existing CLI commands and exit statuses remain compatible; Windows, Linux, and macOS CI and Linux sanitizers pass.

### Validation record

- Windows: `build.bat` and `python tests/test_cli.py build/logview.exe` pass, including all eight locally available regular files under `SAMPLES`.
- Linux/macOS: the test suite contains PTY checks for field summaries, histograms, JSON zoom, navigation, and terminal restoration, but these checks have not been run on this Windows host.
- Publication remains gated on platform CI, Linux sanitizer, and release package checks. Local sample timings are not release benchmark evidence.

## Release 1.3: repeatable investigations

### Scope

- Add TUI bookmarks that survive ordinary navigation and clearly indicate whether they refer to source line numbers or filtered result positions.
- Add saved query profiles for format, filter, regex, field regex, and range bounds. Define a portable, versioned text format; reject unknown or malformed settings with actionable diagnostics.
- Make context size configurable so users can inspect a useful number of records around a match.

### Acceptance criteria

- Saving, loading, and editing a profile does not silently overwrite an existing file; explicit overwrite behavior is documented.
- A profile can be used in batch mode and the TUI with the same filter semantics.
- Bookmarks remain stable through navigation and are invalidated or remapped explicitly when a rescan changes the result set.
- Context display does not cause unbounded memory growth and does not change the selected/exported record set.
- Existing command-line behavior and all platform CI gates pass.

### Implementation record

- Added session-only bookmarks keyed by physical source line, with next-bookmark navigation and explicit clearing when a filter rescan or file rotation changes the underlying result set.
- Added `--context-before` and `--context-after` (0–100, default 4) and interactive `+`/`-` adjustments.
- Added the versioned `GLOGVIEW_PROFILE 1` text format and `--profile`, `--save-profile`, and explicit `--profile-overwrite` options. Profiles preserve parser and query settings, can be shared between batch and TUI runs, and reject unsupported or malformed values. Explicit CLI settings take precedence.
- Added deterministic CLI tests for profile save/load, override precedence, invalid profiles, and overwrite protection, plus POSIX PTY checks for bookmark navigation and context-size controls.
- Windows build and the complete CLI suite pass. POSIX PTY, sanitizer, and release package gates remain unverified on this host.

## Release 1.4: multi-file analysis

### Scope

- Accept multiple input paths and directories only if directory semantics can be made predictable.
- Display the source file for each record and merge timestamped records chronologically.
- Define deterministic ordering for equal timestamps and a documented placement policy for missing or ambiguous timestamps.
- Specify how follow mode handles multiple files, rotation, truncation, and newly appearing files before implementing it.

### Acceptance criteria

- Tests cover interleaved timestamps, equal timestamps, mixed formats, malformed records, missing timestamps, duplicate paths, and file errors.
- The ordering and timestamp policies are documented and stable across platforms.
- The merge remains bounded in memory; any per-file buffering limit is documented and tested.
- Multi-file follow is not enabled until append and rotation semantics pass platform-specific integration tests.

### Implementation record

- Accept up to 16 input files in one batch invocation. A single selected format applies to every file; `--format auto` detects each file independently. Directories, standard input, duplicate files, TUI, follow, and result-window modes are rejected for multi-file runs.
- Merge supported timestamped records with one selected head record per source. Each source must be nondecreasing by parseable timestamp; a decrease stops the run with a source path and line diagnostic. Equal timestamps follow command-line argument order, then source-line order.
- Emit records without a usable timestamp after all timestamped records, preserving argument order and source-line order. Temporary files keep those records off the heap. Output labels identify source path and source line; CSV always includes a `source_file` column.
- Aggregate `--stats` across all input files. Timestamp values are staged in temporary storage and bucketed after the complete selected timestamp range is known.
- Add coverage for timestamp interleaving, deterministic ties, missing timestamps, multi-file CSV labels, statistics, duplicate paths, and descending timestamps. Windows C99 build and full CLI/sample suite pass locally; POSIX, sanitizer, and package gates remain.

## Release 1.5: Kubernetes CRI logs

### Scope

- Add a dependency-free parser for the CRI line form `<RFC3339Nano timestamp> <stdout|stderr> <tag> <message>`.
- Expose `timestamp`, `stream`, `tag`, and `message` fields for filters, CSV export, and statistics.
- Add CRI to format auto-detection without changing original source lines.
- Keep `P` and `F` fragments as individual source records in this release; automatic multiline reconstruction remains a separately specified feature.
- Add `--exclude-regex` for dropping noisy raw records while retaining the existing include regex and field-filter workflow.

### Acceptance criteria

- Valid timestamps with fractional seconds and UTC offsets are parsed; calendar, stream, and timestamp-shape errors are rejected and reported.
- Filters can select stream and tag values; original records remain byte-for-byte available to output.
- Auto-detection and statistics recognize representative CRI files.
- Exclusion patterns apply consistently to batch output, exports, statistics, follow, TUI navigation, and saved query profiles.
- Existing parsers and single-/multi-file workflows pass the Windows, Linux, and macOS test matrix.

### Implementation record

- Implement the `cri` preset and include it in `auto` format scoring.
- Add fixtures for fractional timestamps, `stdout`/`stderr`, `P`/`F`, field filtering, timeline stats, invalid dates, and invalid stream names.
- Windows build, full CLI/sample suite, and package checks pass locally; POSIX, sanitizer, and cross-platform package gates remain before publication.

## Later candidates

Compressed input, multi-line event reconstruction, and persistent indexes are candidates rather than commitments. Evaluate each against user demand, portability, input safety, licensing, implementation cost, and measured benefit. A persistent index must detect file replacement or modification and recover without returning stale results. Automatic log-template discovery remains research work and should not delay the core local investigation workflow.

## Engineering and release rules

- Work in small, reviewable increments; update the user manual, feature catalogue, technical specification, and changelog when behavior changes.
- Add tests for externally visible behavior and regressions. Preserve representative sample scans without requiring sample datasets in release packages.
- Record benchmark hardware, compiler, storage, cache state, and run count. Do not compare timings across machines as if they were controlled results.
- Run clean builds and the full applicable test suite on Windows, Linux, and macOS before publishing a release. Run available sanitizers and package checks.
- Audit all changed and added files for English-only content before commit and release.
