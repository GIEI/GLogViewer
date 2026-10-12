# GLogViewer

GLogViewer is a cross-platform, dependency-free C99 tool for exploring large structured log files from a command line or terminal interface. The executable is `logview`. The batch CLI supports raw and JSON Lines input, syslog RFC 3164/5424, Apache/Nginx access logs, custom patterns, format detection, compound and range filters, regex, reusable query profiles, record sampling, text/CSV export, exact record/level counts, timestamp timelines, estimated frequent field values, and follow mode with truncation and rotation detection. The TUI supports bounded result windows, viewport navigation, regex search, source-line jumps and bookmarks, error/warning jumps, configurable physical context, formatted JSON zoom, field-value summaries, time histograms, filter edits, export, and live follow. The latest stable release is [1.1.0](https://github.com/GIEI/GLogViewer/releases/tag/v1.1.0); the current development version is 1.3.0. See the [release notes](CHANGELOG.md), [user manual](docs/USER_MANUAL.md), and [known limits](docs/TECHNICAL_SPECIFICATION.md).

## Features

- Stream files larger than 1 GB with bounded application memory.
- Parse RFC 3164-like and RFC 5424 syslog, Apache/Nginx access logs, JSON Lines plus pretty-printed JSON object arrays with nested fields, and custom placeholder patterns.
- Combine equality, regex, and lexical range predicates with an internal regular expression engine.
- Inspect a full first or last matching record; limit batch or TUI results with `--top N` and `--bottom N`.
- Browse and search in a terminal UI, inspect pretty-printed JSON, summarize field values, explore timestamp histograms, follow appended records, and export text or CSV.
- Build on Windows, macOS, and Linux using C99 plus minimal platform console and file APIs.

## Build

On Windows, use the batch build script and Python test runner from PowerShell. The Unix `make` targets are not available in a standard Windows shell:

```powershell
.\build.bat
python tests\test_cli.py build\logview.exe
```

The build script locates the latest Visual Studio C++ Build Tools installation and rebuilds the executable, so there is no separate `make clean` step. On macOS or Linux, use a C99 compiler and Make:

```sh
make
make test
```

On macOS/Linux, `make clean && make test` is also supported when you want to remove the previous executable before testing.

The CLI test suite has been reported passing on Windows, Linux, and macOS. Tests use temporary fixtures under `build/test-tmp`, run targeted checks for the original large samples when present, and perform a full raw streaming scan of every other regular file under `SAMPLES`, including nested folders. See [validation notes](docs/VALIDATION.md) for the recorded platform results and limitations.

GitHub Actions builds the project and runs the CLI suite on Windows, Linux, and macOS for pushes and pull requests. Large sample datasets are user-provided and are not included in the repository; CI runs the generated fixtures and scans sample files only when present.

## CLI examples

```text
build\logview.exe SAMPLES\enwik8 --regex "Wikipedia"
build\logview.exe app.jsonl --format jsonl --filter "level=ERROR" --regex-field "message=timeout"
```

Available formats are `raw` (the default), `jsonl`, `syslog3164`, `syslog5424`, `apache-common`, `apache-combined`, `nginx-combined`, `custom`, and `auto`. Filters support `=`, `!=`, `~`, `<`, `<=`, `>`, `>=`, `and`, `or`, and parentheses. Range comparisons are lexical; fixed-format dates with matching timezone conventions sort chronologically.

For formatted JSON datasets, `jsonl` also accepts a top-level array of multiline objects and exposes nested primitive fields through dotted paths. A leaf field name such as `phone` can be used as a short alias when unambiguous in the record:

```text
build\logview.exe SAMPLES\employees_5-level_1-GB_formatted.json --format jsonl --regex-field "phone=^829"
```

Use the full path when you want to identify the exact nested field:

```text
build\logview.exe SAMPLES\employees_5-level_1-GB_formatted.json --format jsonl --regex-field "employee.department.manager.contact.phone=^829"
```

## More examples

Text/CSV export, statistics, and follow are available. `--stats-field FIELD` focuses frequent-value output on one parsed field. Save and reload repeatable query settings with `--save-profile incident.glogprofile` and `--profile incident.glogprofile`; explicit CLI options override the loaded profile. In the TUI, press `b` to bookmark a source line and `B` to visit the next bookmark. Press `c` for context; `--context-before N` and `--context-after N` set its initial physical-line range, while `+` and `-` adjust both values. Press `v` to summarize field values and `h` to build a time histogram; select a bucket and press Enter to jump to its first matching record. Press `z` to inspect a complete record; valid JSON is indented for readability while the source and exports remain unchanged. The TUI uses a bounded row cache and a temporary on-disk checkpoint index for navigation across the full scan. `--top N` and `--bottom N` limit batch or TUI results, while `--record first|last` prints a complete matching record for inspection. Field filters support lexical range operators for sortable values such as fixed-format timestamps. Timeline and TUI histogram use additional streaming passes; frequent field/value counts use bounded memory and report exact or estimated counts.

```text
logview app.log --format custom --pattern "%{TIMESTAMP:ts} %{LEVEL:lvl} %{MESSAGE:msg}" --filter "level=ERROR" --regex "timeout"
logview access.log --format apache-combined --filter "status=500 or status=502" --export csv --output errors.csv
logview app.jsonl --format jsonl --stats
logview app.jsonl --format jsonl --stats-field service
logview app.jsonl --format jsonl --filter "level=ERROR" --regex "timeout" --save-profile incident.glogprofile
logview app.jsonl --profile incident.glogprofile --tui --context-before 8 --context-after 12
logview syslog.log --format syslog3164 --syslog-year 2025 --syslog-timezone +02:00 --stats
logview app.jsonl --format jsonl --filter "timestamp>=2026-10-01T00:00:00Z and timestamp<2026-11-01T00:00:00Z" --top 10
logview app.jsonl --format jsonl --record first
logview app.jsonl --format jsonl --bottom 10 --tui
logview --follow app.log --format jsonl --filter "level=ERROR"
logview --stats app.log --format jsonl
logview --tui app.log
```

## Documents

- [Product roadmap](docs/ROADMAP.md): release history and planned post-1.2 milestones.
- [Implementation plan](docs/IMPLEMENTATION_PLAN.md): completed 1.2 and 1.3 scope, validation status, and acceptance criteria for upcoming releases.
- [Feature catalogue](docs/FEATURES.md): scope, user workflows, competing tools, and priorities.
- [Technical specification](docs/TECHNICAL_SPECIFICATION.md): syntax, data contracts, limits, performance targets, and acceptance tests.
- [Software architecture](docs/SOFTWARE_ARCHITECTURE.md): modules, data flow, platform boundaries, and implementation sequence.
- [User manual](docs/USER_MANUAL.md): commands, format selection, filters, navigation, exports, and troubleshooting.
- [Validation notes](docs/VALIDATION.md): local platform checks and the large syslog/HTTP sample results.

The documents distinguish implemented behavior from the 1.0 design target. The source list in the technical specification records the standards, product documentation, and research used to shape the design.

## Project conventions

All repository content and every source comment must be written in English. See [AGENTS.md](AGENTS.md) and the project agent at [.cursor/agents/english-project-guardian.md](.cursor/agents/english-project-guardian.md).

## Status and license

Status: development version `1.3.0`; latest stable release `1.1.0`. The 1.3 changes pass the local Windows build and test suite; Linux/macOS and sanitizer checks must pass before publication. The existing [LICENSE](LICENSE) governs the repository.

See the [release process](docs/RELEASE_PROCESS.md) for the tag-based packaging workflow.
