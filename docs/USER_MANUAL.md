# User Manual

**Status:** the batch CLI includes the 0.1 MVP, core 0.2 formats/filters, 0.3 reporting, and 0.4 follow features. The 0.5 TUI supports full-scan navigation using temporary checkpoints, filter editing, context view, export of filtered/search-matched records, and live follow; Windows and Linux test suites have passed, and the user reports the macOS suite passing. The formal release benchmark and broader release hardening remain for 1.0. The [roadmap](ROADMAP.md) gives implementation status and release gates.

## 1. What the tool will do

`logview` reads a local line-oriented log file and prints selected records. The current CLI supports raw, JSON Lines, syslog RFC 3164/5424, Apache/Nginx access logs, custom patterns, format detection, text/CSV export, statistics, follow mode, and an initial terminal browser. The input is read-only. Binary systemd journals and native Windows Event Log files must first be exported to a supported line-oriented form.

## Build and options

On Windows, run these separate commands in PowerShell with Visual Studio C++ Build Tools installed:

```powershell
.\build.bat
python tests\test_cli.py build\logview.exe
```

`build.bat` rebuilds the executable, so Windows does not need a separate clean step. The `make clean && make test` sequence applies to macOS/Linux, where Make is available:

```sh
make clean && make test
```

The current CLI supports one file and these options:

```text
logview <file> [--format FORMAT] [--pattern PATTERN] [--filter EXPRESSION]
                [--regex pattern] [--regex-field field=pattern]
                [--stats | --export text|csv --output path]
                [--columns list] [--force]
                [--follow [--from-start] [--flush-partial]]
                [--max-line-bytes count]
```

`--format raw` is the default. Formats include `raw`, `jsonl`, `syslog3164`, `syslog5424`, `apache-common`, `apache-combined`, `nginx-combined`, `custom`, and `auto`. `--filter` accepts equality, inequality, and field regex predicates combined with `and`, `or`, and parentheses. `--regex` searches the entire raw line; `--regex-field` searches a parsed field. These options are combined with `--filter` using AND. `--stats` prints exact record and level counts, a 256-bin timestamp timeline, and estimated frequent field/value pairs. Timestamp aggregation recognizes ISO/RFC 3339 values with `Z` or a numeric offset and Apache Combined timestamps. Timestamps without an explicit timezone or otherwise invalid are counted as unknown. The timeline makes a second streaming pass over the input. `--follow` waits for appended complete lines, detects file truncation and rotation, and handles Ctrl-C. `--export` streams selected records to a text or CSV file. Use `--help` or `--version` to inspect the built program.

The command form below documents the current CLI options. Platform validation and release packaging remain in progress.

## 2. Command form

```text
logview <file> [--format <name>] [--pattern <pattern>]
               [--filter <expression>] [--regex <pattern>]
               [--regex-field <field>=<pattern>]
               [--stats | --export text|csv | --tui]
               [--output <path>] [--columns <fields>]
               [--follow] [--from-start] [--flush-partial]
               [--max-line-bytes <n>] [--force]
```

The default is batch text output and `raw` input. Format names are `raw`, `syslog3164`, `syslog5424`, `apache-common`, `apache-combined`, `nginx-combined`, `jsonl`, `custom`, and `auto`. Auto detection samples up to 32 complete lines or 256 KiB and falls back to raw if no format is confidently detected. `--pattern` requires `--format custom`. `--stats`, `--export`, and `--tui` choose distinct primary output modes in the 1.0 target. `--follow` cannot be combined with `--stats` or `--export` in 1.0. `--tui` needs an interactive terminal.

### Basic examples

```text
logview app.log --format custom --pattern "%{TIMESTAMP} %{LEVEL} %{MESSAGE}"
logview app.log --format jsonl --filter "level=ERROR"
logview app.log --format jsonl --filter "level=ERROR" --regex "timeout"
logview access.log --format apache-combined --filter "status=500 or status=502"
logview syslog.log --format syslog5424 --stats
logview app.log --follow --format jsonl --filter "level=ERROR"
logview app.log --follow --from-start --flush-partial
logview app.log --format jsonl --tui
logview app.log --format jsonl --follow --tui
```

Quote expressions for the shell. On Windows PowerShell and Unix shells, double quotes around a simple expression work as shown. Escaping embedded quotes differs by shell; place complex expressions in single quotes where supported and verify how your shell passes them.

## 3. Choose a format

| Input example | Select | Result |
| --- | --- | --- |
| `Oct  8 09:15:00 host app[123]: message` | `syslog3164` | Time, host, process, PID, and message. No year or time zone is inferred. |
| `<34>1 2026-10-08T09:15:00Z host app 123 ID47 - message` | `syslog5424` | RFC 5424 header fields and message. |
| `192.0.2.4 - - [08/Oct/2026:09:15:00 +0200] "GET / HTTP/1.1" 200 42 "-" "agent"` | `apache-combined` or `nginx-combined` | Client IP, request, status, bytes, referrer, agent. |
| `{"time":"2026-10-08T09:15:00Z","level":"ERROR","message":"timeout"}` | `jsonl` | Canonical timestamp, level, message plus original keys. |
| `2026-10-08 09:15:00 ERROR timeout` | `custom` | Fields selected by the supplied pattern. |

The `jsonl` preset accepts one JSON object per line and a top-level array containing pretty-printed objects. Each object is one searchable record, even when it spans multiple physical lines. Nested objects and arrays are parsed to a maximum nesting depth of 64; primitive values are addressable by dotted paths such as `employee.department.manager.contact.phone`. A leaf alias such as `phone` also works when that leaf name is unique in the record. Each record has a 16 MiB maximum size and up to 64 extracted fields. Malformed records remain available to raw-line regex search. Docker JSON-file records in an exported copy can map `time` to timestamp and `log` to message. Docker advises against interacting directly with Docker-managed JSON log files; use a copy or supported export route. [Docker documentation](https://docs.docker.com/engine/logging/drivers/json-file/)

For example, to find employee phone numbers beginning with `829` in a pretty-printed JSON file:

```text
logview employees_5-level_1-GB_formatted.json --format jsonl --regex-field "phone=^829"
```

To target the full nested path explicitly:

```text
logview employees_5-level_1-GB_formatted.json --format jsonl --regex-field "employee.department.manager.contact.phone=^829"
```

For a custom text format, supply placeholders in their literal order:

```text
logview app.log --format custom --pattern "%{TIMESTAMP:ts} %{LEVEL:lvl} %{MESSAGE:msg}"
```

Available placeholders are `TIMESTAMP`, `LEVEL`, `MESSAGE`, `FIELD:name`, `MONTH`, `DAY`, `TIME`, `HOST`, `PROC`, and `PID`. A token alias such as `%{TIMESTAMP:ts}` maps to the canonical `timestamp` field. `%{FIELD:name}` creates a field with that name. `MESSAGE` must be the final placeholder and captures the remainder. Variable placeholders need a literal delimiter between them.

## 4. Filter records

`--filter` compares named fields. Operators are `=` for equality, `!=` for inequality, and `~` for regex search. Combine clauses with `and`, `or`, and parentheses; `and` takes precedence. Field names and equality values are case-sensitive. A missing field makes a predicate false, including `!=`.

```text
logview app.log --format jsonl --filter "level=ERROR and service=api"
logview app.log --format jsonl --filter "level=ERROR and (message~timeout or request_id=ab12)"
logview access.log --format apache-combined --filter "status=500 or status=503"
```

`--regex` searches the complete raw line. `--regex-field` searches one parsed field. These options combine with `--filter` using AND. The current CLI accepts each option once.

```text
logview app.log --format jsonl --filter "level=ERROR" --regex "timeout"
logview app.log --format jsonl --regex-field "message=timeout"
logview access.log --format apache-combined --regex-field "client_ip=^192\.0\.2\."
```

The internal regex syntax supports literals, `.`, `*`, `+`, `?`, classes and ranges such as `[0-9]`, negated classes, `\d`, `\w`, `\s`, anchors `^` and `$`, groups, and `|`. It does not support lookaround, backreferences, lazy quantifiers, or `{m,n}`. Matching is case-sensitive and byte-oriented; `\d`, `\w`, and `\s` are ASCII classes. Invalid regexes return exit code 3. The expression `message~timeout` uses the same regex engine on `message`.

## 5. Follow a growing file

```text
logview --follow app.log --format jsonl --filter "level=ERROR"
logview --follow --from-start app.log --format custom --pattern "%{TIMESTAMP} %{LEVEL} %{MESSAGE}"
```

Follow begins at the end by default; `--from-start` reads the existing content first. New records appear after their newline arrives. A truncated file restarts at byte zero. If the path is replaced by rotation, the tool drains complete records available on the old handle, then starts the new file at byte zero after detecting the replacement. A rapid rotation sequence can lose lines; this mode is intended for interactive monitoring, not guaranteed collection. Press Ctrl-C to stop. `--flush-partial` emits a final incomplete line on interruption. Follow exits with status 0 on Ctrl-C and cannot be combined with `--stats` or `--export`.

## 6. Export results

```text
logview app.log --format jsonl --filter "level=ERROR" --export text --output errors.log
logview access.log --format apache-combined --filter "status=500" --export csv --output errors.csv
logview app.log --format jsonl --export csv --columns "line_number,timestamp,level,request_id,message" --output report.csv
```

Text export writes each selected record's original content followed by LF. CSV defaults to `line_number,timestamp,level,message,raw`; `--columns` sets a fixed list. Missing fields become empty cells. CSV quotes commas, quotes, and line breaks and replaces invalid UTF-8 with U+FFFD. Existing output files are protected unless `--force` is supplied. The output path cannot refer to the input file. Exports are streamed and may be partially written if an I/O error occurs; the command then returns 2. An empty selection returns 1.

## 7. View statistics

```text
logview app.log --format jsonl --stats
```

The `--stats` output includes exact physical, parsed, rejected, and selected record counts and exact level counts for selected records. Missing levels are counted as `UNSPECIFIED`. A fixed 256-bin timeline spans the observed unambiguous timestamp range. Timestamp values without an explicit timezone or in unsupported formats are reported under `Unknown timestamps`. The `Frequent field values (estimated)` section reports up to ten leading candidates maintained in fixed memory; the count is approximate and includes an error bound. Statistics cannot be combined with export.

## 8. Terminal interface

```text
logview app.log --format jsonl --tui
```

| Key | Action |
| --- | --- |
| Up/Down | Move one row. |
| Page Up/Page Down | Move one screen. |
| Home/End | Go to the first/last selected row in the scanned file. |
| `J` | Enter a one-based source line number. If that line is filtered out, go to the next selected record; if none follows, go to the last selected record. |
| `/` | Enter a regex search. Press Enter to jump to its first match. |
| Enter / Shift+Enter | Go to the next/previous search match. |
| Esc during search navigation | Cancel a long search. Progress appears in the status line. |
| `n` / `N` | Also go to the next/previous search match. |
| `e` / `E` | Go to the next/previous error-level record. |
| `w` / `W` | Go to the next/previous warning-level record. |
| `c` | Show or hide four physical records before and after the selected record. |
| `C` | Clear the search, field filter, raw regex, and field regex, then rescan the full input. |
| `z` | Open a scrollable full-screen view of the complete selected record. Use arrows, Page Up/Page Down, Home/End to scroll; press `z`, Enter, or Esc to close. |
| `f` | Edit the field filter and rescan from the beginning. |
| `r` | Edit the raw-line regex and rescan from the beginning. |
| `x` | Export records matching the active filters and search. Press Enter at the path prompt for a numbered filename in the current directory. |
| `?` | Show help. |
| `q` | Quit; in follow mode, stop waiting for new records. |

Use `--follow --tui` to scan the file, then wait for appended complete records. A final incomplete line stays pending until its newline arrives. File rotation or truncation clears the current view and rescans the replacement file. `--flush-partial` is not supported in TUI follow mode. Error and warning jumps use the parsed `level` field when available; HTTP 5xx responses are treated as errors and 4xx responses as warnings. The status line shows scan state and search text. During the initial scan, a spinner and physical/selected record counters update every 65,536 physical records; the result rows appear when scanning completes. The TUI redraws its viewport after a terminal resize. It keeps a 256-row memory cache and stores navigation checkpoints in temporary files so it can reload rows and context from anywhere in the scanned file. The temporary directory must have enough free space for the indexes. Display text is limited to 1 KiB per row and the available terminal width; long previews are clipped inline. Multiline JSON whitespace is compacted to keep each record on one screen row. Searches inspect the stored preview. Search matches are highlighted by row. Press `x` after scanning to export records matching the active filters and current search. Enter a path ending in `.csv` for CSV output; any other extension or no extension writes text. Press Enter without a path to save in the current directory using the input filename with `_1`, `_2`, and increasing numbers before its extension, skipping existing files. For example, `app.log` becomes `app_1.log`, then `app_2.log`. Changing a filter or raw regex rescans the file. Log control characters are displayed safely, without executing terminal escape sequences.

## 9. Exit codes and troubleshooting

| Code | Meaning | Suggested action |
| --- | --- | --- |
| 0 | Successful match, summary, or normal interactive exit | None. |
| 1 | No records matched a snapshot selection | Relax or inspect the filter. |
| 2 | I/O or terminal environment error | Check paths, permissions, disk space, and terminal support. |
| 3 | Invalid option, format pattern, filter, or regex | Check syntax and supported operators. |

If fields are unexpectedly missing, select the exact format rather than `auto` and inspect the raw line. If an HTTP access log has a custom `LogFormat` or `log_format`, use a custom pattern or adapt the log producer. If a line is rejected, check the 16 MiB cap, JSON nesting, malformed escapes, or a format mismatch. For a direct native journal or Windows event store, export the data to a supported text or JSON line format first.

## 10. Performance and limits

The design target is bounded batch memory and a 1 GB plain scan in less than 5 seconds on a documented reference machine. This has not been measured or achieved yet. Regex and parsing work add cost. The 16 MiB per-line cap, JSON-object subset, 4096-state regex cap, single-file scope, and no native compressed or binary log support are deliberate v1 limits. See [Technical Specification](TECHNICAL_SPECIFICATION.md) for precise behavior and the benchmark protocol.
