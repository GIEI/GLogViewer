# User Manual

**Status:** the batch CLI includes the 0.1 MVP and current 0.2 structured-format work. Statistics, export, follow, and TUI sections describe features planned for later milestones. The [roadmap](ROADMAP.md) gives implementation status and release gates.

## 1. What the tool will do

`logview` reads a local line-oriented log file and prints selected records. The current batch CLI supports raw, JSON Lines, syslog RFC 3164/5424, Apache/Nginx access logs, custom patterns, and format detection. The input is read-only. Later milestones add interactive browsing, follow, statistics, and export. Binary systemd journals and native Windows Event Log files must first be exported to a supported line-oriented form.

## Build and options

On Windows, run `build.bat` with Visual Studio C++ Build Tools installed. On macOS/Linux, run `make` with a C99 compiler. The command-line test suite is `python tests/test_cli.py build/logview.exe` on Windows or `make test` on macOS/Linux.

The current CLI supports one file and these options:

```text
logview <file> [--format FORMAT] [--pattern PATTERN] [--filter EXPRESSION]
                [--regex pattern] [--regex-field field=pattern]
                [--max-line-bytes count]
```

`--format raw` is the default. Formats include `raw`, `jsonl`, `syslog3164`, `syslog5424`, `apache-common`, `apache-combined`, `nginx-combined`, `custom`, and `auto`. `--filter` accepts equality, inequality, and field regex predicates combined with `and`, `or`, and parentheses. `--regex` searches the entire raw line; `--regex-field` searches a parsed field. These options are combined with `--filter` using AND. Use `--help` or `--version` to inspect the built program.

The 1.0 command form and feature descriptions below include options that are not implemented yet.

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
logview app.log --format jsonl --tui
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

The `jsonl` preset accepts only a top-level object with primitive values, up to 64 unique keys and a 16 MiB line cap. Nested objects and arrays are rejected in v1. A malformed record can still be searched by raw-line regex. Docker JSON-file records in an exported copy can map `time` to timestamp and `log` to message. Docker advises against interacting directly with Docker-managed JSON log files; use a copy or supported export route. [Docker documentation](https://docs.docker.com/engine/logging/drivers/json-file/)

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

Follow begins at the end by default; `--from-start` reads the existing content first. New records appear after their newline arrives. A truncated file restarts at byte zero. If the path is replaced by rotation, the tool drains complete lines from the old handle and starts the new file at byte zero after detecting the replacement. A rapid rotation sequence can lose lines; this mode is intended for interactive monitoring, not guaranteed collection. Press Ctrl-C to stop. `--flush-partial` emits a final incomplete line on normal interruption.

## 6. Export results

```text
logview app.log --format jsonl --filter "level=ERROR" --export text --output errors.log
logview access.log --format apache-combined --filter "status=500" --export csv --output errors.csv
logview app.log --format jsonl --export csv --columns "line_number,timestamp,level,request_id,message" --output report.csv
```

Text export writes the original selected lines. CSV defaults to `line_number,timestamp,level,message,raw`; `--columns` sets a fixed list. Missing fields become empty cells. CSV quotes commas, quotes, and line breaks. Existing output files are protected unless `--force` is supplied. Exports are streamed and may be partially written if an I/O error occurs; the command then returns 2. An empty selection returns 1.

## 7. View statistics

```text
logview app.log --format jsonl --stats
```

Statistics include physical, parsed, rejected, and selected record counts; counts by level; a 256-bin timestamp timeline; and frequent field/value candidates. Record and level counts are exact. Frequent-value counts are bounded estimates and are labelled `estimated`. Timestamps that cannot be placed unambiguously on a timeline appear as `UNKNOWN_TIME`. Statistics are a snapshot and cannot be combined with follow mode in v1.

## 8. Terminal interface

```text
logview app.log --format jsonl --tui
```

| Key | Proposed action |
| --- | --- |
| Up/Down | Move one visible row. |
| Page Up/Page Down | Move one viewport. |
| Home/End | Go to beginning/end of the selected view. |
| `/` | Edit an incremental regex search. |
| `n` / `N` | Go to next/previous search match. |
| `f` | Edit the field filter. |
| `r` | Edit the raw-line regex. |
| `s` | Show summary statistics. |
| `?` | Show help. |
| `q` | Quit. |

The status line shows format, progress, active filters, and rejected records. Search matches are highlighted. Changing a filter rescans the file; the visible result count is incomplete until scanning finishes. Long jumps may take time because the TUI keeps a bounded cache and seeks from checkpoints rather than storing every result. Log control characters are displayed safely, without executing terminal escape sequences.

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
