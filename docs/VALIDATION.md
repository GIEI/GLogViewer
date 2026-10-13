# Validation Notes

**Status:** release validation evidence, 2026-10-13. The project owner reports the 1.5.0 test suite passing on Windows, Linux, and macOS. The 1.6.0 tag-triggered workflow is the release gate for the Boolean search changes and runs clean platform builds, package checks, Linux sanitizers, and artifact attestations. Exploratory sample comparisons are observations, not release benchmarks unless explicitly identified below as the formal 1 GiB benchmark.

## Version 1.6.0 release validation

The release workflow runs the complete test suite on Windows, Linux, and macOS, including the POSIX TUI Boolean-search checks on Linux and macOS. It also runs AddressSanitizer and UndefinedBehaviorSanitizer, validates release archives, checks the binary version against the tag, generates SHA-256 checksums and artifact attestations, and publishes only after required jobs succeed.

## Version 1.5.0 release validation

The project owner confirmed that the complete test suite passes on Windows, Linux, and macOS. The local Windows build and CLI/sample suite also pass in this workspace. The `v1.5.0` release workflow repeats the platform build and test matrix, runs Linux AddressSanitizer and UndefinedBehaviorSanitizer checks, validates all release archives, generates SHA-256 checksums and artifact attestations, and publishes only after the required jobs succeed.

## Reader optimization comparison

The line reader now searches each available input-buffer span for LF with `memchr` and appends spans with `memcpy`, instead of handling every byte through the old byte-reader loop. It keeps the reusable 64 KiB input buffer, exact maximum-line behavior, CRLF normalization, source offsets, and oversized-line draining. `tests/test_cli.py` includes refill-boundary and line-limit cases. The full Windows CLI suite passes with this reader.

`tests/benchmark_compare.py` compares a baseline and candidate binary on the available samples. It performs one warm-up per binary, alternates run order for five measured runs, discards stdout, checks exit status and diagnostics, and reports medians and binary hashes. The Windows run with MSVC `/O2` produced these preliminary results:

To repeat the comparison, keep a copy of the old executable, build the candidate as `build/logview.exe`, then run `python tests/benchmark_compare.py build/logview-before.exe build/logview.exe --runs 5 --output build/reader-benchmark.json`.

| Sample and operation | Old reader | Block reader | Speedup |
| --- | ---: | ---: | ---: |
| `enwik8`, raw output discarded | 0.907 s | 0.820 s | 1.11x |
| `Linux_1GB.syslog`, RFC 3164 parse and unmatched PID filter | 1.900 s | 0.893 s | 2.13x |
| `http_access_500MB.log`, Apache Combined parse and unmatched status filter | 1.097 s | 0.592 s | 1.85x |

These are cached-input sample comparisons, not the formal unfiltered 1 GiB release benchmark. AVX2-only compilation (`/O2 /arch:AVX2`) showed no stable improvement on the HTTP sample: the median was 1.146 s compared with 1.097 s for `/O2`. The measured process CPU time was close to wall time, consistent with the current single-thread scan keeping one CPU core busy. The GPU is not used by the application.

The block scanner follows a low-risk first optimization: optimized C library search and copy routines operate over spans while line parsing and filtering stay unchanged. The [simdjson paper](https://arxiv.org/abs/1902.08318) shows that SIMD structural scanning can improve parsing throughput even on one core; adopting a full SIMD JSON parser would add complexity and a dependency, so it is not the first step here. [Hyperscan's NSDI 2019 paper](https://www.usenix.org/conference/nsdi19/presentation/wang-xiang) uses SIMD automata and pattern decomposition for large regex sets, a different workload from GLogViewer's single custom regex and built-in engine. The [simdjson `parse_many` design](https://github.com/simdjson/simdjson/blob/master/doc/parse_many.md) is a useful batching precedent: it reuses parser state and overlaps independent stages.

Multithreaded batch filtering remains a follow-up. A safe design needs newline-aligned batches, a bounded work queue, worker-owned parser scratch, and ordered output. The custom parser currently has mutable static state, and the frequent-value statistics sketch depends on record order; these must be addressed or excluded before enabling concurrent parsing. The [GNU Parallel `--pipe-part` design](https://www.gnu.org/software/parallel/parallel_design.html) is a useful reference for file blocks, job limits, and ordered results. The existing benchmark script can measure later changes against these workloads.

## Formal release benchmark

`tests/benchmark_release.py` creates a generated 1 GiB raw-text fixture, performs one full warmup scan and five measured scans, discards standard output, and writes a JSON report with the binary version, each duration, the median, throughput, operating system, processor, logical CPU count, build details, storage description, and peak RSS. It exits nonzero if the median is not below five seconds or measured peak RSS exceeds 64 MiB. RSS is a conservative proxy for application heap because it includes the executable, runtime, and stacks; report it as RSS rather than as a direct heap measurement.

Build the release candidate from a clean checkout first, then record the actual compiler command and storage device. For example, on Windows:

```powershell
python tests\benchmark_release.py build\logview.exe `
  --build-details "MSVC /O2 /std:c11" `
  --hardware-details "CPU and memory details, if approved for publication" `
  --storage-details "NVMe SSD, NTFS" `
  --fixture-directory C:\benchmark-fixture `
  --report build\release-benchmark-windows.json
```

`--fixture-directory` places the generated input on the volume being measured, even when the checkout and executable are on another drive. On macOS or Linux, pass `build/logview` and describe the compiler flags and input volume in the same way. The default run is the release gate. Smaller `--size-bytes` values are useful only for checking the harness and do not count as benchmark evidence. Save the report with the release validation records and include the full generated input size in any comparison.

### Windows 1.0 reference result

The clean `186cf1a` 1.0.0 Windows build was measured with optimized MSVC. It scanned a generated 1 GiB raw-text fixture on the NVMe reference volume with stdout redirected to the null device, using one warmup followed by five measured scans. The median was **0.765 seconds** (range 0.755–0.772 seconds), or **1.31 GiB/s**. Peak RSS was **4.14 MiB**, below the 64 MiB target. The fixture contains 21,053,761 complete records of 50 bytes plus LF (51 bytes each) and a 13-byte unterminated final record. This is a single-machine observation, not a cross-platform guarantee. The [redacted benchmark report](benchmarks/windows-reference-2026-10-11.json) contains the run data without workstation identifiers or drive paths. Its RSS value conservatively includes executable, runtime, and stack memory; it is not a direct heap measurement.

## Windows CLI validation

The project uses `build.bat` on Windows; the Makefile targets (`make clean` and `make test`) are for macOS/Linux shells.

The following commands completed successfully in the repository workspace:

```powershell
.\build.bat
python tests\test_cli.py build\logview.exe
```

The CLI suite covers help/version output, raw and JSON Lines input, pretty-printed JSON object arrays, nested JSON field paths and leaf aliases, regex constructs and work limits, compound filter precedence, syslog RFC 3164/5424, Kubernetes CRI parsing/auto-detection/tag filters/fractional timestamps/malformed records, Apache Common/Combined, custom patterns, format detection, text/CSV export, exact record/level statistics, high-cardinality frequent-value estimates, ISO and Apache timeline parsing, versioned query profile save/load/override/error/overwrite behavior, multi-file timestamp merge/ties/source labels/unknown timestamp ordering/aggregate stats/duplicate and unsorted input errors, follow append/partial/truncate/from-start behavior, output failures, TUI non-terminal rejection, long lines, and sample files. It runs targeted checks for `enwik8`, `Linux_1GB.syslog`, and `http_access_500MB.log` when present, then performs a full raw streaming scan of every other regular file in `SAMPLES`, including nested folders. POSIX runs additionally test long input paths, rotation, Ctrl-C partial flushing, and PTY search/navigation, invalid filter edits, bookmarks and context controls, histogram navigation, bounded long-line previews, full-record zoom scrolling, terminal restoration, resize, live follow, and TUI export. The formatted employee sample was also scanned using the `phone=^829` field regex; 337 records matched.

## Linux_1GB.syslog sample

The local sample is 1,073,765,600 bytes (about 1.074 decimal GB) and contains 9,915,041 physical records. A streaming inspection found a maximum record length of 206 bytes including its line terminator. It uses BSD-style syslog timestamps without year or timezone, and includes records with PIDs, tags without PIDs, and the legacy tag `syslogd 1.4.1` containing spaces.

The RFC 3164 parser was adjusted to accept those process tag forms. A full `--format syslog3164` scan with a deliberately unmatched PID completed with no rejected-record diagnostic. Auto-detection followed by an unmatched process filter also completed with no rejected-record diagnostic.

Five sequential scans with MSVC `/O2`, RFC 3164 parsing, an unmatched PID filter, and stdout/stderr redirected to null took 8.564, 2.242, 2.247, 2.253, and 2.191 seconds. The median of those five runs was 2.247 seconds. The variation indicates a cold or cache-constrained first pass; the machine CPU, storage model, and peak RSS were not captured, so these numbers are not comparable to the formal 1.0 benchmark protocol. The scan is filtered and parsed, so it does not establish the separate unfiltered 1 GiB throughput gate.

## http_access_500MB.log sample

The local HTTP sample is 526,315,158 bytes and contains 2,220,000 records. It uses Apache Combined layout; the maximum physical line is 1,364 bytes. A full `--format apache-combined` pass found 222 truncated records with unterminated user-agent quotes; these are rejected and counted, while the other records parse successfully. The latest regression scan completed in 3.64 seconds.

The access fixture includes response byte counts represented by `-`, which the parser accepts as a missing value. The truncated user-agent records remain malformed rather than being silently repaired.

Running `--stats` on this sample reported 2,220,000 physical records, 2,219,778 parsed records, 222 rejected records, and 2,220,000 selected records. Since HTTP access records have no severity field, all selected records were counted as `UNSPECIFIED`. The test suite validates timestamp edge bins, offset normalization, unknown timestamps, Apache access timestamps, and frequent-value estimates.

## Platform coverage

The CLI suite and large-sample scans have passed on the user's Linux machine with GCC and no compiler warnings. The expanded POSIX suite also passed, covering follow rotation, temporary path absence and reopen, Ctrl-C behavior, and TUI PTY scenarios. The Windows build and CLI suite pass with MSVC. The user also reported `make clean && make test` passing on macOS; all eight available sample files were scanned. GitHub Actions run [38154893862](https://github.com/GIEI/GLogViewer/actions/runs/38154893862) passed parser/follow hardening tests, all platform builds, packaging, and Linux sanitizers. Run [38155045245](https://github.com/GIEI/GLogViewer/actions/runs/38155045245) passed all platform suites, package checks, benchmark smoke tests, and sanitizers with the binary reporting `logview 1.0.0`. Stable release run [38155296731](https://github.com/GIEI/GLogViewer/actions/runs/38155296731) passed all tagged jobs and published the Windows, Linux, and macOS archives. The downloaded archives match `SHA256SUMS.txt`; the Windows binary reports `logview 1.0.0`, and Linux/macOS executables have executable permission. GitHub lists one signed provenance attestation for each archive. Local `gh attestation verify` could not complete on the Windows workstation because the CLI reported that the Sigstore public-good verifier could not be initialized; this does not affect the successful attestation-generation step. The workflow uses fixed `windows-2025`, `ubuntu-24.04`, and `macos-15` runner images. The sample's `Linux_` filename describes its log content, not the operating system used to run the Windows checks.
