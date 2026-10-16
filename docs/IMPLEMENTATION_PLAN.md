# GLogViewer Implementation Plan

**Status:** Version 1.9.0 adds Linux/macOS GUI builds and headless startup checks to the tagged release workflow. Interactive GUI testing on those platforms remains open. The CLI/TUI release gate runs the Windows, Linux, and macOS test matrix, Linux sanitizers, and cross-platform package checks. All project content and code comments follow the English-only policy in [AGENTS.md](../AGENTS.md).

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

- Windows, Linux, and macOS: the project owner reports the complete test suite passing, including platform-specific PTY checks.
- The tag-triggered release workflow runs clean platform builds, Linux sanitizer checks, and release package validation before publication.
- Local sample timings are not formal release benchmark evidence.

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
- The project owner reports the full test suite passing on Windows, Linux, and macOS, including POSIX PTY coverage. Tag-triggered CI provides the sanitizer and package release gates.

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
- Add coverage for timestamp interleaving, deterministic ties, missing timestamps, multi-file CSV labels, statistics, duplicate paths, and descending timestamps. The project owner reports the full test suite passing on Windows, Linux, and macOS; tag-triggered CI provides sanitizer and package release gates.

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
- Windows, Linux, and macOS test suites pass according to the project owner. Tagged CI performs the release sanitizer and package checks before publication.

## Release 1.6: Boolean TUI search

### Scope

- Search quoted literal terms with `AND`, `OR`, `NOT`, and parentheses while preserving regex search for ordinary regex input.
- Define and document operator precedence and case sensitivity; keep evaluation bounded by fixed parser limits.
- Explain regex metacharacter escaping with log-search examples.

### Acceptance criteria

- Boolean terms match literally, including punctuation that would otherwise be a regex operator; an unquoted logical operator selects Boolean mode.
- `AND`, `OR`, `NOT`, grouping, and precedence behave consistently in navigation, highlighting, and export.
- Invalid expressions produce a recoverable TUI error and leave the previous search intact.
- Windows, Linux, and macOS builds and applicable TUI checks pass.

## Release 1.7: Apache Error logs and desktop GUI preview

### Implemented work

- Parse Apache Error records into timestamp, level, message, optional module, and optional client address fields; recognize the format during auto-detection.
- Include timezone-free Apache Error timestamps in statistics as UTC.
- Add an optional Nuklear/SDL3 desktop GUI with incremental scans, search, filters, paged results, record inspection, analysis, export, follow, timers, and a bottom-anchored status panel.
- Support UTF-8 paths for GUI exports on Windows and auto-discover common extracted SDL3 development packages in the Windows build script.
- Keep the default CLI/TUI build independent of SDL3; release archives continue to package the CLI/TUI binary.

### Release evidence

- Windows CLI build and full sample suite pass locally.
- Windows GUI build with SDL3 3.4.18 succeeds; a live 1 GiB scan capture confirms that status, timing, record counts, and cancellation remain visible.
- Linux and macOS GUI builds and interactive validation are deferred; CI release gates cover the CLI/TUI on all three platforms.

## Release 1.8: desktop workflow improvements

### Implemented work

- Load and save the shared versioned query profile format from the GUI, covering parser, field filter, regex, exclusion regex, and context settings.
- Let users show or hide timestamp, level, and source columns while retaining the message column and sizing visible columns to the available table width.
- Persist column visibility, context sizes, export format, and window dimensions in per-user preferences.
- Accept `--syslog-year` and `--syslog-timezone` in the GUI launcher so RFC 3164 timestamps can be interpreted in the histogram with explicit settings.
- Keep the status panel anchored within the window while preserving room for sidebar controls.

### Release evidence

- Windows GUI build with SDL3 3.4.18 succeeds; `--help` and valid RFC 3164 launch options are accepted.
- The GUI remains a source preview and is excluded from CLI/TUI release archives. Linux/macOS GUI builds and interactive checks remain open.
- The tag-triggered release workflow runs the CLI/TUI platform matrix, Linux sanitizers, package checks, and provenance generation.

## Later candidates

Compressed input, multi-line event reconstruction, and persistent indexes are candidates rather than commitments. Evaluate each against user demand, portability, input safety, licensing, implementation cost, and measured benefit. A persistent index must detect file replacement or modification and recover without returning stale results. Automatic log-template discovery remains research work and should not delay the core local investigation workflow.

## Engineering and release rules

- Work in small, reviewable increments; update the user manual, feature catalogue, technical specification, and changelog when behavior changes.
- Add tests for externally visible behavior and regressions. Preserve representative sample scans without requiring sample datasets in release packages.
- Record benchmark hardware, compiler, storage, cache state, and run count. Do not compare timings across machines as if they were controlled results.
- Run clean builds and the full applicable test suite on Windows, Linux, and macOS before publishing a release. Run available sanitizers and package checks.
- Audit all changed and added files for English-only content before commit and release.

## Desktop GUI preview

The optional Nuklear/SDL3 desktop GUI is included in source. Version 1.8 adds query profiles, persistent preferences, configurable columns, and RFC 3164 analysis options. The CLI/TUI builds remain independent of SDL3. Windows GUI builds pass; macOS/Linux GUI verification, visual interaction checks, and packaging remain open. See the [desktop GUI implementation plan](GUI_IMPLEMENTATION_PLAN.md) for current scope and remaining work.
