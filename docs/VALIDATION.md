# Validation Notes

**Status:** local validation evidence, 2026-10-08. These measurements are observations from one development machine and are not release benchmarks.

## Windows CLI validation

The following commands completed successfully in the repository workspace:

```powershell
.\build.bat
python tests\test_cli.py build\logview.exe
```

The CLI suite covers help/version output, raw and JSON Lines input, regex constructs and work limits, compound filter precedence, syslog RFC 3164/5424, Apache Common/Combined, custom patterns, format detection, text/CSV export, exact record/level statistics, high-cardinality frequent-value estimates, ISO and Apache timeline parsing, follow append/partial/truncate/from-start behavior, output failures, TUI non-terminal rejection, long lines, and optional local sample files. POSIX runs additionally test long input paths, rotation, Ctrl-C partial flushing, and PTY search/navigation, invalid filter edits, bounded long-line previews, terminal restoration, resize, and live follow.

## Linux_1GB.syslog sample

The local sample is 1,073,765,600 bytes (about 1.074 decimal GB) and contains 9,915,041 physical records. A streaming inspection found a maximum record length of 206 bytes including its line terminator. It uses BSD-style syslog timestamps without year or timezone, and includes records with PIDs, tags without PIDs, and the legacy tag `syslogd 1.4.1` containing spaces.

The RFC 3164 parser was adjusted to accept those process tag forms. A full `--format syslog3164` scan with a deliberately unmatched PID completed with no rejected-record diagnostic. Auto-detection followed by an unmatched process filter also completed with no rejected-record diagnostic.

Five sequential scans with MSVC `/O2`, RFC 3164 parsing, an unmatched PID filter, and stdout/stderr redirected to null took 8.564, 2.242, 2.247, 2.253, and 2.191 seconds. The median of those five runs was 2.247 seconds. The variation indicates a cold or cache-constrained first pass; the machine CPU, storage model, and peak RSS were not captured, so these numbers are not comparable to the formal 1.0 benchmark protocol. The scan is filtered and parsed, so it does not establish the separate unfiltered 1 GiB throughput gate.

## http_access_500MB.log sample

The local HTTP sample is 526,315,158 bytes and contains 2,220,000 records. It uses Apache Combined layout; the maximum physical line is 1,364 bytes. A full `--format apache-combined` pass found 222 truncated records with unterminated user-agent quotes; these are rejected and counted, while the other records parse successfully. The latest regression scan completed in 3.64 seconds.

The access fixture includes response byte counts represented by `-`, which the parser accepts as a missing value. The truncated user-agent records remain malformed rather than being silently repaired.

Running `--stats` on this sample reported 2,220,000 physical records, 2,219,778 parsed records, 222 rejected records, and 2,220,000 selected records. Since HTTP access records have no severity field, all selected records were counted as `UNSPECIFIED`. The test suite validates timestamp edge bins, offset normalization, unknown timestamps, Apache access timestamps, and frequent-value estimates.

## Platform coverage

The CLI suite and large-sample scans have passed on the user's Linux machine with GCC and no compiler warnings. The expanded POSIX suite also passed, covering follow rotation/Ctrl-C behavior and the TUI PTY scenarios. The Windows build and CLI suite pass with MSVC. macOS validation is deferred to the 1.0 MVP. The sample's `Linux_` filename describes its log content, not the operating system used to run the Windows checks.
