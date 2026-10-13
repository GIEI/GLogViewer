# GLogViewer Product Roadmap

**Status:** Version 1.6.0 is the current stable release. The tag-triggered workflow runs the Windows, Linux, and macOS test matrix, sanitizer, package, and provenance checks. This roadmap preserves release history and defines the next direction. See the [implementation plan](IMPLEMENTATION_PLAN.md) and [validation notes](VALIDATION.md).

## Product decision

The first MVP solves one complete incident-triage task: read a large local application log, select JSON Lines errors, apply a safe regular expression, and write the original matching records to standard output. A raw-text mode makes the same binary useful even when the source is not structured. The MVP is deliberately a batch CLI; terminal navigation and live monitoring can be built on a verified streaming core.

All releases keep the project language policy in [AGENTS.md](../AGENTS.md): all files and every code comment are written in English.

## Version map

| Release | User-visible outcome | Main additions | Exit gate |
| --- | --- | --- | --- |
| **0.1 MVP** | Filter a large JSON Lines or raw-text file from the command line. | Bounded reader, simple JSON-object parser, equality filter, internal regex, text output, portable builds. | End-to-end CLI scenario and core correctness tests pass on Windows, macOS, and Linux. |
| **0.2 Structured logs** | Investigate syslog, HTTP access, and custom text logs with compound filters. | Presets, placeholder compiler, format detection, full `and`/`or` filter grammar. | Golden format fixtures and predicate precedence tests pass on supported platforms. |
| **0.3 Reports** | Save evidence and quantify an incident. | Text/CSV export, exact counts, bounded timeline and frequent-value estimates. | CSV and statistics tests pass with bounded memory. |
| **0.4 Live monitoring** | Watch append-only logs through rotation and truncation. | Follow mode, partial-line policy, file-identity tracking. | Append/truncate/rotation integration tests pass on all targets. |
| **0.5 Interactive exploration** | Browse, search, highlight, and edit filters in a TUI. | Viewport, input loop, help, bounded checkpoints and row cache. | Manual terminal matrix and responsiveness checks pass. |
| **0.9 Release candidate** | Validate performance, portability, safety, and documentation. | Benchmark harness, fuzz/edge fixtures, CI matrix, packaging drafts, usability fixes. | Every 1.0 requirement has evidence; no release-blocking defect remains. |
| **1.0 Stable** | Deliver a documented cross-platform tool. | Signed/tagged source release and platform binaries where feasible, final manual and changelog. | All 1.0 acceptance gates below pass and artifacts match the documented behavior. |

The numbers identify capability milestones, not calendar dates. A stage is complete only after its exit gate passes; unfinished work remains in that stage rather than being silently deferred.

## Recent milestones and post-1.3 roadmap

Versions 1.2 through 1.6 are complete. Later milestones will evaluate additional formats. Each release is complete only when its exit gate passes on Windows, Linux, and macOS.

| Release | User-visible outcome | Main additions | Exit gate |
| --- | --- | --- | --- |
| **1.2 Structured inspection** | Read and summarize structured records without losing the original evidence. | Pretty-printed JSON; focused field-value statistics; navigable TUI histogram; explicit year and timezone for RFC 3164 timestamp analysis. | Included in 1.5.0; owner reports Windows, Linux, and macOS test suites pass. |
| **1.3 Investigation workflow** | Return to useful evidence and repeat a prior investigation. | TUI source-line bookmarks; versioned query profiles; configurable context before and after a match. | Included in 1.5.0; owner reports Windows, Linux, and macOS test suites pass. |
| **1.4 Multi-file analysis** | Correlate events across related files in chronological order. | Multiple input files, source-file labels, timestamp ordering, explicit handling of records without usable timestamps, aggregated statistics. | Ordering, tie, and missing-timestamp policies are documented and tested; multi-file follow is explicitly deferred; bounded-memory behavior is documented; platform CI passes. |
| **1.5 Container log support** | Filter Kubernetes container logs without a custom pattern. | CRI parser, auto-detection, stream/tag/message fields, RFC 3339 Nano statistics. | Released as 1.5.0; owner reports the full test suite passes on Windows, Linux, and macOS. |
| **1.6 Boolean TUI search** | Combine exact literal terms without escaping regex punctuation. | Quoted terms, `AND`/`OR`/`NOT`, parentheses, documented precedence, regex compatibility. | Released as 1.6.0; CI validates Windows, Linux, and macOS builds and tests. |
| **1.x Format expansion** | Inspect more operational data with the same local workflow. | Compressed input and additional container formats, selected by demand and dependency review. | Format fixtures, malformed-input behavior, portability, license/dependency review, and large-file performance are documented before inclusion. |

Version 1.6.0 is the current stable release and includes Boolean TUI search along with repeatable query profiles, source-line bookmarks, adjustable context, multi-file analysis, and Kubernetes CRI support. See the [implementation plan](IMPLEMENTATION_PLAN.md) and [release changelog](../CHANGELOG.md).

Version 1.2 extends the 1.1 statistics and full-record inspection with focused field summaries, a navigable TUI histogram, and readable JSON presentation. It does not change source records or export content. See [implementation plan](IMPLEMENTATION_PLAN.md) for implementation and acceptance details.

## 0.1 MVP: exact scope

### Included

1. **One local file, two explicit formats.** `--format raw` (default during 0.1) and `--format jsonl`. JSON input supports bounded objects, nested fields, and top-level arrays of formatted objects; malformed objects remain available as raw records but have no parsed fields.
2. **Streaming reader.** LF and CRLF, an unterminated final snapshot line, 64-bit offsets, reusable buffers, a 16 MiB maximum physical line, and deterministic handling of oversized lines. Heap usage must not grow with file size.
3. **One field equality predicate.** `--filter "level=ERROR"`, with the documented missing-field behavior. Canonical JSON aliases for timestamp, level, and message are available. Compound expressions and inequality are deferred to 0.2.
4. **One raw-line and one field regex.** `--regex "timeout"` and `--regex-field "message=timeout"` may be used together and are ANDed with the equality predicate. The internal engine supports the entire planned regular subset—literals, dot, classes and ranges, `\d`/`\w`/`\s`, anchors, `*`/`+`/`?`, grouping, and alternation—through an iterative NFA VM. Parentheses need grouping semantics in the MVP; exposing capture groups beyond the match span is not an MVP user feature. Regex size and work limits apply from the start.
5. **Batch output and stable failures.** Matching original lines go to stdout. `--help` and `--version` work. Exit statuses 0 (match), 1 (no match), 2 (I/O), and 3 (syntax or option error) follow the final CLI contract. Diagnostics go to stderr.
6. **Portable source build.** C99, standard library plus minimal OS wrappers for 64-bit files; a `Makefile` and `build.bat`. A smoke build and test run are required on Windows, macOS, and Linux before declaring the MVP complete.

### MVP command to prove

```text
logview app.jsonl --format jsonl --filter "level=ERROR" --regex "timeout"
```

Given a fixture with INFO, ERROR, malformed JSON, CRLF, and an unterminated final line, this command must print only the raw ERROR records containing `timeout`. A second fixture must prove `--regex-field "message=timeout"` matches the parsed message, not another field. No-match and invalid-regex invocations must return 1 and 3 respectively.

### Excluded from the MVP

Format auto-detection, syslog and HTTP presets, custom placeholders, `and`/`or` expressions, export files and CSV, statistics, follow, TUI, multiple inputs, native journal/Event Log readers, compressed files, and automatic template discovery. These exclusions are staging decisions; features assigned to 0.2–0.5 remain in the 1.0 target.

### MVP engineering gate

- Regex unit tests cover every supported construct, invalid syntax, empty matches, and adversarial expressions such as `(a+)+b` against long nonmatching input. No recursive matching stack or exponential retry is acceptable. This follows the failure mode documented by [Russ Cox](https://swtch.com/~rsc/regexp/regexp1.html).
- Reader and JSON tests cover long lines, line boundaries, malformed escapes, Unicode surrogate pairs, duplicate keys, too many fields, embedded NUL, and allocation failure paths.
- A generated file larger than 1 GiB is scanned without file-size-proportional heap growth. The 64 MiB batch-heap target applies already; the under-five-second throughput target is a 1.0 release gate, measured in 0.9.
- Build commands, test commands, compiler versions, and observed memory use are recorded in a short release note for 0.1.

## 0.2: structured formats and expression language

The implementation includes separate RFC 3164 and RFC 5424 parsers, Apache Common/Combined and NGINX Combined access parsers, custom placeholders, bounded auto-detection with raw fallback, and a compiled equality/inequality/regex expression filter with `and`, `or`, and parentheses. It preserves original records and reports malformed or oversized record counts. Windows/Linux/macOS CI covers parser goldens for the RFC 3164 yearless timestamp, RFC 5424 structured data and malformed data, Apache escaped quotes, custom ambiguity and duplicate aliases, and filter precedence.

**Gate passed:** parser goldens derived from the source standards and vendor documentation pass across the CI platform matrix. All existing 0.1 commands remain valid. [RFC 3164](https://www.rfc-editor.org/info/rfc3164/), [RFC 5424](https://www.rfc-editor.org/info/rfc5424/), [Apache logs](https://httpd.apache.org/docs/2.4/logs.html), [NGINX log module](https://nginx.org/en/docs/http/ngx_http_log_module.html)

## 0.3: export and statistics

The current implementation adds streamed text/CSV export with `--output`, `--columns`, and explicit `--force` overwrite. `--stats` reports exact physical/parsed/rejected/selected record counts and exact per-level counts, a 256-bin timeline over unambiguous timestamps, and bounded Space-Saving estimates for frequent field/value pairs. Statistics describe selected records; estimates are labelled and ambiguous timestamps are counted separately. Timeline bins require a second streaming pass.

**Gate passed:** tests cover text export, CSV quoting, invalid UTF-8 replacement, decoded line breaks, output overwrite protection, exact statistics, ISO timestamps with offsets, Apache timestamps, and frequent-value estimates. Cross-platform builds, sanitizer checks, and the measured memory proxy are recorded in [validation notes](VALIDATION.md).

## 0.4: follow

The implementation adds `--follow`, `--from-start`, and `--flush-partial` for batch output. Follow waits for complete appended lines, detects truncation and file replacement, then restarts from byte zero. It recovers if the rotated path is temporarily missing and later reappears. Rapid rotations between polls can still lose records.

**Gate passed:** Windows and Linux tests cover append, partial-line handling, truncation, default EOF start, and `--from-start`. POSIX tests cover rename/replacement, temporary path absence followed by reopen, and Ctrl-C partial flush. Rapid rotations between polls remain a documented limitation; normal append/truncate/replacement paths are tested for duplicates, and appended lines appear within the polling interval.

## 0.5: terminal interface

The `--tui` implementation provides a 256-row memory cache, temporary checkpoints, navigation and regex search across the full scan, error/warning jumps, context view, scrollable full-record zoom, follow mode with append/partial-line/rotation handling, row highlighting, dynamic filter/raw-regex edits with a rescan, resize handling, help, and terminal restoration. Rows keep at most 1 KiB of display text; search is limited to each row's displayed preview. Unsupported non-interactive streams fail clearly; log control bytes are sanitized.

**Gate passed:** Windows build and non-terminal rejection tests pass. The expanded Linux/macOS PTY scenarios exercise historical search, severity jumps, context, resize, invalid filter edits, bounded previews for long lines, terminal restoration, TUI follow appends, partial records, and rotation. The user has also reported successful interactive TUI use on Windows Terminal and macOS.

## 0.9: release candidate and 1.0 decision

The 1.0 CLI and file-format behavior is frozen. The full test suite and available sanitizers pass; coverage includes high-cardinality statistics, malformed input, long paths on POSIX, invalid UTF-8 CSV output, valid escaped and literal Unicode JSON, surrogate pairs, malformed UTF-8 classes, output failures, and the parser/follow release hardening cases. The Windows 1 GiB reference benchmark is recorded in [validation notes](VALIDATION.md).

The 1.0 release requires:

1. Every RF-01 through RF-10 acceptance result in the technical specification has passing evidence.
2. Windows, macOS, and Linux release builds and smoke tests pass from clean checkouts, without third-party runtime libraries.
3. A normal 1 GiB batch scan stays within the specified 64 MiB application-heap target and the plain unfiltered reference benchmark completes in under five seconds. If the speed gate fails, optimize and repeat the recorded benchmark before 1.0.
4. TUI responsiveness and follow tests meet the 0.4 and 0.5 gates.
5. The README and manual describe the actual binary, installation, examples, known limits, and security behavior; documentation examples are executed in CI where practical.
6. All written project content and all code comments satisfy the English-only policy.

All gates are evidenced by CI run [38154893862](https://github.com/GIEI/GLogViewer/actions/runs/38154893862), the tagged release run [38155296731](https://github.com/GIEI/GLogViewer/actions/runs/38155296731), the benchmark report in [validation notes](VALIDATION.md), and the user's recorded platform testing. Native systemd/Windows event readers, multi-file follow, compression, multiline reconstruction, and automatic template discovery remain candidates for later work.
