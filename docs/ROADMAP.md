# Roadmap to Version 1.0

**Status:** proposed release plan, 2026-10-08. The 0.1 MVP is implemented. The 0.2 structured-log implementation is in progress; the current Windows build and CLI checks pass. macOS and Linux validation remains outstanding. This roadmap stages the [1.0 technical contract](TECHNICAL_SPECIFICATION.md).

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

1. **One local file, two explicit formats.** `--format raw` (default during 0.1) and `--format jsonl`. JSON Lines accepts the bounded top-level primitive-object subset defined in the technical specification; unknown or malformed objects remain visible as raw records but have no parsed fields.
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

Add streamed text and CSV export with `--output`, `--columns`, and explicit `--force` overwrite. Add exact physical/parsed/rejected/selected and per-level counts, a 256-bin timeline over unambiguous timestamps, and bounded estimated frequent field/value candidates. Statistics describe the selected records; estimate labels and `UNKNOWN_TIME` are visible.

**Gate:** byte-level text export and CSV quoting tests pass, including non-ASCII, quotes, commas, invalid UTF-8 replacement in CSV, empty values, write errors, and no matches. Exact counts match fixtures; heavy-hitter output is marked estimated. A large scan still meets the memory target.

## 0.4: follow

Add `--follow`, `--from-start`, and `--flush-partial` for batch output. Follow complete appended lines, detect truncation and file replacement, drain the old handle, and reopen the new path. Keep the documented rapid-rotation loss limitation explicit.

**Gate:** scripted append, partial-line, truncate, rename/replacement, reopen-failure, and Ctrl-C cases pass on each platform. No duplicate complete record is emitted in the normal append/truncate/replacement test paths. New complete records are visible within one second on the release test machine under normal load.

## 0.5: terminal interface

Add `--tui` with scrolling, search, highlighting, filter edits, help, summary, and follow integration. Use a bounded viewport cache and sparse checkpoints; rescan on distant jumps and after filter changes. Reject unsupported terminals clearly and sanitize log control bytes before rendering.

**Gate:** manual scenarios pass on Windows Terminal and representative macOS/Linux terminals. Key input remains responsive during a 1 GiB scan; target p95 input-to-redraw latency is at most 200 ms on the documented release test machine. Resizing, invalid filter edits, long lines, and terminal restoration after errors are included.

## 0.9: release candidate and 1.0 decision

Freeze the 1.0 CLI and file-format behavior. Run the full test suite and sanitizer or equivalent checks available per platform; test high-cardinality fields, malformed input, long paths, Unicode, and output failures. Publish a benchmark report using the protocol in the technical specification: generated 1 GiB fixture, five warm-cache runs, median elapsed time, peak heap/RSS, compiler settings, CPU, storage, and output destination. Freeze the reference machine before running the acceptance benchmark.

The 1.0 release requires:

1. Every RF-01 through RF-10 acceptance result in the technical specification has passing evidence.
2. Windows, macOS, and Linux release builds and smoke tests pass from clean checkouts, without third-party runtime libraries.
3. A normal 1 GiB batch scan stays within the specified 64 MiB application-heap target and the plain unfiltered reference benchmark completes in under five seconds. If the speed gate fails, optimize and repeat the recorded benchmark before 1.0.
4. TUI responsiveness and follow tests meet the 0.4 and 0.5 gates.
5. The README and manual describe the actual binary, installation, examples, known limits, and security behavior; documentation examples are executed in CI where practical.
6. All written project content and all code comments satisfy the English-only policy.

Version 1.0 is the first stable release after these gates pass. Native systemd/Windows event readers, multi-file merge, compression, multiline reconstruction, and automatic template discovery remain candidates for post-1.0 work unless separately approved and specified.
