# Technical Specification

**Project:** GLogViewer (`logview`)  
**Status:** 1.0 design target; 0.1, core 0.2, and 0.3 reporting are implemented. Cross-platform and release validation remain. See the [roadmap](ROADMAP.md).
**Language:** C99; all repository content is English.

## 1. Purpose and scope

`logview` reads local, line-oriented log files, extracts useful fields, evaluates filters, and presents results through a batch CLI or a terminal UI (TUI). The first release supports one input file per invocation and does not ingest network streams, read native binary journals, or provide a centralized log service. Input files are read-only.

The product is aimed at incident triage, application debugging, HTTP access analysis, and local security investigation. The input may exceed 1 GB. CLI scanning must use memory bounded by configured limits rather than file size. The TUI must remain usable during a scan and must not retain every matching line.

## 2. Evidence and format choices

The format list is grounded in source specifications and software documentation. “Common” here means widely documented and useful for these workflows; it is **not** a measured market-share ranking.

| Source software or domain | Log form users analyze | Fields of interest | Initial handling |
| --- | --- | --- | --- |
| Unix services, network equipment, syslog collectors | BSD-style syslog and RFC 5424 syslog | time, host, application, process, priority, message | Two separate presets; RFC 5424 structured data retained as raw field in v1. [RFC 3164](https://www.rfc-editor.org/info/rfc3164/), [RFC 5424](https://www.rfc-editor.org/info/rfc5424/) |
| Apache HTTP Server | Common and Combined access logs; separate error logs | client address, request, status, bytes, referrer, agent | Access presets; error logs through custom pattern. [Apache log documentation](https://httpd.apache.org/docs/2.4/logs.html) |
| NGINX | Configurable access logs and error logs | remote address, request, status, upstream clues | Combined access preset; custom `log_format` through custom pattern. [NGINX log module](https://nginx.org/en/docs/http/ngx_http_log_module.html), [NGINX logging guide](https://docs.nginx.com/nginx/admin-guide/monitoring/logging/) |
| Applications, including Java services using Log4j | Text patterns or JSON events | timestamp, severity, message, service, request/trace ID | Custom pattern or simple JSON Lines. [Log4j layouts](https://logging.apache.org/log4j/2.x/manual/layouts.html), [OpenTelemetry log model](https://opentelemetry.io/docs/specs/otel/logs/data-model/) |
| Docker containers | JSON records with `log`, `stream`, and `time` | stream, timestamp, message | Compatible JSON-object mapping from an exported copy; direct modification of Docker-managed files is discouraged by Docker. [Docker JSON-file driver](https://docs.docker.com/engine/logging/drivers/json-file/) |
| Kubernetes workloads | Application stdout/stderr wrapped by CRI log records | time, stream, partial/full flag, message | Custom pattern in v1; dedicated CRI preset proposed later. [Kubernetes logging architecture](https://kubernetes.io/docs/concepts/cluster-administration/logging/) |
| Windows Event Log and systemd journal | Native event/journal stores | event ID, unit, priority, message | Native binary/API readers out of scope; consume a line-oriented export. [Windows Event Log reference](https://learn.microsoft.com/en-us/windows/win32/wes/windows-event-log-reference), [systemd journal fields](https://www.freedesktop.org/software/systemd/man/latest/systemd.journal-fields.html) |

JSON Lines specifies UTF-8 and one valid JSON value per line. The `jsonl` preset accepts a bounded subset of JSON objects on individual lines and additionally accepts a top-level array of pretty-printed objects as a practical extension for exported datasets. It does not claim to accept every JSON value or every JSON document form. [JSON Lines specification](https://jsonlines.org/)

## 3. Functional contract

| ID | Requirement | Acceptance result |
| --- | --- | --- |
| RF-01 | Stream one regular file larger than 1 GB | No file-size-proportional heap growth during a CLI scan. |
| RF-02 | Parse `syslog3164`, `syslog5424`, `apache-common`, `apache-combined`, `nginx-combined`, `jsonl`, and `custom` | Golden fixtures produce documented normalized fields. |
| RF-03 | Compile custom `%{TOKEN}` and `%{TOKEN:name}` patterns | Valid patterns extract fields; invalid/ambiguous patterns fail before scanning. |
| RF-04 | Filter raw lines or individual fields with built-in regex | Match spans and field predicates agree with test vectors. |
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
logview <file> [--format auto|raw|syslog3164|syslog5424|apache-common|apache-combined|nginx-combined|jsonl|custom]
               [--pattern <pattern>] [--filter <expression>] [--regex <regex>]
               [--regex-field <field>=<regex>] [--stats] [--export text|csv]
               [--output <path>] [--columns <comma-separated-fields>] [--force] [--follow]
               [--tui] [--from-start] [--max-line-bytes <n>] [--flush-partial]
logview --help
logview --version
```

`auto` samples at most 32 complete lines or 256 KiB and chooses a preset only on a unique, confident match; otherwise it treats records as raw text. `raw` bypasses parsing and exposes only the raw line and source position. Users can select `--format custom --pattern ...` for unknown formats. Multiple `--regex` or `--regex-field` options are combined with logical AND and also ANDed with `--filter`. `--pattern` is legal only with `custom`. `--follow` with `--stats` or `--export` is invalid in 1.0. `--output` requires `--export`; exports fail rather than overwrite an existing file unless `--force` is supplied. `--tui` requires both input and output terminals; otherwise the command fails with an argument error. Without `--tui`, matching records go to standard output, except `--stats` which prints a summary.

| Code | Meaning | Examples |
| --- | --- | --- |
| 0 | Successful operation with at least one match; `--stats` succeeded; normal TUI/follow exit | Matching scan, valid summary. |
| 1 | Snapshot selection produced no matches | Empty filtered scan or export. |
| 2 | I/O or environment failure | Input read failure, output failure, unsupported terminal. |
| 3 | Invalid argument, parsing pattern, filter, or regex | Unknown option, malformed expression. |

Diagnostics go to standard error. On an output write failure the process returns 2, even after partial output. Error 3 is detected before opening an export file when possible. No-match status does not apply to an unfiltered empty stats report.

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

`and` binds more tightly than `or`. `=` and `!=` compare complete field values byte-for-byte; `~` searches for a regex anywhere within the named field. Double-quoted strings allow `\"` and `\\`. The `raw` field is always present. For example, `level=ERROR and (request_id=ab12 or message~"timeout")`. A field that is absent makes every predicate false. `--regex` searches the full raw line; `--regex-field message=timeout` searches only `message`. Regex matching is case-sensitive, byte-oriented, and ASCII for `\d`, `\w`, and `\s`; UTF-8 non-ASCII bytes are matched literally or by `.`. No Unicode case folding is implied.

Regex syntax in v1: literals, escaped metacharacters, `.`, `*`, `+`, `?`, `[abc]`, ranges such as `[a-z]`, negated classes, `\d`, `\w`, `\s`, `^`, `$`, capturing parentheses, and `|`. `.` excludes LF and CR. `^` and `$` refer to the beginning and end of the target field or raw line. Empty alternatives and empty groups are valid. Invalid ranges, dangling escapes, unmatched parentheses/brackets, and a quantifier without an operand are compile errors. Backreferences, lookaround, lazy quantifiers, and `{m,n}` are unsupported. The first leftmost match is used for highlighting; zero-length matches advance by one byte before seeking the next highlight.

The compiler builds an AST for syntax validation and then a Thompson-style NFA program. An iterative NFA VM performs matching with bounded stacks and no recursive backtracking. Capturing spans may need additional bookkeeping but must not create unbounded recursion or exponential retry. Russ Cox's comparison of NFA simulation with recursive backtracking motivates this change from the initial idea; nested patterns such as `(a+)+b` must not cause catastrophic backtracking. [Regular Expression Matching Can Be Simple And Fast](https://swtch.com/~rsc/regexp/regexp1.html) For a fixed regex of `m` states and input of `n` bytes, the baseline target is O(mn) time and O(m) active-state memory; `m` is capped at 4096 states. Literal-prefix scanning is an optional optimization, not a correctness dependency.

## 8. Statistics

`--stats` reports exact counts of physical, parsed, rejected, and selected records. Per-level counts, timeline bins, and frequent fields describe **selected** records; missing level appears as `UNSPECIFIED`. A fixed 256-bin timeline covers the observed timestamp range, so a two-pass scan is used for timeline output on seekable files; timestamps lacking a zone or otherwise ambiguous are listed as `UNKNOWN_TIME` and omitted from bins. The time range and bin width are printed. ISO/RFC 3339 timestamps with `Z` or numeric offsets and Apache Combined timestamps are interpreted. Frequent field-name and field-value pairs use a bounded Space-Saving table of 512 candidates; their counts are estimates and are marked `estimated`. Exact top-frequency data for arbitrary cardinality would violate fixed memory. The timeline's second pass reapplies the same parser and filters; timestamps beyond the first-pass range cause a read/statistics error.

## 9. Export

Text export writes selected raw lines and one LF terminator per record, preserving all other bytes. CSV export writes UTF-8 with CRLF records and RFC 4180-style quoting: fields containing comma, quote, CR, or LF are quoted; quotes are doubled. Invalid UTF-8 bytes in non-JSON source fields are replaced with U+FFFD in CSV only, while text export preserves them. The default columns are `line_number,timestamp,level,message,raw`; `--columns` chooses an explicit fixed list. Missing values are empty. A file is created exclusively by default; `--force` allows replacement. The export is streamed and does not buffer all matches.

## 10. Follow and TUI behavior

Snapshot mode begins at byte zero. `--follow` begins at the current end unless `--from-start` is set. It polls at 250 ms nominal intervals and waits for newline-terminated records. On truncation it resets to offset zero. On file identity change it closes the old handle and opens the new path from offset zero. Identity uses device/inode on Unix and file ID on Windows. Follow does not promise zero-loss handling across a fast sequence of rotations. Ctrl-C exits cleanly with code 0; `--flush-partial` emits a pending incomplete record on interruption. `--follow` cannot be combined with statistics or export.

The `--tui` implementation requires an interactive terminal. It keeps a 256-row ring with up to 1 KiB of display bytes per row and writes source line/byte-offset checkpoints to temporary files. One index supports navigation every 128 selected rows; a second supports physical source-line lookup, storing the selected-row sequence at each checkpoint. JSON document checkpoints inside multiline objects are skipped so jumps can resume at a valid record boundary. After the initial scan, arrows, paging, and Home/End can navigate the full selected input; a cache miss seeks to the preceding checkpoint and reapplies the active selectors. `J` prompts for a one-based physical source line, seeks to its preceding checkpoint, reapplies the selectors from there, and selects the first visible record at or after that line. If no visible record follows the target, it selects the last visible record; invalid and out-of-range line numbers are reported. Regex search and next/previous match traverse the full selected input but inspect at most the first 1 KiB of each row. Submitting `/` search selects the first match; Enter and Shift+Enter move forward and backward with wrapping. Search navigation runs in bounded batches, reports progress, and can be cancelled with Esc. `n`/`N` remain aliases. `e`/`E` jump to error records and `w`/`W` to warnings, using parsed levels and mapping HTTP 5xx/4xx statuses. `c` toggles a view of four physical records on either side of the selected row, including records excluded by filters. `C` clears the search and all field/raw regex filters, then rescans the input. `--follow --tui` waits without blocking input, joins appended partial lines after newline, and clears/rescans after truncation or replacement. The viewport redraws after terminal size changes. Control bytes are sanitized. Field-filter/raw-regex edits trigger a rescan. Terminal state must be restored on normal exit and recoverable errors.

The TUI shows a viewport of selected records, current format, scan progress, rejected-record count, and active filters. Arrow/Page Up/Page Down/Home/End navigate; `/` opens regex search and jumps to the first match; Enter/Shift+Enter move to the next/previous match, while `n`/`N` remain aliases. `z` opens a full-record view that wraps to the terminal width and can scroll through the complete selected record; it reads the record through a separate bounded reader so the active scan is preserved. `f` edits the field filter, `r` edits the raw regex, `C` clears all search and filter predicates and rescans, `s` opens summary, `?` shows help, and `q` quits. A bounded ring of displayed rows plus sparse byte-offset checkpoints supports scrolling; jumping far backward may rescan from a checkpoint. Filter changes invalidate the displayed match cache and trigger a cancellable rescan. Scanning is sliced between input/render polls, so input remains responsive. ANSI output is used on Unix terminals; Windows enables virtual terminal output where available and uses Win32 console calls for input. Terminals without required capability receive a clear error; redirected output uses batch mode.

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
