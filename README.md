# GLogViewer

GLogViewer is a cross-platform, dependency-free C99 tool for exploring large structured log files from a command line or terminal interface. The executable is `logview`. The batch CLI supports raw and JSON Lines input, syslog RFC 3164/5424, Apache/Nginx access logs, custom patterns, format detection, compound filters, regex, text/CSV export, exact record/level counts, a timestamp timeline, estimated frequent field values, and follow mode with truncation and rotation detection. The TUI supports a bounded viewport, checkpoint-backed navigation and regex search across the full scan, source-line jumps, error/warning jumps, physical context lines, full-record zoom, filter edits, export of filtered/search-matched records, and live follow. The first public release is `0.5.0-alpha.1`; see the [release notes](CHANGELOG.md) and [roadmap](docs/ROADMAP.md) for scope and remaining work to 1.0.

## Version 1.0 target

- Stream files larger than 1 GB with bounded application memory.
- Parse RFC 3164-like and RFC 5424 syslog, Apache/Nginx access logs, JSON Lines plus pretty-printed JSON object arrays with nested fields, and custom placeholder patterns.
- Combine field predicates and an internal regular expression engine.
- Browse and search in a terminal UI, follow appended records, export text or CSV, and calculate summary statistics.
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

## CLI examples

```text
build\logview.exe SAMPLES\enwik8 --regex "Wikipedia"
build\logview.exe app.jsonl --format jsonl --filter "level=ERROR" --regex-field "message=timeout"
```

Available formats are `raw` (the default), `jsonl`, `syslog3164`, `syslog5424`, `apache-common`, `apache-combined`, `nginx-combined`, `custom`, and `auto`. Filters support `=`, `!=`, `~`, `and`, `or`, and parentheses. Use `--help` for the exact options.

For formatted JSON datasets, `jsonl` also accepts a top-level array of multiline objects and exposes nested primitive fields through dotted paths. A leaf field name such as `phone` can be used as a short alias when unambiguous in the record:

```text
build\logview.exe SAMPLES\employees_5-level_1-GB_formatted.json --format jsonl --regex-field "phone=^829"
```

Use the full path when you want to identify the exact nested field:

```text
build\logview.exe SAMPLES\employees_5-level_1-GB_formatted.json --format jsonl --regex-field "employee.department.manager.contact.phone=^829"
```

## More examples

Text/CSV export, statistics, and follow are available. The TUI uses a bounded row cache and a temporary on-disk checkpoint index for navigation across the full scan. Timestamp timeline output uses a second streaming pass; frequent field/value counts are bounded estimates.

```text
logview app.log --format custom --pattern "%{TIMESTAMP:ts} %{LEVEL:lvl} %{MESSAGE:msg}" --filter "level=ERROR" --regex "timeout"
logview access.log --format apache-combined --filter "status=500 or status=502" --export csv --output errors.csv
logview app.jsonl --format jsonl --stats
logview --follow app.log --format jsonl --filter "level=ERROR"
logview --stats app.log --format jsonl
logview --tui app.log
```

## Documents

- [Roadmap to 1.0](docs/ROADMAP.md): MVP scope, release sequence, and exit gates.
- [Feature catalogue](docs/FEATURES.md): scope, user workflows, competing tools, and priorities.
- [Technical specification](docs/TECHNICAL_SPECIFICATION.md): syntax, data contracts, limits, performance targets, and acceptance tests.
- [Software architecture](docs/SOFTWARE_ARCHITECTURE.md): modules, data flow, platform boundaries, and implementation sequence.
- [User manual](docs/USER_MANUAL.md): commands, format selection, filters, navigation, exports, and troubleshooting.
- [Validation notes](docs/VALIDATION.md): local platform checks and the large syslog/HTTP sample results.

The documents distinguish implemented behavior from the 1.0 design target. The source list in the technical specification records the standards, product documentation, and research used to shape the design.

## Project conventions

All repository content and every source comment must be written in English. See [AGENTS.md](AGENTS.md) and the project agent at [.cursor/agents/english-project-guardian.md](.cursor/agents/english-project-guardian.md).

## Status and license

Status: `0.5.0-alpha.1`; the formal release benchmark and broader platform validation remain before 1.0. The existing [LICENSE](LICENSE) governs the repository.
