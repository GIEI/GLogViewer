# GLogViewer

GLogViewer is a cross-platform, dependency-free C99 tool for exploring large structured log files from a command line or terminal interface. The proposed executable is `logview`. The batch CLI supports raw and JSON Lines input, syslog RFC 3164/5424, Apache/Nginx access logs, custom patterns, format detection, compound filters, regex, text/CSV export, exact record/level counts, a timestamp timeline, estimated frequent field values, and follow mode with truncation and rotation detection. An initial TUI supports a bounded viewport, regex search, filter edits, and help; full-file navigation is still in development. macOS validation is deferred to the 1.0 MVP. See the [roadmap](docs/ROADMAP.md) for remaining work to 1.0.

## Version 1.0 target

- Stream files larger than 1 GB with bounded application memory.
- Parse RFC 3164-like and RFC 5424 syslog, Apache/Nginx access logs, a defined JSON Lines subset, and custom placeholder patterns.
- Combine field predicates and an internal regular expression engine.
- Browse and search in a terminal UI, follow appended records, export text or CSV, and calculate summary statistics.
- Build on Windows, macOS, and Linux using C99 plus minimal platform console and file APIs.

## Build

On Windows, run from PowerShell:

```powershell
.\build.bat
python tests\test_cli.py build\logview.exe
```

The build script locates the latest Visual Studio C++ Build Tools installation. On macOS or Linux, use a C99 compiler and Make:

```sh
make
make test
```

The Linux Makefile and test suite have passed on the user's Linux machine. macOS validation is deferred to the 1.0 MVP. Tests use temporary fixtures under `build/test-tmp` and scan local samples when present.

## CLI examples

```text
build\logview.exe SAMPLES\enwik8 --regex "Wikipedia"
build\logview.exe app.jsonl --format jsonl --filter "level=ERROR" --regex-field "message=timeout"
```

Available formats are `raw` (the default), `jsonl`, `syslog3164`, `syslog5424`, `apache-common`, `apache-combined`, `nginx-combined`, `custom`, and `auto`. Filters support `=`, `!=`, `~`, `and`, `or`, and parentheses. Use `--help` for the exact options.

## More examples

Text/CSV export, statistics, and follow are available. The TUI is in progress and currently retains a bounded window of recent selected records. Timestamp timeline output uses a second streaming pass; frequent field/value counts are bounded estimates.

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
- [User manual](docs/USER_MANUAL.md): proposed commands, format selection, filters, navigation, exports, and troubleshooting.
- [Validation notes](docs/VALIDATION.md): local platform checks and the large syslog/HTTP sample results.

The documents distinguish implemented behavior from the 1.0 design target. The source list in the technical specification records the standards, product documentation, and research used to shape the design.

## Project conventions

All repository content and every source comment must be written in English. See [AGENTS.md](AGENTS.md) and the project agent at [.cursor/agents/english-project-guardian.md](.cursor/agents/english-project-guardian.md).

## Status and license

Status: 0.3 reporting milestone implemented; cross-platform and release validation remain before 1.0. The existing [LICENSE](LICENSE) governs the repository.
