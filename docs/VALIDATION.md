# Validation Notes

**Status:** local validation evidence, 2026-10-09. These measurements are observations from one development machine and are not release benchmarks.

## Reader optimization comparison

The line reader now searches each available input-buffer span for LF with `memchr` and appends spans with `memcpy`, instead of handling every byte through the old byte-reader loop. It keeps the reusable 64 KiB input buffer, exact maximum-line behavior, CRLF normalization, source offsets, and oversized-line draining. `tests/test_cli.py` includes refill-boundary and line-limit cases. The full Windows CLI suite passes with this reader.

`tests/benchmark_compare.py` compares a baseline and candidate binary on the available samples. It performs one warm-up per binary, alternates run order for five measured runs, discards stdout, checks exit status and diagnostics, and reports medians and binary hashes. The run on Windows 11 with MSVC `/O2`, on the workstation reported as Ryzen 5800/Radeon RX 6900 XT, produced these preliminary results:

To repeat the comparison, keep a copy of the old executable, build the candidate as `build/logview.exe`, then run `python tests/benchmark_compare.py build/logview-before.exe build/logview.exe --runs 5 --output build/reader-benchmark.json`.

| Sample and operation | Old reader | Block reader | Speedup |
| --- | ---: | ---: | ---: |
| `enwik8`, raw output discarded | 0.907 s | 0.820 s | 1.11x |
| `Linux_1GB.syslog`, RFC 3164 parse and unmatched PID filter | 1.900 s | 0.893 s | 2.13x |
| `http_access_500MB.log`, Apache Combined parse and unmatched status filter | 1.097 s | 0.592 s | 1.85x |

These are cached-input sample comparisons, not the formal unfiltered 1 GiB release benchmark. AVX2-only compilation (`/O2 /arch:AVX2`) showed no stable improvement on the HTTP sample: the median was 1.146 s compared with 1.097 s for `/O2`. The measured process CPU time was close to wall time, consistent with the current single-thread scan keeping one CPU core busy. The GPU is not used by the application.

The block scanner follows a low-risk first optimization: optimized C library search and copy routines operate over spans while line parsing and filtering stay unchanged. The [simdjson paper](https://arxiv.org/abs/1902.08318) shows that SIMD structural scanning can improve parsing throughput even on one core; adopting a full SIMD JSON parser would add complexity and a dependency, so it is not the first step here. [Hyperscan's NSDI 2019 paper](https://www.usenix.org/conference/nsdi19/presentation/wang-xiang) uses SIMD automata and pattern decomposition for large regex sets, a different workload from GLogViewer's single custom regex and built-in engine. The [simdjson `parse_many` design](https://github.com/simdjson/simdjson/blob/master/doc/parse_many.md) is a useful batching precedent: it reuses parser state and overlaps independent stages.

Multithreaded batch filtering remains a follow-up. A safe design needs newline-aligned batches, a bounded work queue, worker-owned parser scratch, and ordered output. The custom parser currently has mutable static state, and the frequent-value statistics sketch depends on record order; these must be addressed or excluded before enabling concurrent parsing. The [GNU Parallel `--pipe-part` design](https://www.gnu.org/software/parallel/parallel_design.html) is a useful reference for file blocks, job limits, and ordered results. The existing benchmark script can measure later changes against these workloads.

## Windows CLI validation

The project uses `build.bat` on Windows; the Makefile targets (`make clean` and `make test`) are for macOS/Linux shells.

The following commands completed successfully in the repository workspace:

```powershell
.\build.bat
python tests\test_cli.py build\logview.exe
```

The CLI suite covers help/version output, raw and JSON Lines input, pretty-printed JSON object arrays, nested JSON field paths and leaf aliases, regex constructs and work limits, compound filter precedence, syslog RFC 3164/5424, Apache Common/Combined, custom patterns, format detection, text/CSV export, exact record/level statistics, high-cardinality frequent-value estimates, ISO and Apache timeline parsing, follow append/partial/truncate/from-start behavior, output failures, TUI non-terminal rejection, long lines, and sample files. It runs targeted checks for `enwik8`, `Linux_1GB.syslog`, and `http_access_500MB.log` when present, then performs a full raw streaming scan of every other regular file in `SAMPLES`, including nested folders. POSIX runs additionally test long input paths, rotation, Ctrl-C partial flushing, and PTY search/navigation, invalid filter edits, bounded long-line previews, full-record zoom scrolling, terminal restoration, resize, live follow, and TUI export. The formatted employee sample was also scanned using the `phone=^829` field regex; 337 records matched.

## Linux_1GB.syslog sample

The local sample is 1,073,765,600 bytes (about 1.074 decimal GB) and contains 9,915,041 physical records. A streaming inspection found a maximum record length of 206 bytes including its line terminator. It uses BSD-style syslog timestamps without year or timezone, and includes records with PIDs, tags without PIDs, and the legacy tag `syslogd 1.4.1` containing spaces.

The RFC 3164 parser was adjusted to accept those process tag forms. A full `--format syslog3164` scan with a deliberately unmatched PID completed with no rejected-record diagnostic. Auto-detection followed by an unmatched process filter also completed with no rejected-record diagnostic.

Five sequential scans with MSVC `/O2`, RFC 3164 parsing, an unmatched PID filter, and stdout/stderr redirected to null took 8.564, 2.242, 2.247, 2.253, and 2.191 seconds. The median of those five runs was 2.247 seconds. The variation indicates a cold or cache-constrained first pass; the machine CPU, storage model, and peak RSS were not captured, so these numbers are not comparable to the formal 1.0 benchmark protocol. The scan is filtered and parsed, so it does not establish the separate unfiltered 1 GiB throughput gate.

## http_access_500MB.log sample

The local HTTP sample is 526,315,158 bytes and contains 2,220,000 records. It uses Apache Combined layout; the maximum physical line is 1,364 bytes. A full `--format apache-combined` pass found 222 truncated records with unterminated user-agent quotes; these are rejected and counted, while the other records parse successfully. The latest regression scan completed in 3.64 seconds.

The access fixture includes response byte counts represented by `-`, which the parser accepts as a missing value. The truncated user-agent records remain malformed rather than being silently repaired.

Running `--stats` on this sample reported 2,220,000 physical records, 2,219,778 parsed records, 222 rejected records, and 2,220,000 selected records. Since HTTP access records have no severity field, all selected records were counted as `UNSPECIFIED`. The test suite validates timestamp edge bins, offset normalization, unknown timestamps, Apache access timestamps, and frequent-value estimates.

## Platform coverage

The CLI suite and large-sample scans have passed on the user's Linux machine with GCC and no compiler warnings. The expanded POSIX suite also passed, covering follow rotation/Ctrl-C behavior and the TUI PTY scenarios. The Windows build and CLI suite pass with MSVC. The user also reported `make clean && make test` passing on macOS with an Apple M5 and 16 GB RAM; all eight available sample files were scanned. A GitHub Actions workflow now repeats clean builds and the CLI suite on Windows, Linux, and macOS for pushes and pull requests; the first hosted run is pending. The sample's `Linux_` filename describes its log content, not the operating system used to run the Windows checks.
