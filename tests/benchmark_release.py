"""Run the repeatable warm-cache throughput benchmark used by the release gate."""

import argparse
import ctypes
import datetime
import hashlib
import json
import os
import platform
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


DEFAULT_SIZE_BYTES = 1024 * 1024 * 1024
RECORD = b"INFO 2026-01-01T00:00:00Z release benchmark record\n"
WRITE_BLOCK_BYTES = 4 * 1024 * 1024


def parse_args():
    parser = argparse.ArgumentParser(
        description="Benchmark an unfiltered raw scan using a generated fixture."
    )
    parser.add_argument("binary", type=Path, help="Path to the logview executable")
    parser.add_argument(
        "--build-details",
        required=True,
        help="Compiler and build flags used to create the executable",
    )
    parser.add_argument(
        "--storage-details",
        required=True,
        help="Input storage device and filesystem used for the benchmark",
    )
    parser.add_argument(
        "--hardware-details",
        required=True,
        help="CPU model and installed memory of the benchmark machine",
    )
    parser.add_argument(
        "--runs", type=int, default=5,
        help="Number of measured warm-cache runs (release gate: 5)",
    )
    parser.add_argument(
        "--size-bytes", type=int, default=DEFAULT_SIZE_BYTES,
        help="Generated fixture size in bytes (release gate: 1073741824)",
    )
    parser.add_argument(
        "--report", type=Path,
        help="Optional path for a JSON report",
    )
    parser.add_argument(
        "--fixture-directory",
        type=Path,
        default=Path("build") / "release-benchmark",
        help="Directory on the volume used for the generated input fixture",
    )
    parser.add_argument(
        "--keep-fixture", action="store_true",
        help="Keep the generated fixture under build/release-benchmark",
    )
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be at least 1")
    if args.size_bytes < len(RECORD):
        parser.error("--size-bytes must fit at least one benchmark record")
    if not args.build_details.strip():
        parser.error("--build-details must not be empty")
    if not args.storage_details.strip():
        parser.error("--storage-details must not be empty")
    if not args.hardware_details.strip():
        parser.error("--hardware-details must not be empty")
    return args


def write_fixture(path, size_bytes):
    block = RECORD * (WRITE_BLOCK_BYTES // len(RECORD))
    written = 0
    with path.open("wb") as output:
        while written < size_bytes:
            chunk_size = min(len(block), size_bytes - written)
            output.write(block[:chunk_size])
            written += chunk_size
    return written


def get_windows_peak_working_set(process):
    class ProcessMemoryCounters(ctypes.Structure):
        _fields_ = [
            ("cb", ctypes.c_ulong),
            ("PageFaultCount", ctypes.c_ulong),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
        ]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel32.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    psapi.GetProcessMemoryInfo.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ProcessMemoryCounters),
        ctypes.c_ulong,
    ]
    psapi.GetProcessMemoryInfo.restype = ctypes.c_int

    process_handle = kernel32.OpenProcess(0x0400 | 0x0010, 0, process.pid)
    if not process_handle:
        return None
    try:
        counters = ProcessMemoryCounters()
        counters.cb = ctypes.sizeof(counters)
        if not psapi.GetProcessMemoryInfo(
            process_handle, ctypes.byref(counters), counters.cb
        ):
            return None
        return int(counters.PeakWorkingSetSize)
    finally:
        kernel32.CloseHandle(process_handle)


def get_peak_rss_bytes(process):
    if os.name == "nt":
        return get_windows_peak_working_set(process)

    import resource

    peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    if sys.platform == "darwin":
        return int(peak)
    return int(peak * 1024)


def scan_once(binary, fixture):
    with tempfile.TemporaryFile() as errors:
        started = time.perf_counter()
        process = subprocess.Popen(
            [str(binary), str(fixture), "--format", "raw"],
            stdout=subprocess.DEVNULL,
            stderr=errors,
        )
        return_code = process.wait()
        elapsed = time.perf_counter() - started
        peak_rss_bytes = get_peak_rss_bytes(process)
        errors.seek(0)
        diagnostic = errors.read().decode("utf-8", errors="replace")
    if return_code != 0:
        raise RuntimeError(
            "logview exited with status {}: {}".format(
                return_code, diagnostic.strip()
            )
        )
    if diagnostic:
        raise RuntimeError("logview wrote diagnostics: " + diagnostic.strip())
    return {"elapsed_seconds": elapsed, "peak_rss_bytes": peak_rss_bytes}


def get_binary_version(binary):
    result = subprocess.run(
        [str(binary), "--version"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    version = result.stdout.decode("utf-8", errors="replace").strip()
    if result.returncode != 0 or not version.startswith("logview "):
        raise RuntimeError("Could not read logview version: " + version)
    return version


def get_source_state():
    root = Path(__file__).resolve().parents[1]
    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=root, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, check=False,
    )
    if revision.returncode != 0:
        return None, None
    status = subprocess.run(
        ["git", "status", "--porcelain"], cwd=root, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, check=False,
    )
    clean = status.returncode == 0 and not status.stdout
    return revision.stdout.decode("ascii", errors="replace").strip(), clean


def get_binary_sha256(binary):
    digest = hashlib.sha256()
    with binary.open("rb") as executable:
        for block in iter(lambda: executable.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def build_report(args, binary, version, fixture, fixture_size, warmup, runs):
    median_seconds = statistics.median(
        item["elapsed_seconds"] for item in runs
    )
    peak_values = [
        item["peak_rss_bytes"] for item in [warmup, *runs]
        if item["peak_rss_bytes"] is not None
    ]
    peak_rss_bytes = max(peak_values) if peak_values else None
    gib = fixture_size / (1024 * 1024 * 1024)
    return {
        "benchmark": "glogviewer-plain-raw-warm-cache-v1",
        "timestamp_utc": datetime.datetime.now(
            datetime.timezone.utc
        ).isoformat(),
        "system": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor() or platform.uname().processor,
        "logical_cpu_count": os.cpu_count(),
        "hardware_details": args.hardware_details,
        "storage_details": args.storage_details,
        "binary": str(args.binary.resolve()),
        "binary_sha256": get_binary_sha256(binary),
        "version": version,
        "build_details": args.build_details,
        "input_path": str(fixture.resolve()),
        "input_size_bytes": fixture_size,
        "fixture_record_size_bytes_including_lf": len(RECORD),
        "fixture_full_record_count": fixture_size // len(RECORD),
        "fixture_final_unterminated_bytes": fixture_size % len(RECORD),
        "warmup": warmup,
        "measured_runs": runs,
        "median_elapsed_seconds": median_seconds,
        "throughput_gib_per_second": gib / median_seconds,
        "peak_rss_bytes": peak_rss_bytes,
        "output_destination": "stdout redirected to the null device",
        "procedure": "One full warmup scan followed by sequential measured scans.",
        "targets": {
            "median_under_5_seconds": median_seconds < 5.0,
            "peak_rss_under_64_mib": (
                peak_rss_bytes is not None and peak_rss_bytes <= 64 * 1024 * 1024
            ),
            "rss_note": (
                "Peak resident set size is a conservative proxy for the 64 MiB "
                "application-heap target; it includes runtime and stack memory."
            ),
        },
    }


def main():
    args = parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        raise SystemExit("Executable not found: " + str(binary))
    version = get_binary_version(binary)

    fixture_root = args.fixture_directory
    fixture_root.mkdir(parents=True, exist_ok=True)
    if args.keep_fixture:
        fixture = fixture_root / "fixture-{}-bytes.log".format(args.size_bytes)
        fixture_size = write_fixture(fixture, args.size_bytes)
        report = run_benchmark(args, binary, version, fixture, fixture_size)
    else:
        with tempfile.TemporaryDirectory(
            prefix="glogviewer-benchmark-", dir=fixture_root
        ) as temporary:
            fixture = Path(temporary) / "fixture.log"
            fixture_size = write_fixture(fixture, args.size_bytes)
            report = run_benchmark(args, binary, version, fixture, fixture_size)

    rendered = json.dumps(report, indent=2, sort_keys=True)
    print(rendered)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(rendered + "\n", encoding="utf-8")
    if not report["targets"]["median_under_5_seconds"]:
        return 1
    if not report["targets"]["peak_rss_under_64_mib"]:
        return 1
    return 0


def run_benchmark(args, binary, version, fixture, fixture_size):
    print("Warmup scan", file=sys.stderr)
    warmup = scan_once(binary, fixture)
    runs = []
    for index in range(args.runs):
        print("Measured scan {}/{}".format(index + 1, args.runs), file=sys.stderr)
        runs.append(scan_once(binary, fixture))
    report = build_report(args, binary, version, fixture, fixture_size, warmup, runs)
    report["source_revision"], report["source_tree_clean"] = get_source_state()
    return report


if __name__ == "__main__":
    raise SystemExit(main())
