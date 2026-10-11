# Roadmap to Version 1.0

**Status:** release plan, 2026-10-11. The 0.1–0.5 milestones are implemented. The CLI suite is reported passing on Windows, Linux, and macOS; the expanded Linux PTY suite has passed. The GitHub Actions matrix passes on Windows, Linux, and macOS, and the Linux sanitizer job passes. The formal 1 GiB warm-cache benchmark passed on the Windows reference machine. CI now packages all three platforms and is configured to publish validated archives and checksums for version tags; the release workflow still needs a tagged end-to-end run. Broader release hardening remains. See [validation notes](VALIDATION.md). This roadmap stages the [1.0 technical contract](TECHNICAL_SPECIFICATION.md).

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

The current implementation includes separate RFC 3164 and RFC 5424 parsers, Apache Common/Combined and NGINX Combined access parsers, custom placeholders, bounded auto-detection with raw fallback, and a compiled equality/inequality/regex expression filter with `and`, `or`, and parentheses. It preserves original records and reports malformed or oversized record counts. Remaining 0.2 work is broader golden and malformed-input coverage, validation on other platforms, and closing the exit gate.

**Gate:** parser goldens derived from the source standards and vendor documentation pass, including RFC 3164's missing year, RFC 5424 structured data, quoted HTTP fields, malformed lines, ambiguous custom patterns, and filter precedence. All existing 0.1 commands remain valid. [RFC 3164](https://www.rfc-editor.org/info/rfc3164/), [RFC 5424](https://www.rfc-editor.org/info/rfc5424/), [Apache logs](https://httpd.apache.org/docs/2.4/logs.html), [NGINX log module](https://nginx.org/en/docs/http/ngx_http_log_module.html)

## 0.3: export and statistics

The current implementation adds streamed text/CSV export with `--output`, `--columns`, and explicit `--force` overwrite. `--stats` reports exact physical/parsed/rejected/selected record counts and exact per-level counts, a 256-bin timeline over unambiguous timestamps, and bounded Space-Saving estimates for frequent field/value pairs. Statistics describe selected records; estimates are labelled and ambiguous timestamps are counted separately. Timeline bins require a second streaming pass.

**Gate:** implemented tests cover text export, CSV quoting, invalid UTF-8 replacement, decoded line breaks, output overwrite protection, exact statistics, ISO timestamps with offsets, Apache timestamps, and frequent-value estimates. Follow-up release hardening remains in the 0.9 validation stage, including cross-platform builds and a measured peak-memory check.

## 0.4: follow

The implementation adds `--follow`, `--from-start`, and `--flush-partial` for batch output. Follow waits for complete appended lines, detects truncation and file replacement, then restarts from byte zero. Rapid rotations between polls can still lose records.

**Gate:** Windows and Linux tests cover append, partial-line handling, truncation, default EOF start, and `--from-start`. POSIX tests also cover rename/replacement and Ctrl-C partial flush. Reopen/error handling and rapid-rotation races remain release-hardening cases. No duplicate complete record should be emitted in normal append/truncate/replacement paths; new lines should appear within one second.

## 0.5: terminal interface

The `--tui` implementation provides a 256-row memory cache, temporary checkpoints, navigation and regex search across the full scan, error/warning jumps, context view, scrollable full-record zoom, follow mode with append/partial-line/rotation handling, row highlighting, dynamic filter/raw-regex edits with a rescan, resize handling, help, and terminal restoration. Rows keep at most 1 KiB of display text; search is limited to each row's displayed preview. Broader terminal behavior remains. Unsupported non-interactive streams fail clearly; log control bytes are sanitized.

**Gate:** Windows build and non-terminal rejection tests pass. The expanded Linux PTY scenario passes and exercises historical search, severity jumps, context, resize, invalid filter edits, bounded previews for long lines, terminal restoration, TUI follow appends, partial records, and rotation. User-reported manual checks cover Windows Terminal and macOS; a repeatable terminal matrix remains part of release validation.

## 0.9: release candidate and 1.0 decision

Freeze the 1.0 CLI and file-format behavior. Run the full test suite and sanitizer or equivalent checks available per platform; the suite now covers high-cardinality statistics, malformed input, long paths on POSIX, invalid UTF-8 CSV output, and output failures. The Linux sanitizer job passes. Add broader Unicode checks. The Windows 1 GiB reference benchmark is recorded in [validation notes](VALIDATION.md); repeat it when the reference hardware or release build changes.

The 1.0 release requires:

1. Every RF-01 through RF-10 acceptance result in the technical specification has passing evidence.
2. Windows, macOS, and Linux release builds and smoke tests pass from clean checkouts, without third-party runtime libraries.
3. A normal 1 GiB batch scan stays within the specified 64 MiB application-heap target and the plain unfiltered reference benchmark completes in under five seconds. If the speed gate fails, optimize and repeat the recorded benchmark before 1.0.
4. TUI responsiveness and follow tests meet the 0.4 and 0.5 gates.
5. The README and manual describe the actual binary, installation, examples, known limits, and security behavior; documentation examples are executed in CI where practical.
6. All written project content and all code comments satisfy the English-only policy.

Version 1.0 is the first stable release after these gates pass. Native systemd/Windows event readers, multi-file merge, compression, multiline reconstruction, and automatic template discovery remain candidates for post-1.0 work unless separately approved and specified.
