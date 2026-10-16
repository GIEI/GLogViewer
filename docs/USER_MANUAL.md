# User Manual

**Status:** GLogViewer 1.9.0 adds Linux/macOS GUI builds and headless startup checks to CI. The optional GUI still requires SDL3 and is built separately from the CLI/TUI release archives. See the [release process](RELEASE_PROCESS.md).

## 1. What the tool will do

`logview` reads local line-oriented log files and prints selected records. The current CLI supports raw, JSON Lines, syslog RFC 3164/5424, Kubernetes CRI, Apache/Nginx access logs, custom patterns, format detection, text/CSV export, statistics, follow mode, and an interactive terminal browser. Batch mode can merge up to 16 files by timestamp. The input is read-only. Binary systemd journals and native Windows Event Log files must first be exported to a supported line-oriented form.

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

The CLI accepts one or more file paths and these options:

```text
logview <file> [<file> ...] [--format FORMAT] [--pattern PATTERN] [--filter EXPRESSION]
                [--regex pattern] [--regex-field field=pattern]
                [--exclude-regex pattern]
                [--stats [--stats-field FIELD] | --export text|csv --output path]
                [--columns list] [--force]
                [--follow [--from-start] [--flush-partial]]
                [--top <count> | --bottom <count>]
                [--record first|last]
                [--max-line-bytes count]
```

`--format raw` is the default. Formats include `raw`, `jsonl`, `syslog3164`, `syslog5424`, `cri`, `apache-common`, `apache-combined`, `apache-error`, `nginx-combined`, `custom`, and `auto`. `--filter` accepts equality, inequality, regex, and lexical range comparisons combined with `and`, `or`, and parentheses. `--regex` searches the entire raw line; `--regex-field` searches a parsed field; `--exclude-regex` removes records whose raw line matches its pattern. Positive and exclusion regexes combine with each other and with `--filter` using AND. Matching is case-sensitive. `--stats` prints exact record and level counts, a 256-bin timestamp timeline, and frequent field/value pairs, marking each count exact or estimated. Add `--stats-field FIELD` to show frequent values only for a particular parsed field; it also enables stats mode by itself. Timestamp aggregation recognizes ISO/RFC 3339 values with `Z` or a numeric offset, Apache access timestamps, and Apache Error timestamps. Apache Error timestamps have no timezone; the timeline interprets them as UTC. RFC 3164 timestamps need `--syslog-year YYYY`; their timezone defaults to UTC and can be set with `--syslog-timezone +/-HH:MM`. Dates without enough information to interpret them remain unknown. The timeline makes a second streaming pass over the input. `--follow` waits for appended complete lines, detects file truncation and rotation, and handles Ctrl-C. `--export` streams selected records to a text or CSV file. Use `--help` or `--version` to inspect the built program.

The command form below documents the current CLI options. See the [release process](RELEASE_PROCESS.md) for platform archive details and verification.

## 2. Command form

```text
logview <file> [--format <name>] [--pattern <pattern>]
               [--filter <expression>] [--regex <pattern>]
               [--regex-field <field>=<pattern>]
               [--exclude-regex <pattern>]
               [--stats | --export text|csv | --tui]
               [--output <path>] [--columns <fields>]
               [--follow] [--from-start] [--flush-partial]
               [--top <count> | --bottom <count>] [--record first|last]
               [--max-line-bytes <n>] [--force]
```

Multi-file batch runs use one format for every source, unless `--format auto` is selected. They merge by timestamp and label each record with its source path and source line. TUI and follow remain single-file modes.

The default is batch text output and `raw` input. Format names are `raw`, `syslog3164`, `syslog5424`, `cri`, `apache-common`, `apache-combined`, `apache-error`, `nginx-combined`, `jsonl`, `custom`, and `auto`. Auto detection samples up to 32 complete lines or 256 KiB and falls back to raw if no format is confidently detected. `--pattern` requires `--format custom`. `--stats`, `--export`, and `--tui` choose distinct primary output modes. `--follow` cannot be combined with `--stats` or `--export`. `--tui` needs an interactive terminal.

### Basic examples

```text
logview app.log --format custom --pattern "%{TIMESTAMP} %{LEVEL} %{MESSAGE}"
logview app.log --format jsonl --filter "level=ERROR"
logview app.log --format jsonl --filter "level=ERROR" --regex "timeout"
logview app.log --exclude-regex "error"
logview app.log --regex "timeout" --exclude-regex "connection refused"
logview access.log --format apache-combined --filter "status=500 or status=502"
logview syslog.log --format syslog5424 --stats
logview syslog.log --format syslog3164 --syslog-year 2025 --syslog-timezone +02:00 --stats
logview app.jsonl --format jsonl --stats-field service
logview app.log --follow --format jsonl --filter "level=ERROR"
logview app.log --follow --from-start --flush-partial
logview app.log --format jsonl --tui
logview app.log --format jsonl --follow --tui
logview app.log --format jsonl --top 10
logview app.log --format jsonl --bottom 10 --tui
logview app.log --format jsonl --record first
logview app.log --format jsonl --record last
logview api-a.jsonl api-b.jsonl --format jsonl --filter "level=ERROR" --export csv --output merged.csv
logview service-a.log service-b.log --format auto --stats
```

### Merge multiple files

Batch mode accepts up to 16 file paths. Input argument order breaks ties between equal timestamps, while records from one source retain their source-line order. Each file must be nondecreasing by every parseable timestamp; the command stops with exit code 2 if a source timestamp moves backward. Records with no supported timestamp follow all timestamped records, grouped by input file and then source-line order. Text output prefixes each record with `[path:line]`. CSV output always includes a `source_file` column, appending it when `--columns` omits it.

`--format auto` can detect the format independently for each source; any explicit format applies to all inputs. Multi-file runs do not accept directories, duplicate file identities, stdin (`-`), TUI, follow, `--top`, `--bottom`, or `--record`. Rows without timestamps are staged in temporary files, so they may require disk space proportional to their combined size even though heap use stays bounded.

RFC 3164 timestamps contain a month, day, and time, but no year or timezone. To include them in batch timelines and the TUI histogram, supply the year represented by the file with `--syslog-year`. The default assumed timezone is UTC; use `--syslog-timezone` to provide the source's fixed UTC offset, such as `+02:00` or `-07:00`. The offset means local time relative to UTC. The year is applied uniformly to every RFC 3164 timestamp; files spanning a year boundary should be split because rollover is not inferred. These options require `--format syslog3164` or `--format auto` and do not change the original timestamp text or range-filter comparisons.

Quote expressions for the shell. On Windows PowerShell and Unix shells, double quotes around a simple expression work as shown. Escaping embedded quotes differs by shell; place complex expressions in single quotes where supported and verify how your shell passes them.

## 3. Choose a format

| Input example | Select | Result |
| --- | --- | --- |
| `Oct  8 09:15:00 host app[123]: message` | `syslog3164` | Time, host, process, PID, and message. Add `--syslog-year` and, when needed, `--syslog-timezone` for timeline analysis. |
| `<34>1 2026-10-08T09:15:00Z host app 123 ID47 - message` | `syslog5424` | RFC 5424 header fields and message. |
| `2026-10-12T09:15:00.123456789Z stdout F application started` | `cri` | Timestamp, `stdout`/`stderr`, fragment tag, and message. |
| `192.0.2.4 - - [08/Oct/2026:09:15:00 +0200] "GET / HTTP/1.1" 200 42 "-" "agent"` | `apache-combined` or `nginx-combined` | Client IP, request, status, bytes, referrer, agent. |
| `[Mon Feb 27 05:41:08 2006] [error] [client 192.0.2.4] File not found` | `apache-error` | Time, level, client source, and message; module and client IP when present. |
| `{"time":"2026-10-08T09:15:00Z","level":"ERROR","message":"timeout"}` | `jsonl` | Canonical timestamp, level, message plus original keys. |
| `2026-10-08 09:15:00 ERROR timeout` | `custom` | Fields selected by the supplied pattern. |

Apache Common and Combined are **access log** formats. Apache Error logs start with `[weekday month day time year] [level]` and need `--format apache-error` or `--format auto`. The GUI detects this mismatch and switches from an Apache access preset to Apache Error. Bare continuation lines remain visible as raw text, without invented fields. For example, `logview SAMPLES/Apache.log --format apache-error --filter "level=error" --top 10` displays the first ten errors.

The `cri` preset parses Kubernetes/containerd records in the form `<RFC3339Nano timestamp> <stdout|stderr> <tag> <message>`. Use `--filter "stream=stderr"` or `--filter "tag=P"` to inspect one stream or partial fragments; `tag=F` marks a complete line. Each physical line remains a separate record in this version, so `P` fragments are not joined automatically. The original CRI line remains intact in text output and exports. `--format auto` detects CRI records, and CRI timestamps are included in `--stats` and multi-file ordering.

```text
logview pod-container.log --format cri --filter "stream=stderr and tag=F" --regex "connection refused"
logview pod-container.log --format auto --stats
```

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

`--filter` compares named fields. Operators are `=` for equality, `!=` for inequality, `~` for regex search, and `<`, `<=`, `>`, `>=` for bytewise lexical comparisons. Combine clauses with `and`, `or`, and parentheses; `and` takes precedence. Field names and values are case-sensitive. A missing field makes a predicate false, including `!=` and range comparisons.

```text
logview app.log --format jsonl --filter "level=ERROR and service=api"
logview app.log --format jsonl --filter "level=ERROR and (message~timeout or request_id=ab12)"
logview access.log --format apache-combined --filter "status=500 or status=503"
logview events.jsonl --format jsonl --filter "timestamp>=2026-10-01T00:00:00Z and timestamp<2026-11-01T00:00:00Z"
logview SAMPLES/crypto_historical_365days_1-level_10-MB_minified.json --format jsonl --filter 'date>=2024-12-10 and date<2024-12-16'
```

The crypto sample has both a `date` field (`YYYY-MM-DD`) and a `timestamp` field (`YYYY-MM-DD HH:MM:SS`). This half-open interval includes December 10 through December 15, 2024, and excludes December 16. Running the example returns six records. The `date` field keeps the filter argument simple in PowerShell because its values contain no spaces. To filter on `timestamp`, quote each value inside the filter expression: `timestamp>="2024-12-10 00:00:00" and timestamp<"2024-12-16 00:00:00"`.

### Query profiles

Save the effective format and query settings from a command in a versioned profile, then reuse them for batch output or the TUI:

```sh
logview app.jsonl --format jsonl --filter 'level=ERROR' --regex-field 'message=timeout' --save-profile incident.glogprofile
logview app.jsonl --profile incident.glogprofile
logview app.jsonl --profile incident.glogprofile --regex-field 'message=database'
logview app.jsonl --profile incident.glogprofile --tui
```

Explicit command-line settings override values loaded from `--profile`. Profile files use the `GLOGVIEW_PROFILE 1` header and plain UTF-8 `key=value` lines. Unknown, duplicate, malformed, or unsupported settings are rejected. Profiles support format, custom pattern, field filter, raw regex, field regex, excluded raw-line regex, RFC 3164 year/time zone, and TUI context sizes. A saved profile is never replaced by default; use `--profile-overwrite` together with `--save-profile` to replace it. Profile values cannot contain line breaks.

Range comparisons compare field bytes in lexical order; they do not parse dates or numbers. Fixed-width ISO 8601 timestamps with the same timezone representation sort chronologically. For timestamps with different UTC offsets, normalize the values first. For numbers, use equal-width zero-padded values (for example, `00042`) or a regex suited to the field format.

`--top N` emits the first N records that match active filters. `--bottom N` emits the last N matches in original file order; batch mode stores selected content in a temporary file, so temporary disk space grows with the selected input size. Both options work with batch output and `--tui`; batch `--top` also supports export. `--bottom` cannot be combined with export, and limited TUI views cannot be exported with `x`. Neither option can be combined with follow or stats. `N` must be between 1 and 1,000,000.

`--record first` and `--record last` print the complete first or last record matching active filters. For JSON input this includes the original multiline record, useful for inspecting nested keys and timestamp formats. These options cannot be combined with TUI, export, stats, follow, or a result limit.

`--regex` searches the complete raw line. `--regex-field` searches one parsed field. `--exclude-regex` rejects records whose complete raw line matches. These options combine with `--filter` using AND. The current CLI accepts each option once. The TUI applies command-line exclusions during initial indexing, rescans, follow, navigation, and export; press `C` to clear them with the other filters.

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

The `--stats` output includes exact physical, parsed, rejected, and selected record counts and exact level counts for selected records. Missing levels are counted as `UNSPECIFIED`. A fixed 256-bin timeline spans the observed unambiguous timestamp range. Timestamp values without an explicit timezone or in unsupported formats are reported under `Unknown timestamps`. Frequent field/value candidates use a fixed-memory Space-Saving table. Counts with zero error are marked `(exact)`; other candidates are marked `(estimated; error <= N)`. `--stats-field FIELD` limits this section to one field while retaining the record counts, level counts, and timeline. Statistics cannot be combined with export.

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
| `/` | Enter a regex search or a Boolean expression of quoted literal terms. Press Enter to jump to its first match. |
| Enter / Shift+Enter | Go to the next/previous search match. |
| Esc during search navigation | Cancel a long search. Progress appears in the status line. |
| `n` / `N` | Also go to the next/previous search match. |
| `e` / `E` | Go to the next/previous error-level record. |
| `w` / `W` | Go to the next/previous warning-level record. |
| `c` | Show or hide the configured number of physical records before and after the selected record. |
| `b` / `B` | Toggle a bookmark on the selected source line / jump to the next bookmark in source order. Bookmarked rows are shown in light green. |
| `+` / `-` | Increase / decrease both configured context radii. |
| `C` | Clear the search, field filter, raw regex, and field regex, then rescan the full input. |
| `z` | Open a scrollable full-screen view of the complete selected record. Use arrows, Page Up/Page Down, Home/End to scroll; press `z`, Enter, or Esc to close. |
| `v` | Enter a parsed field name and show up to ten frequent values among records selected by the active field filter and regex predicates. Counts are marked exact or estimated. Press Esc to cancel the scan. |
| `h` | Build a 256-bin time histogram for selected records. Time runs from oldest at the left to newest at the right, in UTC. The vertical labels are relative volume levels, not months or days; the tallest displayed column is labeled with its record count. `#`, `+`, and `!` mark normal, warning, and error records. Each screen column may group adjacent time buckets. The selected bucket shows its exact record, error, and warning counts plus its UTC time span. Use Left/Right or Up/Down to move between nonempty bins, Home/End for the first/last bin, and Enter to jump to the first matching record in the chosen bin. Unsupported or ambiguous timestamps are counted as unknown. For RFC 3164, launch with `--syslog-year YYYY` and optional `--syslog-timezone +/-HH:MM`. |
| `f` | Edit the field filter and rescan from the beginning. |
| `r` | Edit the raw-line regex and rescan from the beginning. |
| `x` | Export records matching the active filters and search. Press Enter at the path prompt for a numbered filename in the current directory. |
| `?` | Show help. |
| `q` | Quit; in follow mode, stop waiting for new records. |

Use `--follow --tui` to scan the file, then wait for appended complete records. A final incomplete line stays pending until its newline arrives. File rotation or truncation clears the current view and rescans the replacement file. `--flush-partial` is not supported in TUI follow mode. Error and warning jumps use the parsed `level` field when available; HTTP 5xx responses are treated as errors and 4xx responses as warnings. The status line shows scan state and search text. During the initial scan, a spinner and physical/selected record counters update every 65,536 physical records; the result rows appear when scanning completes. The TUI redraws its viewport after a terminal resize. It keeps a 256-row memory cache and stores navigation checkpoints in temporary files so it can reload rows and context from anywhere in the scanned file. The temporary directory must have enough free space for the indexes. Display text is limited to 1 KiB per row and the available terminal width; long previews are clipped inline. Multiline JSON whitespace is compacted to keep each record on one screen row. Searches inspect the stored preview. Search matches are highlighted by row. Press `x` after scanning to export records matching the active filters and current search. Enter a path ending in `.csv` for CSV output; any other extension or no extension writes text. Press Enter without a path to save in the current directory using the input filename with `_1`, `_2`, and increasing numbers before its extension, skipping existing files. For example, `app.log` becomes `app_1.log`, then `app_2.log`. Changing a filter or raw regex rescans the file. Log control characters are displayed safely, without executing terminal escape sequences.

Press `b` to bookmark the selected record's physical source line and `B` to jump to the next bookmark in source order, wrapping to the first. Up to 32 bookmarks are kept for the current TUI session. They refer to source lines, so navigation and search do not change their meaning. A filter or regex rescan clears bookmarks because the selected result set changed; file rotation also clears them. Bookmarks are not persisted after quitting.

Set context size at launch with `--context-before N` and `--context-after N` (0–100; default 4 for each). Press `c` to show or hide context, and `+`/`-` to increase or decrease both values while browsing. Context includes physical source records even when filters exclude them; it does not change filtering or export.

With `--top N` or `--bottom N`, the TUI shows only the first or last N selected records. Source-line jump (`J`) is disabled in this limited view because the displayed sequence does not represent the complete file.

The `v` field summary scans the complete current filtered result set and is available after the initial scan; it does not include the separate interactive search string. The `h` histogram requires a complete scan of a seekable file without `--top` or `--bottom`; it makes two additional streaming passes and is unavailable during follow. If the selected record is JSON, `z` displays a pretty-printed version when the formatted output is within the 16 MiB record limit. Malformed JSON, formatting errors, or larger formatted output fall back to the source text. Filtering, search, and export continue to use original input data.

The `/` prompt accepts the built-in regex syntax or a Boolean search expression. In Boolean mode, write literal terms in double quotes and combine them with `AND`, `OR`, `NOT`, and parentheses. Operators are case-insensitive; precedence is `NOT`, then `AND`, then `OR`. For example, `/"66.249" AND "20/May"` matches rows containing both literal strings in either order. `/"timeout" AND NOT "retry"` excludes rows containing `retry`, and `/("ERROR" OR "FATAL") AND "database"` combines alternatives. Terms are case-sensitive. An unquoted `AND`, `OR`, or `NOT` selects Boolean mode; otherwise input remains a regular expression. Escape regex metacharacters when searching for them literally, such as `\[` or `\.`.

## 9. Desktop GUI

The optional desktop interface uses Nuklear and SDL3. Build it separately from the dependency-free CLI/TUI.

On Windows, the build script finds an extracted SDL3 Visual C development package in `third_party\SDL3` or in the user's local temporary directory:

```powershell
.\build.bat gui
.\build\logview-gui.exe SAMPLES\Linux_1GB.syslog --format syslog3164
.\build\logview-gui.exe SAMPLES\Linux_1GB.syslog --format auto --syslog-year 2026
.\build\logview-gui.exe SAMPLES\Apache.log --format apache-error --filter "level=error"
```

The GUI also accepts `--syslog-year YYYY` and `--syslog-timezone +/-HH:MM` for RFC 3164 timestamp analysis. If omitted, the GUI assumes the current year and UTC.

If SDL3 is elsewhere, set `$env:SDL3_DIR` to the directory containing `include\SDL3\SDL.h` and `lib\x64\SDL3.lib` before building. For example: `$env:SDL3_DIR = "C:\Libraries\SDL3-3.4.18"`.

On macOS or Linux, install SDL3 development files and `pkg-config`, then run `make gui` and launch `build/logview-gui SAMPLES/Linux_1GB.syslog --format syslog3164`.

The GUI uses a three-pane log explorer layout: source and filters on the left, matching records in the center, and selected-record details on the right. The table shows timestamp, level, source, and message. Column widths follow the visible values, with the message using the remaining space. Choose a format and enter filters in the left pane, enter a search query in the top bar, then select **Scan / Apply**. Severity checkboxes for TRACE, DEBUG, INFO, NOTICE, WARN, ERROR, and OTHER rescan the file when changed; FATAL and CRITICAL records use ERROR. The search field accepts the TUI Boolean syntax with quoted literal terms, `AND`, `OR`, `NOT`, and parentheses. Separate inputs apply a raw-line regex, a `field=pattern` regex, and an exclusion regex. The field filter supports the same field expressions as the CLI. For custom formats, choose **Custom pattern** and enter a `%{TOKEN:name}` pattern.

Use **Previous/Next match** to move through selected records and **Previous/Next error** or **Previous/Next warning** to navigate by severity. The result list is paged in groups of 64 across the full selected result set. **First**, **Previous**, **Next**, and **Last** move between pages. Enter a one-based source line and choose **Jump** to reach the nearest matching record. Toggle bookmarks on the selected record and use **Next bookmark** to visit them.

The details pane has **Structured fields**, **Raw record**, and **Context** tabs. The structured view shows the message and parsed name/value pairs. The raw view reads the complete selected record in 1 KiB pages; for JSON Lines, **Format JSON** toggles a readable formatted view. Context controls adjust the number of nearby physical source lines. Enter a field name beside **Field values** to focus the frequency summary; leave it empty to summarize all fields. **Time histogram** shows UTC time buckets with counts, and selecting a bucket jumps to its first matching record. RFC 3164 statistics use the current year and UTC as the assumed timezone.

Choose **Export** to write the complete active result set as text or CSV. The GUI scans again during export and remains responsive; the export can be cancelled. Select **Follow** before scanning to read existing records and continue monitoring, or enable it after a completed scan to watch for appended records. File replacement or truncation triggers a rescan. Setup, scan, and total ready times appear below the results along with byte progress.

The GUI handles one file at a time and does not yet provide the CLI/TUI `--top` and `--bottom` result limits. Use **Load profile** and **Save profile** in the sidebar to reuse the shared `GLOGVIEW_PROFILE 1` settings for format, custom pattern, field filter, raw regex, field regex, excluded regex, RFC 3164 year/time zone, and context sizes. Loading a profile clears the GUI-only Boolean search and severity selections; those settings are not stored in the shared profile format. Pass `--syslog-year YYYY` and optionally `--syslog-timezone +/-HH:MM` at launch to configure RFC 3164 time histogram analysis; the defaults are the current year and UTC. Open **Visible columns** in the sidebar to show or hide Timestamp, Level, and Source; Message always stays visible. For CSV export, enter a comma-separated **CSV columns** list; leave it empty to use the default columns. In the Context tab, set the before and after radii separately or adjust both together. Column visibility, context sizes, export format, CSV columns, and window dimensions are saved in a separate per-user preferences file and restored on the next launch.

## 10. Exit codes and troubleshooting

| Code | Meaning | Suggested action |
| --- | --- | --- |
| 0 | Successful match, summary, or normal interactive exit | None. |
| 1 | No records matched a snapshot selection | Relax or inspect the filter. |
| 2 | I/O or terminal environment error | Check paths, permissions, disk space, and terminal support. |
| 3 | Invalid option, format pattern, filter, or regex | Check syntax and supported operators. |

If fields are unexpectedly missing, select the exact format rather than `auto` and inspect the raw line. If an HTTP access log has a custom `LogFormat` or `log_format`, use a custom pattern or adapt the log producer. If a line is rejected, check the 16 MiB cap, JSON nesting, malformed escapes, or a format mismatch. For a direct native journal or Windows event store, export the data to a supported text or JSON line format first.

## 11. Performance and limits

The design target is bounded batch memory and a 1 GB plain scan in less than 5 seconds on a documented reference machine. This has not been measured or achieved yet. Regex and parsing work add cost. The 16 MiB per-line cap, JSON-object subset, 4096-state regex cap, single-file scope, and no native compressed or binary log support are deliberate v1 limits. See [Technical Specification](TECHNICAL_SPECIFICATION.md) for precise behavior and the benchmark protocol.
