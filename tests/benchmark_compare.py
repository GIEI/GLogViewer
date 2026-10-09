"""Compare two builds on identical local samples with interleaved warm runs."""

import argparse
import hashlib
import json
import platform
import statistics
import subprocess
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--samples", type=Path, default=Path("SAMPLES"))
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    binaries = [args.baseline.resolve(), args.candidate.resolve()]
    report = {
        "platform": platform.platform(),
        "binaries": [{"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                     for path in binaries],
        "protocol": "One warm-up per build, alternating run order; stdout discarded; no cache eviction.",
        "cases": [],
    }
    cases = [
        ("enwik8", ["--format", "raw"], 0),
        ("Linux_1GB.syslog", ["--format", "syslog3164", "--filter", "pid=__glogviewer_no_such_pid__"], 1),
        ("http_access_500MB.log", ["--format", "apache-combined", "--filter", "status=__glogviewer_no_such_status__"], 1),
    ]
    for filename, options, expected_code in cases:
        sample = (args.samples / filename).resolve()
        if not sample.is_file():
            print("Skipped missing sample: {}".format(sample), flush=True)
            continue
        timings = [[], []]
        diagnostics = None
        for iteration in range(args.runs + 1):
            order = range(2) if iteration % 2 == 0 else (1, 0)
            for index in order:
                started = time.perf_counter()
                result = subprocess.run([str(binaries[index]), str(sample), *options],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                        timeout=300, check=False)
                elapsed = time.perf_counter() - started
                if result.returncode != expected_code:
                    raise RuntimeError("{} exited {}: {!r}".format(binaries[index], result.returncode, result.stderr))
                if diagnostics is None:
                    diagnostics = result.stderr
                elif result.stderr != diagnostics:
                    raise RuntimeError("Builds produced different diagnostics for {}".format(filename))
                if iteration:
                    timings[index].append(elapsed)
            print("{}: {}/{} measured pairs".format(filename, iteration, args.runs), flush=True)
        medians = [statistics.median(values) for values in timings]
        case = {"sample": str(sample), "bytes": sample.stat().st_size,
                "options": options, "seconds": timings, "median_seconds": medians,
                "speedup": medians[0] / medians[1]}
        report["cases"].append(case)
        print("{}: {:.3f}s -> {:.3f}s ({:.2f}x)".format(filename, *medians, case["speedup"]), flush=True)
    if not report["cases"]:
        raise RuntimeError("No sample files were available")
    if args.output:
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")


if __name__ == "__main__":
    main()
