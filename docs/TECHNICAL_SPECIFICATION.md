# Technical Specification

**Project:** GLogViewer (`logview`)  
**Status:** Version 1.9.1 adds separate GUI archives for Windows, Linux, and macOS with SDL3 runtime files and launch scripts. The CLI/TUI packages remain independent of SDL3. Linux GUI packages require host graphics/audio libraries; the macOS package targets arm64. See [validation notes](VALIDATION.md).
**Language:** C99; all repository content is English.

## 1. Purpose and scope

`logview` reads local, line-oriented log files, extracts useful fields, evaluates filters, and presents results through a batch CLI or a terminal UI (TUI). Multi-file batch mode accepts up to 16 local files and merges parseable timestamps chronologically; TUI and follow remain single-file modes. The tool does not ingest network streams, read native binary journals, or provide a centralized log service. Input files are read-only.

The product is aimed at incident triage, application debugging, HTTP access analysis, and local security investigation. The input may exceed 1 GB. CLI scanning must use memory bounded by configured limits rather than file size. The TUI must remain usable during a scan and must not retain every matching line.

## 2. Evidence and format choices

The format list is grounded in source specifications and software documentation. “Common” here means widely documented and useful for these workflows; it is **not** a measured market-share ranking.

| Source software or domain | Log form users analyze | Fields of interest | Initial handling |
| --- | --- | --- | --- |
| Unix services, network equipment, syslog collectors | BSD-style syslog and RFC 5424 syslog | time, host, application, process, priority, message | Two separate presets; RFC 5424 structured data retained as raw field in v1. [RFC 3164](https://www.rfc-editor.org/info/rfc3164/), [RFC 5424](https://www.rfc-editor.org/info/rfc5424/) |
| Apache HTTP Server | Common and Combined access logs; separate error logs | client address, request, status, bytes, referrer, agent, error level, module | Access and error presets. [Apache log documentation](https://httpd.apache.org/docs/2.4/logs.html) |
| NGINX | Configurable access logs and error logs | remote address, request, status, upstream clues | Combined access preset; custom `log_format` through custom pattern. [NGINX log module](https://nginx.org/en/docs/http/ngx_http_log_module.html), [NGINX logging guide](https://docs.nginx.com/nginx/admin-guide/monitoring/logging/) |
| Applications, including Java services using Log4j | Text patterns or JSON events | timestamp, severity, message, service, request/trace ID | Custom pattern or simple JSON Lines. [Log4j layouts](https://logging.apache.org/log4j/2.x/manual/layouts.html), [OpenTelemetry log model](https://opentelemetry.io/docs/specs/otel/logs/data-model/) |
| Docker containers | JSON records with `log`, `stream`, and `time` | stream, timestamp, message | Compatible JSON-object mapping from an exported copy; direct modification of Docker-managed files is discouraged by Docker. [Docker JSON-file driver](https://docs.docker.com/engine/logging/drivers/json-file/) |
| Kubernetes workloads | Application stdout/stderr wrapped by CRI log records | timestamp, stream, tag, message | `cri` preset parses RFC 3339 Nano timestamps and preserves each source line; `P`/`F` fragments are not automatically joined. [Kubernetes CRI logging proposal](https://github.com/kubernetes/design-proposals-archive/blob/main/node/kubelet-cri-logging.md) |
| Windows Event Log and systemd journal | Native event/journal stores | event ID, unit, priority, message | Native binary/API readers out of scope; consume a line-oriented export. [Windows Event Log reference](https://learn.microsoft.com/en-us/windows/win32/wes/windows-event-log-reference), [systemd journal fields](https://www.freedesktop.org/software/systemd/man/latest/systemd.journal-fields.html) |

JSON Lines specifies UTF-8 and one valid JSON value per line. The `jsonl` preset accepts a bounded subset of JSON objects on individual lines and additionally accepts a top-level array of pretty-printed objects as a practical extension for exported datasets. It does not claim to accept every JSON value or every JSON document form. [JSON Lines specification](https://jsonlines.org/)

### CRI parser

The `cri` preset accepts `<timestamp> <stream> <tag> <message>`, where timestamp is an RFC 3339 Nano value, stream is exactly `stdout` or `stderr`, and tag is a non-empty token. It exposes `timestamp`, `stream`, `tag`, and `message`. The parser validates timestamp shape, calendar ranges, fractional precision up to nine digits, and UTC offsets. It preserves each physical source line as one record; `P` (partial) and `F` (full) values remain separate records and can be selected with `--filter "tag=P"` or `--filter "tag=F"`. Automatic fragment joining is not included. `--format auto` scores CRI alongside the existing presets.

## 3. Functional contract

| ID | Requirement | Acceptance result |
| --- | --- | --- |
| RF-01 | Stream one regular file larger than 1 GB | No file-size-proportional heap growth during a CLI scan. |
| RF-02 | Parse `syslog3164`, `syslog5424`, `cri`, `apache-common`, `apache-combined`, `apache-error`, `nginx-combined`, `jsonl`, and `custom` | Golden fixtures produce documented normalized fields. |
| RF-03 | Compile custom `%{TOKEN}` and `%{TOKEN:name}` patterns | Valid patterns extract fields; invalid/ambiguous patterns fail before scanning. |
| RF-04 | Include or exclude raw lines and filter individual fields with built-in regex | Match spans, exclusions, and field predicates agree with test vectors. |
| RF-05 | Combine predicates with `and`, `or`, and parentheses | Precedence and missing-field behavior match section 7. |
| RF-06 | TUI navigation, incremental search, match highlighting, help | Interactive smoke tests on all target OSes. |
| RF-07 | Follow appended lines | Complete new lines appear without duplicates; rotation/truncation follows section 10. |
| RF-08 | Export selected records as text or CSV | Output escapes line data and follows section 9. |
| RF-09 | Report level counts, a timeline, and frequent fields/values | Counts are exact where specified; estimates are labelled. |
| RF-10 | Distinct exit statuses | All conditions follow section 5. |

## 4. Input and normalized record

The reader consumes LF and CRLF files, strips only the record terminator, and keeps the original bytes in `raw`. An unterminated final line is processed at end-of-file in snapshot mode. In follow mode it is held until a newline arrives; a final partial line is emitted once on normal interruption only if `--flush-partial` is set. A NUL byte is a record error. UTF-8 is expected for JSON; text presets preserve bytes and render invalid sequences with replacement glyphs without changing exported raw bytes.

The default maximum physical line and logical JSON record size is **16 MiB**; `--max-line-bytes` can lower it but not raise it in v1. An oversized line or object is counted as rejected and does not stop the scan. The reader copies record fragments into a reusable buffer. JSON records accept objects with nested objects and arrays to a maximum depth of 64, up to 64 extracted primitive fields per entry, unique keys within each object, and strings/numbers/booleans/null. Top-level arrays are streamed one object at a time; their contents are not retained as a whole document. Strings support JSON escapes including `\uXXXX` with valid surrogate pairs. Nested primitive fields are exposed through dotted paths; a unique leaf key is also available by its short name. Malformed JSON is a rejected record. These limits prevent a single record from defeating bounded-memory behavior.

The internal `LogEntry` carries `raw`, source line number, byte offset, parse status, optional timestamp, optional level, optional message, and a bounded set of named fields. Fields refer to spans of the reusable input buffer where possible; decoded JSON strings live in one reusable scratch buffer. They are valid only until the next read. Output and TUI caches copy only the data they retain. Values are strings for filter purposes; JSON numbers retain their input lexical form. Field names are case-sensitive ASCII identifiers (`[A-Za-z_][A-Za-z0-9_]*`), optionally joined by dots for nested JSON paths. Canonical aliases are `timestamp`, `level`, `message`, `host`, `process`, `pid`, `client_ip`, `request`, `status`, and `raw`.

For JSON objects, `timestamp`/`time`/`ts`, `level`/`severity`, and `message`/`msg`/`log` map to canonical fields in that order; original keys remain addressable. Null is treated as missing. Docker's `stream` remains an ordinary field. Syslog `PRI` is decoded to facility and severity fields; a level label is mapped from severity. RFC 3164's missing year and time zone are not invented. Timestamp display retains the original text; timeline aggregation uses only timestamps that can be interpreted unambiguously. HTTP access records have no intrinsic level; `level` is missing, while `status` is present.

Parse failures retain `raw` and source position for raw regex filtering and TUI display. Field filters on missing fields evaluate false, including `!=`. Rejected records are counted separately from successfully parsed records. Invalid records are never silently counted as `INFO`.

## 5. Proposed CLI and exit behavior

```text
logview <file> [--format auto|raw|syslog3164|syslog5424|cri|apache-common|apache-combined|apache-error|nginx-combined|jsonl|custom]
               [--pattern <pattern>] [--filter <expression>] [--regex <regex>]
               [--regex-field <field>=<regex>] [--exclude-regex <regex>]
               [--stats [--stats-field FIELD]]
               [--syslog-year YYYY] [--syslog-timezone +/-HH:MM]
               [--profile <file>] [--save-profile <file> [--profile-overwrite]]
               [--context-before <n>] [--context-after <n>]
               [--export text|csv]
               [--output <path>] [--columns <comma-separated-fields>] [--force] [--follow]
               [--tui] [--from-start] [--max-line-bytes <n>] [--flush-partial]
logview --help
logview --version
```

`auto` samples at most 32 complete lines or 256 KiB and chooses a preset only on a unique, confident match; otherwise it treats records as raw text. `raw` bypasses parsing and exposes only the raw line and source position. Users can select `--format custom --pattern ...` for unknown formats. `--regex`, `--regex-field`, and `--exclude-regex` are each accepted once and combine with the field filter using AND. `--exclude-regex` rejects a record when the pattern matches its complete raw line. `--pattern` is legal only with `custom`. `--follow` with `--stats` or `--export` is invalid. `--output` requires `--export`; exports fail rather than overwrite an existing file unless `--force` is supplied. `--tui` requires both input and output terminals; otherwise the command fails with an argument error. Without `--tui`, matching records go to standard output, except `--stats` which prints a summary.

| Code | Meaning | Examples |
| --- | --- | --- |
| 0 | Successful operation with at least one match; `--stats` succeeded; normal TUI/follow exit | Matching scan, valid summary. |
| 1 | Snapshot selection produced no matches | Empty filtered scan or export. |
| 2 | I/O or environment failure | Input read failure, output failure, unsupported terminal. |
| 3 | Invalid argument, parsing pattern, filter, or regex | Unknown option, malformed expression. |

Diagnostics go to standard error. On an output write failure the process returns 2, even after partial output. Error 3 is detected before opening an export file when possible. No-match status does not apply to an unfiltered empty stats report.

Query profiles use the text line format `GLOGVIEW_PROFILE 1` followed by unique `key=value` lines. Supported keys are `format`, `pattern`, `filter`, `regex`, `regex_field`, `exclude_regex`, `syslog_year`, `syslog_timezone`, `context_before`, and `context_after`. Values are limited to 4095 bytes and may not contain CR or LF. Unknown keys, duplicate keys, invalid values, and unsupported profile versions are errors. CLI values override loaded profile values. Saving refuses to replace an existing path unless `--profile-overwrite` is also supplied. The profile save path must differ textually from the input path.

## 6. Parsing patterns

Patterns are compiled into a sequence of literal fragments and typed captures, **not** translated into the user regex syntax. This avoids requiring named groups or `{m,n}` quantifiers from the built-in regex engine. Matching is anchored to the whole record. Adjacent variable-length placeholders without an intervening literal are ambiguous and rejected, except a terminal `MESSAGE` capture. Matching uses the next literal as delimiter; `MESSAGE` at the end takes the remainder. This deterministic rule is documented so ambiguous records cannot silently shift fields.

Supported placeholders: `TIMESTAMP`, `LEVEL`, `MESSAGE`, `FIELD:name`, `MONTH`, `DAY`, `TIME`, `HOST`, `PROC`, and `PID`. `%{TIMESTAMP:ts}` writes both canonical `timestamp` and field `ts`; `%{LEVEL:lvl}` and `%{MESSAGE:msg}` work the same way. `%{FIELD:request_id}` writes only `request_id`. Alias names must be unique. Literal `%%` represents `%`; literal backslash has no special meaning in the pattern grammar.

`TIMESTAMP` accepts ISO 8601/RFC 3339-style date-times with an optional numeric offset or `Z`, plus `YYYY-MM-DD HH:MM:SS`; dates without a zone remain displayable but are not placed on an absolute timeline. `MONTH`, `DAY`, and `TIME` accept the BSD syslog-style English month abbreviation, one- or two-digit day, and `HH:MM:SS`. `PID` accepts decimal digits, `LEVEL` accepts an ASCII word, and `HOST`/`PROC` accept non-space text. `FIELD:name` captures text up to the next literal delimiter. A terminal `MESSAGE` captures any remaining bytes. Presets apply their own stricter grammar where required.

Examples:

```text
%{TIMESTAMP:ts} %{LEVEL:lvl} %{MESSAGE:msg}
%{MONTH} %{DAY} %{TIME} %{HOST} %{PROC}[%{PID}]: %{MESSAGE}
```

The second example is a BSD-style syslog body and does not model RFC 5424. Preset parsers are dedicated functions because each native format needs validation beyond delimiter extraction. Custom patterns are deliberately a useful subset; complex quoting and nested structure should use a preset or a later parser extension.

## 7. Filter and regex semantics

The filter grammar is:

```text
expression := or_expr
or_expr    := and_expr ("or" and_expr)*
and_expr   := primary ("and" primary)*
primary    := '(' expression ')' | identifier operator value
operator   := '=' | '!=' | '~'
value      := bare_token | double_quoted_string
```

`and` binds more tightly than `or`. `=` and `!=` compare complete field values byte-for-byte; `~` searches for a regex anywhere within the named field; `<`, `<=`, `>`, and `>=` perform bytewise lexical comparisons. Double-quoted strings allow `\"` and `\\`. The `raw` field is always present. For example, `level=ERROR and (request_id=ab12 or message~"timeout")`. A field that is absent makes every predicate false, including `!=`. Range operators do not parse numbers or dates: fixed-width ISO 8601 values with the same timezone representation sort chronologically, while differing timezone offsets must be normalized first. `--regex` searches the full raw line; `--regex-field message=timeout` searches only `message`; `--exclude-regex error` rejects raw lines containing `error`. Regex matching is case-sensitive, byte-oriented, and ASCII for `\d`, `\w`, and `\s`; UTF-8 non-ASCII bytes are matched literally or by `.`. No Unicode case folding is implied.

`--top N` retains the first N selected records. `--bottom N` streams selected raw records to a temporary file and retains an N-element offset/length index before replaying the final records in source order. The TUI stores only the final N row checkpoints for bottom mode. Both forms scan the complete source except that batch `--top` can stop as soon as N matches are emitted. N is limited to 1,000,000. Limited TUI views disable source-line jump because that index refers to the full source rather than the displayed subset. `--record first|last` emits the full original first or last selected record, including multiline JSON; last-record mode retains at most one record in memory, within the configured record-size limit.

### Multi-file batch mode

Provide two or more file paths before or after the shared options, for example `logview api-a.jsonl api-b.jsonl --format jsonl`. Up to 16 input files are accepted. One explicit format and custom pattern apply to every source; `--format auto` detects each independently. Directory paths and duplicate file identities are rejected. The inputs must be seekable local files; `-` is not accepted. Multi-file mode does not support `--tui`, `--follow`, `--top`, `--bottom`, or `--record`.

Records with supported timestamps are merged in ascending UTC epoch. Every source must be nondecreasing across its parseable timestamps, even when a record is excluded by the active filters. A timestamp decrease stops processing with exit code 2. For equal timestamps, command-line argument order wins; records from one source retain their source-line order. Records with absent or unrecognized timestamps are emitted after all timestamped records, grouped by input argument order and then source-line order. This makes their placement explicit without pretending they have a chronological position.

Text output prefixes each record with `[source-path:source-line]`. CSV exports always include `source_file`; when `--columns` is supplied, the column is appended if omitted. Line numbers refer to the start of a source record. `--stats` aggregates all selected records and builds one timeline over the combined timestamp range. Heap use is bounded by the 16 source readers, their reusable input buffers, and at most one current record per source, so its record-memory component is O(input-count × max-line-bytes); it does not grow with total file size. Timestamp values and untimestamped records are staged in temporary files. Temporary storage can therefore approach the selected input size for sources with many unknown timestamps.

Regex syntax in v1: literals, escaped metacharacters, `.`, `*`, `+`, `?`, `[abc]`, ranges such as `[a-z]`, negated classes, `\d`, `\w`, `\s`, `^`, `$`, capturing parentheses, and `|`. `.` excludes LF and CR. `^` and `$` refer to the beginning and end of the target field or raw line. Empty alternatives and empty groups are valid. Invalid ranges, dangling escapes, unmatched parentheses/brackets, and a quantifier without an operand are compile errors. Backreferences, lookaround, lazy quantifiers, and `{m,n}` are unsupported. The first leftmost match is used for highlighting; zero-length matches advance by one byte before seeking the next highlight.

The compiler builds an AST for syntax validation and then a Thompson-style NFA program. An iterative NFA VM performs matching with bounded stacks and no recursive backtracking. Capturing spans may need additional bookkeeping but must not create unbounded recursion or exponential retry. Russ Cox's comparison of NFA simulation with recursive backtracking motivates this change from the initial idea; nested patterns such as `(a+)+b` must not cause catastrophic backtracking. [Regular Expression Matching Can Be Simple And Fast](https://swtch.com/~rsc/regexp/regexp1.html) For a fixed regex of `m` states and input of `n` bytes, the baseline target is O(mn) time and O(m) active-state memory; `m` is capped at 4096 states. Literal-prefix scanning is an optional optimization, not a correctness dependency.

## 8. Statistics

`--stats` reports exact counts of physical, parsed, rejected, and selected records. Per-level counts, timeline bins, and frequent fields describe **selected** records; missing level appears as `UNSPECIFIED`. A fixed 256-bin timeline covers the observed timestamp range, so a two-pass scan is used for timeline output on seekable files; timestamps lacking a zone or otherwise ambiguous are listed as unknown and omitted from bins. The time range and bin width are printed. ISO/RFC 3339 timestamps with `Z` or numeric offsets and Apache access timestamps are interpreted. Apache Error timestamps are interpreted as UTC because the format carries no timezone. RFC 3164 timestamps can be interpreted when `--syslog-year YYYY` is supplied; `--syslog-timezone +/-HH:MM` sets the assumed fixed offset and defaults to UTC. The configured year is applied uniformly, so year-boundary rollover is not inferred. These options require `--format syslog3164` or `--format auto` and affect timeline interpretation only. Frequent field-name and field-value pairs use a bounded Space-Saving table of 512 candidates. A candidate with zero error is marked exact; otherwise it is marked estimated and includes an upper error bound. `--stats-field FIELD` limits candidates to the named parsed field and implies stats mode. The timeline's second pass reapplies the same parser and filters; timestamps beyond the first-pass range cause a read/statistics error.

## 9. Export

Text export writes selected raw lines and one LF terminator per record, preserving all other bytes. CSV export writes UTF-8 with CRLF records and RFC 4180-style quoting: fields containing comma, quote, CR, or LF are quoted; quotes are doubled. Invalid UTF-8 bytes in non-JSON source fields are replaced with U+FFFD in CSV only, while text export preserves them. The default columns are `line_number,timestamp,level,message,raw`; `--columns` chooses an explicit fixed list. Missing values are empty. A file is created exclusively by default; `--force` allows replacement. The export is streamed and does not buffer all matches.

## 10. Follow and TUI behavior

Snapshot mode begins at byte zero. `--follow` begins at the current end unless `--from-start` is set. It polls at 250 ms nominal intervals and waits for newline-terminated records. On truncation it resets to offset zero. On file identity change it closes the old handle and opens the new path from offset zero. Identity uses device/inode on Unix and file ID on Windows. Follow does not promise zero-loss handling across a fast sequence of rotations. Ctrl-C exits cleanly with code 0; `--flush-partial` emits a pending incomplete record on interruption. `--follow` cannot be combined with statistics or export.

The `--tui` implementation requires an interactive terminal. It keeps a 256-row ring with up to 1 KiB of display bytes per row and writes source line/byte-offset checkpoints to temporary files. One index supports navigation every 128 selected rows; a second supports physical source-line lookup, storing the selected-row sequence at each checkpoint. JSON document checkpoints inside multiline objects are skipped so jumps can resume at a valid record boundary. After the initial scan, arrows, paging, and Home/End can navigate the full selected input; a cache miss seeks to the preceding checkpoint and reapplies the active selectors. `J` prompts for a one-based physical source line, seeks to its preceding checkpoint, reapplies the selectors from there, and selects the first visible record at or after that line. If no visible record follows the target, it selects the last visible record; invalid and out-of-range line numbers are reported. Regex search and next/previous match traverse the full selected input but inspect at most the first 1 KiB of each row. Submitting `/` search selects the first match; Enter and Shift+Enter move forward and backward with wrapping. Search navigation runs in bounded batches, reports progress, and can be cancelled with Esc. `n`/`N` remain aliases. `e`/`E` jump to error records and `w`/`W` to warnings, using parsed levels and mapping HTTP 5xx/4xx statuses. `b` toggles a source-line bookmark and `B` moves to the next bookmark in source order; up to 32 are held in memory for the current TUI session. Filter rescans and file rotation clear them explicitly. `c` toggles physical context, including records excluded by filters; `--context-before` and `--context-after` configure each radius from 0 to 100, default 4, and `+`/`-` adjust both radii. `C` clears the search and all field/raw regex filters, then rescans the input. `--follow --tui` waits without blocking input, joins appended partial lines after newline, and clears/rescans after truncation or replacement. The viewport redraws after terminal size changes. Control bytes are sanitized. Field-filter/raw-regex edits trigger a rescan. Terminal state must be restored on normal exit and recoverable errors.

The TUI shows a viewport of selected records, current format, scan progress, rejected-record count, and active filters. Arrow/Page Up/Page Down/Home/End navigate; `/` opens regex search and jumps to the first match; Enter/Shift+Enter move to the next/previous match, while `n`/`N` remain aliases. `z` opens a full-record view that wraps to the terminal width and can scroll through the complete selected record; valid JSON is formatted into a temporary file and loaded only if the formatted output is within the 16 MiB cap. Failure or oversized output falls back to source text; this affects presentation only. `v` prompts for a field name and runs a bounded-memory frequent-value summary over selected records; candidates show exact counts only when the Space-Saving error is zero. `h` builds a 256-bin histogram in two streaming passes, reports unknown timestamps, and lets the user jump to the first matching record sequence in a nonempty bucket. RFC 3164 timestamps contribute when `--syslog-year` is set; the optional fixed offset comes from `--syslog-timezone`. Histogram mode requires a complete scan of a seekable input and is unavailable with result limits or follow. `f` edits the field filter, `r` edits the raw regex, `C` clears all search and filter predicates and rescans, `?` shows help, and `q` quits. A bounded ring of displayed rows plus sparse byte-offset checkpoints supports scrolling; jumping far backward may rescan from a checkpoint. Filter changes invalidate the displayed match cache and trigger a cancellable rescan. Scanning is sliced between input/render polls, so input remains responsive. ANSI output is used on Unix terminals; Windows enables virtual terminal output where available and uses Win32 console calls for input. Terminals without required capability receive a clear error; redirected output uses batch mode.

TUI search also accepts Boolean expressions over quoted, case-sensitive literal terms. `AND`, `OR`, `NOT`, and parentheses use `NOT` > `AND` > `OR` precedence. An unquoted `AND`, `OR`, or `NOT` selects this grammar; otherwise the query is compiled as a regex. All search modes inspect at most the first 1 KiB of each displayed row.

## 11. Portability, safety, and performance

Build with a C99 compiler and the C standard library; the 0.1 MVP uses standard C file I/O and has a `Makefile` and `build.bat`. Later platform wrappers will use POSIX or Win32 APIs for TUI terminal input, sleeping, and file identity. `getline`, `fseeko`, `termios`, and `_fseeki64` are not required by the 0.1 reader. No third-party runtime libraries are required.

Memory target: at most 64 MiB application heap for a normal batch scan with a 16 MiB maximum line, including parser scratch, regex states, and statistics; the OS page cache and C runtime overhead are excluded. Scan throughput target: 1 GB in under 5 seconds for a plain, unfiltered scan on a reference NVMe machine after warm-up. This is a benchmark target, **not a guaranteed throughput**; filesystem cache, CPU, format, encoding, and filters matter. The benchmark protocol records hardware, cold/warm cache, compiler flags, file format, line-length distribution, elapsed wall time, peak RSS, and output destination. It uses a generated 1 GiB fixture and repeats five times, reporting the median. The 1 GB criterion from the initial proposal is not accepted until measured.

Input paths are never executed. TUI escapes control characters from log content before writing terminal output, preventing untrusted log lines from injecting terminal commands. Output path errors, short writes, integer overflow, invalid UTF-8 in JSON, and allocation failure are checked. Regex limits cap compile size; matching has an operation budget per line for defensive failure reporting even though the NFA VM is iterative.

## 12. Verification plan

1. Regex unit tests cover every operator, precedence, anchors, empty matches, invalid syntax, capture spans, and adversarial nested quantifiers.
2. Parser fixture tests cover RFC 3164 versus RFC 5424, Apache Common/Combined, NGINX Combined, JSON escaping, malformed JSON, custom placeholders, and missing fields.
3. CLI integration tests cover filter precedence, output quoting, exit codes, no matches, long lines, CRLF, partial final lines, and write errors.
4. Follow tests append, truncate, and rotate files while checking duplicate and loss behavior against section 10.
5. Manual TUI tests run on Windows Terminal, a Linux terminal, and a macOS terminal with large files, resize events, search, scrolling, and redirected I/O.
6. A performance benchmark records the protocol and target in section 11 rather than assuming the target is met.

For varied fixtures, [Loghub](https://github.com/logpai/loghub) offers documented system-log datasets; their licenses and privacy characteristics must be reviewed before redistribution. The [Drain paper](https://pinjiahe.github.io/files/pdf/research/ICWS17.pdf) is useful background for future automatic template discovery, which is outside this release.
