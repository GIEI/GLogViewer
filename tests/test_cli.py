import subprocess
import csv
import io
import json
import os
import re
import signal
import sys
import tempfile
import time
from pathlib import Path


def run(binary, *arguments):
    return subprocess.run(
        [str(binary), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def check_reader_boundaries(binary, root):
    boundary_file = root / "reader-boundary.log"
    for length in (65534, 65535, 65536, 65537, 131073):
        for terminator in (b"\n", b"\r\n"):
            payload = b"x" * length
            boundary_file.write_bytes(payload + terminator + b"\nend\n")
            result = run(binary, str(boundary_file))
            require(result.returncode == 0 and result.stderr == b"" and
                    result.stdout == payload + b"\n\nend\n",
                    "reader should preserve records across refill boundaries "
                    "(length={}, terminator={!r})".format(length, terminator))

    # The byte limit includes CR before CRLF normalization, but excludes LF.
    limit = 65536
    records = [b"a" * limit + b"\n", b"b" * (limit + 1) + b"\n",
               b"\n", b"c" * (limit - 1) + b"\r\n",
               b"d" * limit + b"\r\n", b"valid\n", b"final\r"]
    boundary_file.write_bytes(b"".join(records))
    expected = (b"a" * limit + b"\n\n" + b"c" * (limit - 1) +
                b"\nvalid\nfinal\r\n")
    result = run(binary, str(boundary_file), "--max-line-bytes", str(limit))
    require(result.returncode == 0 and result.stdout == expected and
            b"skipped 2 invalid or oversized record(s)" in result.stderr,
            "reader should enforce the exact byte limit, drain oversized records, "
            "and preserve an unterminated final CR")
    csv_file = root / "reader-boundary.csv"
    exported = run(binary, str(boundary_file), "--max-line-bytes", str(limit),
                   "--export", "csv", "--output", str(csv_file),
                   "--columns", "line_number,raw")
    require(exported.returncode == 0, "reader boundary CSV export should succeed")
    with csv_file.open("r", encoding="utf-8", newline="") as stream:
        rows = list(csv.reader(stream))
    require(exported.returncode == 0 and rows == [
                ["line_number", "raw"], ["1", "a" * limit], ["3", ""],
                ["4", "c" * (limit - 1)], ["6", "valid"], ["7", "final\r"]],
            "CSV physical line numbers should include empty and oversized records")

    for ending, expected in ((b"\r", b"\n\nlast\r\n"),
                             (b"\r\n", b"\n\nlast\n")):
        boundary_file.write_bytes(b"\n\r\nlast" + ending)
        result = run(binary, str(boundary_file))
        require(result.returncode == 0 and result.stdout == expected,
                "reader should preserve empty records and normalize CR only before LF")

    output_buffer_file = root / "output-buffer.log"
    output_records = [
        ("row-{:04d}-".format(index).encode("ascii") + b"x" * 150 + b"\n")
        for index in range(5000)
    ]
    large_output_record = b"L" * (300 * 1024) + b"\n"
    expected_output = b"".join(output_records) + large_output_record
    output_buffer_file.write_bytes(expected_output)
    buffered_output = run(binary, str(output_buffer_file),
                          "--max-line-bytes", "400000")
    require(buffered_output.returncode == 0 and
            buffered_output.stdout == expected_output and
            buffered_output.stderr == b"",
            "buffered text output should preserve records across flush and direct-write boundaries")


def main():
    binary = Path(sys.argv[1]).resolve()
    help_result = run(binary, "--help")
    require(help_result.returncode == 0 and
            all(option in help_result.stdout for option in (
                b"--format", b"--pattern", b"--filter", b"--regex-field",
                b"--exclude-regex",
                b"--export", b"--stats", b"--follow", b"--tui",
                b"--stats-field", b"--syslog-year", b"--syslog-timezone",
                b"--profile", b"--save-profile", b"--profile-overwrite",
                b"--context-before", b"--context-after",
                b"--max-line-bytes", b"--version", b"cri")),
            "help should list the documented CLI features")
    version_result = run(binary, "--version")
    require(version_result.returncode == 0 and
            version_result.stdout == b"logview 1.7.0\n",
            "version should print the program name and version")
    scratch_root = Path(__file__).resolve().parents[1] / "build" / "test-tmp"
    scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="glogviewer-test-",
        dir=scratch_root,
    ) as temporary:
        root = Path(temporary)
        sample_directory = Path(__file__).resolve().parents[1] / "SAMPLES"
        tested_samples = set()
        crypto_sample = sample_directory / "crypto_historical_365days_1-level_10-MB_minified.json"
        if crypto_sample.is_file():
            crypto_date_range = run(
                binary, str(crypto_sample), "--format", "jsonl", "--filter",
                "date>=2024-12-10 and date<2024-12-16")
            crypto_rows = [json.loads(line) for line in crypto_date_range.stdout.splitlines()]
            require(crypto_date_range.returncode == 0 and
                    [row["date"] for row in crypto_rows] == [
                        "2024-12-10", "2024-12-11", "2024-12-12",
                        "2024-12-13", "2024-12-14", "2024-12-15"],
                    "the crypto sample should support a half-open timestamp range filter")
        check_reader_boundaries(binary, root)

        raw_file = root / "raw.log"
        raw_file.write_bytes(b"first timeout\r\nsecond line\nlast timeout")
        result = run(binary, str(raw_file), "--regex", "timeout$")
        require(result.returncode == 0, "raw regex should find matching records")
        require(result.stdout == b"first timeout\nlast timeout\n",
                "CRLF and unterminated final lines should be handled")
        excluded = run(binary, str(raw_file), "--regex", "timeout",
                       "--exclude-regex", "first")
        require(excluded.returncode == 0 and
                excluded.stdout == b"last timeout\n",
                "exclude regex should remove matching raw records while retaining included matches")
        excluded_only = run(binary, str(raw_file), "--exclude-regex", "timeout")
        require(excluded_only.returncode == 0 and
                excluded_only.stdout == b"second line\n",
                "exclude regex should work without a positive regex")

        cri_file = root / "container.cri.log"
        cri_lines = (
            b"2026-10-12T09:15:00.123456789Z stdout F application started\n"
            b"2026-10-12T09:15:01.000000001Z stderr P database connection "
            b"failed\n"
            b"2026-10-12T09:15:01.000000002Z stderr F retry exhausted\n"
        )
        cri_file.write_bytes(cri_lines)
        cri_filtered = run(binary, str(cri_file), "--format", "cri",
                           "--filter", "stream=stderr", "--regex-field", "tag=^F")
        require(cri_filtered.returncode == 0 and
                cri_filtered.stdout == cri_lines.splitlines(keepends=True)[2],
                "CRI parsing should expose stream and P/F tag fields for filtering")
        cri_auto = run(binary, str(cri_file), "--format", "auto")
        require(cri_auto.returncode == 0 and cri_auto.stdout == cri_lines and
                cri_auto.stderr == b"",
                "format auto-detection should recognize CRI timestamped records")
        cri_stats = run(binary, str(cri_file), "--format", "cri", "--stats")
        require(cri_stats.returncode == 0 and
                b"Selected records: 3" in cri_stats.stdout and
                b"Timestamped records: 3" in cri_stats.stdout and
                b"Unknown timestamps: 0" in cri_stats.stdout,
                "CRI timestamps should participate in the shared statistics timeline")
        cri_whitespace = root / "container-whitespace.cri.log"
        cri_whitespace.write_bytes(
            b"2026-10-12T09:15:02Z stdout F  leading payload spaces\n")
        cri_whitespace_result = run(binary, str(cri_whitespace), "--format", "cri",
                                    "--regex-field", "message=^ leading")
        require(cri_whitespace_result.returncode == 0 and
                cri_whitespace_result.stdout == cri_whitespace.read_bytes(),
                "CRI message parsing should preserve leading payload whitespace")
        malformed_cri = root / "container-malformed.cri.log"
        malformed_cri.write_bytes(
            b"2026-02-30T09:15:00Z stdout F invalid date\n" +
            b"2026-10-12T09:15:00Z other F invalid stream\n")
        malformed_cri_result = run(binary, str(malformed_cri), "--format", "cri")
        require(malformed_cri_result.returncode == 0 and
                malformed_cri_result.stdout == malformed_cri.read_bytes() and
                b"skipped 2 invalid or oversized record" in malformed_cri_result.stderr,
                "malformed CRI timestamps and stream names should be reported as rejected")

        multi_first = root / "multi-first.jsonl"
        multi_second = root / "multi-second.jsonl"
        multi_first.write_text(
            '{"timestamp":"2026-01-01T00:00:01Z","message":"first-a"}\n'
            '{"message":"unknown-a"}\n'
            '{"timestamp":"2026-01-01T00:00:03Z","message":"tie-a"}\n',
            encoding="utf-8")
        multi_second.write_text(
            '{"timestamp":"2026-01-01T00:00:02Z","message":"middle-b"}\n'
            '{"message":"unknown-b"}\n'
            '{"timestamp":"2026-01-01T00:00:03Z","message":"tie-b"}\n',
            encoding="utf-8")
        multi_result = run(binary, str(multi_first), str(multi_second),
                           "--format", "jsonl")
        expected_multi = (
            "[{}:1] {{\"timestamp\":\"2026-01-01T00:00:01Z\",\"message\":\"first-a\"}}\n"
            "[{}:1] {{\"timestamp\":\"2026-01-01T00:00:02Z\",\"message\":\"middle-b\"}}\n"
            "[{}:3] {{\"timestamp\":\"2026-01-01T00:00:03Z\",\"message\":\"tie-a\"}}\n"
            "[{}:3] {{\"timestamp\":\"2026-01-01T00:00:03Z\",\"message\":\"tie-b\"}}\n"
            "[{}:2] {{\"message\":\"unknown-a\"}}\n"
            "[{}:2] {{\"message\":\"unknown-b\"}}\n"
        ).format(multi_first, multi_second, multi_first, multi_second,
                 multi_first, multi_second).encode("utf-8")
        require(multi_result.returncode == 0 and
                multi_result.stdout == expected_multi and multi_result.stderr == b"",
                "multiple files should merge by timestamp, break ties by argument order, "
                "and append unknown timestamps in stable source order")
        multi_filtered = run(binary, str(multi_first), str(multi_second),
                             "--format", "jsonl", "--filter",
                             "message=middle-b or message=tie-a")
        require(multi_filtered.returncode == 0 and
                multi_filtered.stdout == (
                    "[{}:1] {{\"timestamp\":\"2026-01-01T00:00:02Z\",\"message\":\"middle-b\"}}\n"
                    "[{}:3] {{\"timestamp\":\"2026-01-01T00:00:03Z\",\"message\":\"tie-a\"}}\n"
                ).format(multi_second, multi_first).encode("utf-8"),
                "multi-file merge should apply structured filters before emission")
        multi_csv = root / "multi.csv"
        multi_export = run(binary, str(multi_first), str(multi_second),
                           "--format", "jsonl", "--export", "csv",
                           "--output", str(multi_csv), "--columns", "message")
        require(multi_export.returncode == 0,
                "multi-file CSV export should succeed")
        with multi_csv.open("r", encoding="utf-8", newline="") as stream:
            multi_rows = list(csv.reader(stream))
        require(multi_rows[0] == ["message", "source_file"] and
                [row[1] for row in multi_rows[1:]] == [
                    str(multi_first), str(multi_second), str(multi_first),
                    str(multi_second), str(multi_first), str(multi_second)],
                "multi-file CSV should always include the source file")
        multi_stats = run(binary, str(multi_first), str(multi_second),
                          "--format", "jsonl", "--stats")
        require(multi_stats.returncode == 0 and b"Selected records: 6" in multi_stats.stdout and
                b"Timestamped records: 4" in multi_stats.stdout and
                b"Unknown timestamps: 2" in multi_stats.stdout and
                multi_stats.stdout.startswith(b"Physical records:"),
                "multi-file statistics should aggregate selected and timestamp counts")
        unsorted_file = root / "multi-unsorted.jsonl"
        unsorted_file.write_text(
            '{"timestamp":"2026-01-01T00:00:02Z"}\n'
            '{"timestamp":"2026-01-01T00:00:01Z"}\n', encoding="utf-8")
        unsorted_result = run(binary, str(unsorted_file), str(multi_second),
                              "--format", "jsonl")
        require(unsorted_result.returncode == 2 and
                b"not ordered by timestamp" in unsorted_result.stderr,
                "multi-file analysis should reject a source with decreasing timestamps")
        duplicate_result = run(binary, str(multi_first), str(multi_first),
                               "--format", "jsonl")
        require(duplicate_result.returncode == 3,
                "multi-file analysis should reject duplicate input files")
        duplicate_alias = run(binary, str(multi_first),
                              os.path.relpath(multi_first, Path.cwd()),
                              "--format", "jsonl")
        require(duplicate_alias.returncode == 3 and
                b"specified more than once" in duplicate_alias.stderr,
                "multi-file analysis should reject alternate paths to the same file")
        missing_multi = run(binary, str(multi_first), str(root / "missing.jsonl"),
                            "--format", "jsonl")
        require(missing_multi.returncode == 2 and
                b"cannot open" in missing_multi.stderr,
                "multi-file analysis should report input open failures")
        mixed_raw = root / "multi-raw.log"
        mixed_raw.write_text("legacy event without a parsed timestamp\n", encoding="utf-8")
        mixed_result = run(binary, str(multi_first), str(mixed_raw), "--format", "auto")
        require(mixed_result.returncode == 0 and
                mixed_result.stdout.startswith(
                    ("[{}:1] {{\"timestamp\":\"2026-01-01T00:00:01Z\"".format(
                        multi_first)).encode("utf-8")) and
                mixed_result.stdout.endswith(
                    "[{}:1] legacy event without a parsed timestamp\n".format(
                        mixed_raw).encode("utf-8")),
                "auto format should detect JSON and raw formats independently per source")
        malformed_file = root / "multi-malformed.jsonl"
        malformed_file.write_text('{"timestamp":"2026-01-01T00:00:00Z"}\n'
                                  '{broken json marker}\n', encoding="utf-8")
        malformed_result = run(binary, str(malformed_file), str(multi_second),
                                "--format", "jsonl", "--regex", "broken")
        require(malformed_result.returncode == 0 and
                malformed_result.stdout == "[{}:2] {{broken json marker}}\n".format(
                    malformed_file).encode("utf-8") and
                b"skipped 1 invalid or oversized record" in malformed_result.stderr,
                "malformed selected records should remain available with source labels")

        if os.name != "nt":
            long_path_directory = root
            for index in range(5):
                long_path_directory = long_path_directory / (
                    "directory-{:02d}-{}".format(index, "x" * 55))
                long_path_directory.mkdir()
            long_path_file = long_path_directory / "long-path.log"
            long_path_file.write_bytes(b"long path record\n")
            require(len(os.fsencode(str(long_path_file))) > 260,
                    "long path fixture should exceed the traditional 260-byte path limit")
            long_path_result = run(binary, str(long_path_file))
            require(long_path_result.returncode == 0 and
                    long_path_result.stdout == b"long path record\n",
                    "the CLI should open a long nested input path")

        follow_file = root / "follow.log"
        follow_file.write_bytes(b"existing record\n")
        follower = subprocess.Popen(
            [str(binary), "--follow", str(follow_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            time.sleep(0.3)
            with follow_file.open("ab") as stream:
                stream.write(b"appended partial")
                stream.flush()
            try:
                follower.communicate(timeout=0.5)
                raise AssertionError("follow should remain active while waiting for a newline")
            except subprocess.TimeoutExpired as timeout:
                require(timeout.output in (None, b""),
                        "follow should hold an incomplete appended record")
            with follow_file.open("ab") as stream:
                stream.write(b" record\n")
                stream.flush()
            time.sleep(0.4)
        finally:
            follower.terminate()
            followed_output, followed_error = follower.communicate(timeout=5)
        require(b"appended partial record\n" in followed_output,
                "follow should emit an appended record after its newline arrives")
        require(b"existing record\n" not in followed_output and followed_error == b"",
                "follow should start at EOF by default without replaying existing records")

        truncation_file = root / "follow-truncate.log"
        truncation_file.write_bytes(b"initial content\n")
        truncation_process = subprocess.Popen(
            [str(binary), "--follow", str(truncation_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            time.sleep(0.3)
            truncation_file.write_bytes(b"after truncate\n")
            time.sleep(0.6)
        finally:
            truncation_process.terminate()
            truncated_output, truncated_error = truncation_process.communicate(timeout=5)
        require(b"after truncate\n" in truncated_output and truncated_error == b"",
                "follow should restart from byte zero when the file is truncated")

        start_follow_file = root / "follow-from-start.log"
        start_follow_file.write_bytes(b"replayed existing record\n")
        start_follow_process = subprocess.Popen(
            [str(binary), "--follow", "--from-start", str(start_follow_file)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            time.sleep(0.4)
        finally:
            start_follow_process.terminate()
            start_output, start_error = start_follow_process.communicate(timeout=5)
        require(start_output == b"replayed existing record\n" and start_error == b"",
                "--from-start should process existing records before following")

        if os.name != "nt":
            rotation_file = root / "follow-rotation.log"
            rotated_old_file = root / "follow-rotation.old"
            rotation_file.write_bytes(b"old generation\n")
            rotation_process = subprocess.Popen(
                [str(binary), "--follow", str(rotation_file)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                time.sleep(0.3)
                os.replace(rotation_file, rotated_old_file)
                rotation_file.write_bytes(b"new generation\n")
                time.sleep(0.7)
            finally:
                rotation_process.terminate()
                rotation_output, rotation_error = rotation_process.communicate(timeout=5)
            require(rotation_output == b"new generation\n" and rotation_error == b"",
                    "follow should reopen a rotated file at its beginning")

            reopen_file = root / "follow-reopen.log"
            reopen_old = root / "follow-reopen.old"
            reopen_file.write_bytes(b"")
            reopen_process = subprocess.Popen(
                [str(binary), "--follow", str(reopen_file)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                time.sleep(0.3)
                os.replace(reopen_file, reopen_old)
                time.sleep(0.4)
                reopen_file.write_bytes(b"reopened generation\n")
                time.sleep(1.2)
            finally:
                reopen_process.terminate()
                reopen_output, reopen_error = reopen_process.communicate(timeout=5)
            require(reopen_output == b"reopened generation\n" and reopen_error == b"",
                    "follow should recover when a rotated path is temporarily missing; "
                    "output={!r}, stderr={!r}".format(reopen_output, reopen_error))

            flush_file = root / "follow-flush.log"
            flush_file.write_bytes(b"")
            flush_process = subprocess.Popen(
                [str(binary), "--follow", "--flush-partial", str(flush_file)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                time.sleep(0.3)
                with flush_file.open("ab") as stream:
                    stream.write(b"final partial")
                    stream.flush()
                flush_process.send_signal(signal.SIGINT)
                flush_output, flush_error = flush_process.communicate(timeout=5)
            finally:
                if flush_process.poll() is None:
                    flush_process.kill()
                    flush_process.communicate(timeout=5)
            require(flush_process.returncode == 0 and flush_output == b"final partial\n" and
                    flush_error == b"",
                    "--flush-partial should emit the pending record on Ctrl-C")

        json_file = root / "app.jsonl"
        json_file.write_bytes(
            b'{"level":"INFO","message":"timeout"}\r\n'
            b'{"level":"ERROR","message":"connection timeout"}\n'
            b'{"severity":"ERROR","msg":"timeout"}\n'
            b'{"level":"ERROR","message":"other"}\n'
            b'{"level":"ERROR","message":"bad",}\n'
            b'{"level":"ERROR","level":"WARN","message":"timeout"}\n'
            b'{"level":"ERROR","message":"nested","details":{"code":3}}\n'
            b'{"level":"ERROR","message":"bad surrogate \\uD800"}\n'
        )
        result = run(
            binary,
            str(json_file),
            "--format",
            "jsonl",
            "--filter",
            "level=ERROR",
            "--regex-field",
            "message=timeout",
        )
        require(result.returncode == 0, "JSON field filter should find records")
        require(
            result.stdout
            == b'{"level":"ERROR","message":"connection timeout"}\n'
            b'{"severity":"ERROR","msg":"timeout"}\n',
            "canonical aliases and field regex should select matching JSON records",
        )
        profile_path = root / "incident.glogprofile"
        profile_saved = run(
            binary, str(json_file), "--format", "jsonl", "--filter", "level=ERROR",
            "--regex-field", "message=timeout", "--context-before", "0",
            "--context-after", "2", "--exclude-regex", "connection",
            "--save-profile", str(profile_path),
        )
        require(profile_saved.returncode == 0 and
                b'"message":"connection timeout"' not in profile_saved.stdout and
                b'"msg":"timeout"' in profile_saved.stdout and
                profile_path.read_text(encoding="utf-8").startswith("GLOGVIEW_PROFILE 1\n"),
                "saving a versioned query profile should preserve include and exclude regex filters")
        profile_loaded = run(binary, str(json_file), "--profile", str(profile_path))
        require(profile_loaded.returncode == 0 and
                profile_loaded.stdout == profile_saved.stdout,
                "a saved query profile should reproduce its batch selection")
        crlf_profile_path = root / "crlf.glogprofile"
        crlf_profile_path.write_bytes(profile_path.read_bytes().replace(b"\n", b"\r\n"))
        crlf_profile = run(binary, str(json_file), "--profile", str(crlf_profile_path))
        require(crlf_profile.returncode == 0 and
                crlf_profile.stdout == profile_saved.stdout,
                "query profiles should be portable across LF and CRLF line endings")
        profile_override = run(
            binary, str(json_file), "--profile", str(profile_path),
            "--regex-field", "message=other",
        )
        require(profile_override.returncode == 0 and
                b'"message":"other"' in profile_override.stdout and
                b'"message":"connection timeout"' not in profile_override.stdout,
                "explicit command-line filters should override loaded profile values")
        profile_refused = run(
            binary, str(json_file), "--profile", str(profile_path),
            "--save-profile", str(profile_path),
        )
        require(profile_refused.returncode == 2 and b"already exists" in profile_refused.stderr,
                "saving a profile should refuse to overwrite by default")
        profile_replaced = run(
            binary, str(json_file), "--profile", str(profile_path),
            "--save-profile", str(profile_path), "--profile-overwrite",
        )
        require(profile_replaced.returncode == 0 and
                b"saved query profile" in profile_replaced.stderr,
                "explicit profile overwrite should replace the existing profile")
        invalid_profile_path = root / "invalid.glogprofile"
        invalid_profile_path.write_text(
            "GLOGVIEW_PROFILE 1\nunknown_setting=value\n", encoding="utf-8")
        invalid_profile = run(binary, str(json_file), "--profile",
                              str(invalid_profile_path))
        require(invalid_profile.returncode == 3 and
                b"invalid or unsupported setting" in invalid_profile.stderr,
                "invalid and stale profile settings should fail with a diagnostic")
        compound = run(
            binary,
            str(json_file),
            "--format",
            "jsonl",
            "--filter",
            'level=INFO or level=ERROR and message~"connection|timeout"',
        )
        require(
            compound.returncode == 0
            and compound.stdout
            == b'{"level":"INFO","message":"timeout"}\n'
            b'{"level":"ERROR","message":"connection timeout"}\n'
            b'{"severity":"ERROR","msg":"timeout"}\n',
            "filter precedence should evaluate AND before OR and support field regex",
        )
        top_records = run(binary, str(json_file), "--format", "jsonl",
                          "--filter", "level=ERROR", "--top", "2")
        bottom_records = run(binary, str(json_file), "--format", "jsonl",
                             "--filter", "level=ERROR", "--bottom", "2")
        first_record = run(binary, str(json_file), "--format", "jsonl",
                           "--record", "first")
        last_record = run(binary, str(json_file), "--format", "jsonl",
                          "--record", "last")
        require(top_records.returncode == 0 and
                top_records.stdout == (b'{"level":"ERROR","message":"connection timeout"}\n'
                                       b'{"severity":"ERROR","msg":"timeout"}\n'),
                "--top should emit the first N records matching active filters")
        require(bottom_records.returncode == 0 and
                bottom_records.stdout == (b'{"level":"ERROR","message":"other"}\n'
                                          b'{"level":"ERROR","message":"nested","details":{"code":3}}\n'),
                "--bottom should emit the last N matching records in source order: {!r} {!r} {!r}".format(
                    bottom_records.returncode, bottom_records.stdout, bottom_records.stderr))
        require(first_record.returncode == 0 and
                first_record.stdout == b'{"level":"INFO","message":"timeout"}\n' and
                last_record.returncode == 0 and
                last_record.stdout == b'{"level":"ERROR","message":"bad surrogate \\uD800"}\n',
                "--record first/last should print the complete matching source record")
        date_file = root / "date-range.jsonl"
        date_file.write_bytes(
            b'{"timestamp":"2024-01-01T23:59:59Z","id":1}\n'
            b'{"timestamp":"2024-01-02T00:00:00Z","id":2}\n'
            b'{"timestamp":"2024-01-03T12:00:00Z","id":3}\n'
            b'{"timestamp":"2024-01-04T00:00:00Z","id":4}\n'
        )
        date_range = run(binary, str(date_file), "--format", "jsonl", "--filter",
                         "timestamp>=2024-01-02T00:00:00Z and timestamp<2024-01-04T00:00:00Z")
        require(date_range.returncode == 0 and
                date_range.stdout == (b'{"timestamp":"2024-01-02T00:00:00Z","id":2}\n'
                                      b'{"timestamp":"2024-01-03T12:00:00Z","id":3}\n'),
                "field comparison operators should support inclusive/exclusive string ranges")
        grouped = run(
            binary,
            str(json_file),
            "--format",
            "jsonl",
            "--filter",
            '(level=INFO or level=ERROR) and message=timeout',
        )
        require(grouped.returncode == 0 and grouped.stdout.count(b"\n") == 2,
                "parentheses should override default filter precedence")
        statistics = run(binary, str(json_file), "--format", "jsonl",
                         "--filter", "level=ERROR", "--stats")
        require(statistics.returncode == 0 and
                b"Physical records: 8\n" in statistics.stdout and
                b"Parsed records: 5\n" in statistics.stdout and
                b"Rejected records: 3\n" in statistics.stdout and
                b"Selected records: 4\n" in statistics.stdout and
                b"  ERROR: 4\n" in statistics.stdout,
                "statistics should report exact record and selected-level counts")
        cardinality_file = root / "high-cardinality.jsonl"
        with cardinality_file.open("wb") as stream:
            for _ in range(900):
                stream.write(b'{"service":"api"}\n')
            for number in range(1000):
                stream.write(( '{{"service":"user-{}"}}\n'.format(number)
                              ).encode("ascii"))
        cardinality_stats = run(binary, str(cardinality_file),
                                "--format", "jsonl", "--stats")
        require(cardinality_stats.returncode == 0 and
                b"Physical records: 1900\n" in cardinality_stats.stdout and
                b"Selected records: 1900\n" in cardinality_stats.stdout and
                b"service=api: 900 (exact)" in cardinality_stats.stdout,
                "statistics should retain exact counts and bounded heavy hitters at high cardinality")
        timeline_file = root / "timeline.jsonl"
        timeline_file.write_bytes(
            b'{"timestamp":"2025-01-01T00:00:00Z","level":"INFO","service":"api"}\n'
            b'{"timestamp":"2025-01-02T12:00:00+02:00","level":"ERROR","service":"api"}\n'
            b'{"timestamp":"bad","level":"ERROR","service":"web"}\n'
        )
        timeline = run(binary, str(timeline_file), "--format", "jsonl", "--stats")
        require(timeline.returncode == 0 and
                b"Unknown timestamps: 1\n" in timeline.stdout and
                b"  000: 1\n" in timeline.stdout and
                b"  255: 1\n" in timeline.stdout and
                b"service=api: 2 (exact)\n" in timeline.stdout,
                "timeline should normalize UTC offsets, and heavy hitters should count field values")
        syslog_timeline_file = root / "syslog3164-timeline.log"
        syslog_timeline_file.write_bytes(
            b"Jun  4 15:16:01 host app: first\n"
            b"Jun 14 15:17:01 host app: second\n"
            b"Jun 14 15:18:01 host app: third\n")
        ambiguous_syslog_stats = run(binary, str(syslog_timeline_file),
                                     "--format", "syslog3164", "--stats")
        require(ambiguous_syslog_stats.returncode == 0 and
                b"Timestamped records: 0\nUnknown timestamps: 3\n" in
                ambiguous_syslog_stats.stdout,
                "RFC 3164 timestamps should remain unknown without an explicit year")
        configured_syslog_stats = run(
            binary, str(syslog_timeline_file), "--format", "syslog3164",
            "--syslog-year", "2025", "--syslog-timezone", "+02:00", "--stats")
        require(configured_syslog_stats.returncode == 0 and
                b"Timestamped records: 3\nUnknown timestamps: 0\n" in
                configured_syslog_stats.stdout and
                b"  000: 1\n" in configured_syslog_stats.stdout and
                b"  255: 2\n" in configured_syslog_stats.stdout,
                "RFC 3164 timestamps should use the configured year and UTC offset")
        timezone_profile = root / "timezone.glogprofile"
        timezone_profile_saved = run(
            binary, str(syslog_timeline_file), "--format", "syslog3164",
            "--syslog-year", "2025", "--syslog-timezone", "+23:59",
            "--save-profile", str(timezone_profile))
        require(timezone_profile_saved.returncode == 0 and
                b"syslog_timezone=+23:59\n" in timezone_profile.read_bytes(),
                "query profiles should preserve the maximum valid timezone offset")
        utc_syslog_stats = run(
            binary, str(syslog_timeline_file), "--format", "syslog3164",
            "--syslog-year", "2025", "--stats")
        configured_range = re.search(
            rb"Timeline range \(UTC epoch seconds\): (\d+)\.\.(\d+)",
            configured_syslog_stats.stdout)
        utc_range = re.search(
            rb"Timeline range \(UTC epoch seconds\): (\d+)\.\.(\d+)",
            utc_syslog_stats.stdout)
        require(configured_range is not None and utc_range is not None and
                int(configured_range.group(1)) == int(utc_range.group(1)) - 7200 and
                int(configured_range.group(2)) == int(utc_range.group(2)) - 7200,
                "RFC 3164 timezone offsets should normalize local timestamps to UTC")
        invalid_leap_day_file = root / "syslog3164-invalid-leap-day.log"
        invalid_leap_day_file.write_bytes(b"Feb 29 12:00:00 host app: leap day\n")
        invalid_leap_day_stats = run(
            binary, str(invalid_leap_day_file), "--format", "syslog3164",
            "--syslog-year", "2025", "--stats")
        require(invalid_leap_day_stats.returncode == 0 and
                b"Timestamped records: 0\nUnknown timestamps: 1\n" in
                invalid_leap_day_stats.stdout,
                "RFC 3164 dates should be validated against the configured year's calendar")
        missing_syslog_format = run(binary, str(syslog_timeline_file),
                                    "--syslog-year", "2025")
        require(missing_syslog_format.returncode == 3,
                "RFC 3164 date settings should require syslog3164 or auto format")
        short_syslog_year = run(
            binary, str(syslog_timeline_file), "--format", "syslog3164",
            "--syslog-year", "25")
        require(short_syslog_year.returncode == 3,
                "RFC 3164 year settings should require four digits")
        invalid_syslog_timezone = run(
            binary, str(syslog_timeline_file), "--format", "syslog3164",
            "--syslog-year", "2025", "--syslog-timezone", "+25:00")
        require(invalid_syslog_timezone.returncode == 3,
                "invalid syslog timezone offsets should be rejected")
        service_stats = run(binary, str(timeline_file), "--format", "jsonl",
                            "--stats-field", "service")
        require(service_stats.returncode == 0 and
                b"Frequent values for service:\n" in service_stats.stdout and
                b"service=api: 2 (exact)\n" in service_stats.stdout and
                b"level=INFO:" not in service_stats.stdout,
                "--stats-field should focus frequent-value output on the requested field")
        filtered_service_stats = run(
            binary, str(timeline_file), "--format", "jsonl", "--filter",
            "level=ERROR", "--stats-field", "service")
        require(filtered_service_stats.returncode == 0 and
                b"Selected records: 2\n" in filtered_service_stats.stdout and
                b"service=api: 1 (exact)" in filtered_service_stats.stdout and
                b"service=web: 1 (exact)" in filtered_service_stats.stdout and
                b"level=INFO:" not in filtered_service_stats.stdout,
                "field statistics should apply the active filter before counting values")
        estimated_stats = run(binary, str(cardinality_file), "--format", "jsonl",
                              "--stats-field", "service")
        require(estimated_stats.returncode == 0 and
                b"(estimated; error <= " in estimated_stats.stdout,
                "high-cardinality focused statistics should label approximate candidates and bounds")
        access_file = root / "access-stats.log"
        access_line = (
            b'203.0.113.8 - alice [10/Oct/2000:13:55:36 -0700] '
            b'"GET /index.html HTTP/1.1" 200 2326 "https://example.test/" '
            b'"Mozilla/5.0 test agent"\n'
        )
        access_file.write_bytes(access_line)
        apache_stats = run(binary, str(access_file), "--format", "apache-combined", "--stats")
        require(apache_stats.returncode == 0 and b"Unknown timestamps: 0\n" in apache_stats.stdout and
                b"Timeline counts:\n" in apache_stats.stdout,
                "Apache combined timestamps should contribute to timeline statistics")
        invalid_filter = run(binary, str(json_file), "--filter", "(level=ERROR")
        require(invalid_filter.returncode == 3,
                "unclosed filter parentheses should return syntax status")
        tui_without_terminal = run(binary, str(json_file), "--tui")
        require(tui_without_terminal.returncode == 2 and
                b"requires an interactive" in tui_without_terminal.stderr,
                "TUI should report a clear error when stdin or stdout is not a terminal")
        tui_conflict = run(binary, str(json_file), "--tui", "--stats")
        require(tui_conflict.returncode == 3,
                "TUI and statistics output modes should be mutually exclusive")
        tui_follow_partial = run(binary, str(json_file), "--tui", "--follow",
                                 "--flush-partial")
        require(tui_follow_partial.returncode == 3,
                "TUI follow should reject the batch-only partial flush option")

        if os.name != "nt":
            import fcntl
            import pty
            import select
            import struct
            import termios

            tui_file = root / "tui.jsonl"
            tui_records = [
                b'{"level":"INFO","message":"startup"}\n',
                b'{"level":"ERROR","message":"connection timeout"}\n',
                b'{"level":"WARN","message":"disk usage high"}\n',
                b'{"level":"INFO","message":"second connection timeout"}\n',
            ]
            tui_records.extend(
                ('{"level":"INFO","message":"filler %d"}\n' % number).encode("ascii")
                for number in range(600)
            )
            tui_file.write_bytes(b"".join(tui_records))
            master_fd, slave_fd = pty.openpty()
            fcntl.ioctl(slave_fd, termios.TIOCSWINSZ,
                        struct.pack("HHHH", 24, 100, 0, 0))
            original_terminal = termios.tcgetattr(slave_fd)
            tui_process = subprocess.Popen(
                [str(binary), str(tui_file), "--format", "jsonl", "--tui"],
                stdin=slave_fd,
                stdout=slave_fd,
                stderr=slave_fd,
                close_fds=True,
            )
            tui_output = bytearray()
            try:
                def read_until(needle, timeout, start=0):
                    deadline = time.monotonic() + timeout
                    while needle not in tui_output[start:] and tui_process.poll() is None and time.monotonic() < deadline:
                        ready, _, _ = select.select([master_fd], [], [], 0.1)
                        if ready:
                            try:
                                tui_output.extend(os.read(master_fd, 65536))
                            except OSError:
                                break

                def read_until_count(needle, count, timeout):
                    deadline = time.monotonic() + timeout
                    while tui_output.count(needle) < count and tui_process.poll() is None and time.monotonic() < deadline:
                        ready, _, _ = select.select([master_fd], [], [], 0.1)
                        if ready:
                            try:
                                tui_output.extend(os.read(master_fd, 65536))
                            except OSError:
                                break

                read_until(b"GLogViewer", 2)
                read_until(b"Scan complete", 2)
                os.write(master_fd, b"/")
                read_until(b"\033[24;1H\033[2K/", 2)
                os.write(master_fd, b"timeout\n")
                read_until(b"Search: timeout", 2)
                read_until(b"\033[7m       2", 2)
                enter_output_start = len(tui_output)
                os.write(master_fd, b"\r")
                read_until(b"\033[7m       4", 2, enter_output_start)
                reverse_output_start = len(tui_output)
                os.write(master_fd, b"\x1b[13;2u")
                read_until(b"\033[7m       2", 2, reverse_output_start)
                next_output_start = len(tui_output)
                os.write(master_fd, b"n")
                read_until(b"\033[7m       4", 2, next_output_start)
                warning_output_start = len(tui_output)
                os.write(master_fd, b"w")
                read_until(b'3 {"level":"WARN","message":"disk usage high"}',
                           2, warning_output_start)
                fcntl.ioctl(master_fd, termios.TIOCSWINSZ,
                            struct.pack("HHHH", 12, 60, 0, 0))
                read_until(b"\033[11;1H\033[2K", 2)
                os.write(master_fd, b"f")
                read_until(b"Filter: ", 2)
                os.write(master_fd, b"(level=ERROR\n")
                read_until(b"Invalid filter:", 2)
                os.write(master_fd, b"f")
                read_until(b"Filter: ", 2)
                os.write(master_fd, b"level=ERROR\n")
                read_until_count(b"Scan complete", 2, 3)
                os.write(master_fd, b"r")
                read_until(b"Regex: ", 2)
                os.write(master_fd, b"timeout\n")
                read_until_count(b"Scan complete", 3, 3)
                os.write(master_fd, b"/")
                read_until(b"\033[12;1H\033[2K/", 2)
                os.write(master_fd, b"timeout\n")
                read_until(b"Search: timeout", 2)
                os.write(master_fd, b"C")
                read_until(b"Scanning input", 2)
                read_until(b"selected: 604", 3)
                read_until_count(b"Scan complete", 4, 3)
                field_summary_start = len(tui_output)
                os.write(master_fd, b"v")
                read_until(b"Summarize field: ", 2)
                os.write(master_fd, b"level\r")
                read_until(b"Field values | level", 2, field_summary_start)
                read_until(b"ERROR", 2, field_summary_start)
                os.write(master_fd, b"\x1b")
                read_until(b"Search: (none)", 2)
                os.write(master_fd, b"q")
                try:
                    tui_process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process_state = "unavailable"
                    process_wait_channel = "unavailable"
                    if Path("/proc").is_dir():
                        try:
                            process_state = Path("/proc/{}/status".format(
                                tui_process.pid)).read_text(encoding="utf-8")
                            process_wait_channel = Path("/proc/{}/wchan".format(
                                tui_process.pid)).read_text(encoding="utf-8")
                        except OSError:
                            pass
                    require(False,
                            "TUI process did not exit after quit; status={!r}, wchan={!r}, output={!r}".format(
                                process_state[-1000:], process_wait_channel,
                                bytes(tui_output[-2000:])))
                drain_deadline = time.monotonic() + 1
                while time.monotonic() < drain_deadline:
                    try:
                        ready, _, _ = select.select([master_fd], [], [], 0.05)
                    except OSError:
                        break
                    if not ready:
                        break
                    try:
                        chunk = os.read(master_fd, 65536)
                    except OSError:
                        break
                    if not chunk:
                        break
                    tui_output.extend(chunk)
                restored_terminal = termios.tcgetattr(slave_fd)
                terminal_restored = (
                    (restored_terminal[0] & (termios.IXON | termios.ICRNL)) ==
                    (original_terminal[0] & (termios.IXON | termios.ICRNL)) and
                    (restored_terminal[3] & (termios.ICANON | termios.ECHO)) ==
                    (original_terminal[3] & (termios.ICANON | termios.ECHO)) and
                    restored_terminal[6][termios.VMIN] == original_terminal[6][termios.VMIN] and
                    restored_terminal[6][termios.VTIME] == original_terminal[6][termios.VTIME]
                )
                changed_terminal_fields = []
                if (restored_terminal[0] & (termios.IXON | termios.ICRNL)) != \
                        (original_terminal[0] & (termios.IXON | termios.ICRNL)):
                    changed_terminal_fields.append((0, original_terminal[0], restored_terminal[0]))
                if (restored_terminal[3] & (termios.ICANON | termios.ECHO)) != \
                        (original_terminal[3] & (termios.ICANON | termios.ECHO)):
                    changed_terminal_fields.append((3, original_terminal[3], restored_terminal[3]))
                if restored_terminal[6][termios.VMIN] != original_terminal[6][termios.VMIN]:
                    changed_terminal_fields.append(("VMIN", original_terminal[6][termios.VMIN],
                                                    restored_terminal[6][termios.VMIN]))
                if restored_terminal[6][termios.VTIME] != original_terminal[6][termios.VTIME]:
                    changed_terminal_fields.append(("VTIME", original_terminal[6][termios.VTIME],
                                                    restored_terminal[6][termios.VTIME]))
                tui_checks = {
                    "normal exit": tui_process.returncode == 0,
                    "search result": b"connection timeout" in tui_output,
                    "error navigation": b'2 {"level":"ERROR","message":"connection timeout"}' in tui_output,
                    "warning navigation": b'3 {"level":"WARN","message":"disk usage high"}' in tui_output,
                    "resize redraw": b"\033[11;1H\033[2K" in tui_output,
                    "search status": b"Search: timeout" in tui_output,
                    "enter search navigation": b"\033[7m       4" in tui_output,
                    "shift-enter reverse navigation": b"\033[7m       2" in tui_output,
                    "clear resets filters and search": b"selected: 604" in tui_output,
                    "invalid filter diagnostic": b"Invalid filter:" in tui_output,
                    "field value summary": b"Field values | level" in tui_output and
                                          b"ERROR  1 (exact)" in tui_output,
                    "terminal mode restoration": terminal_restored,
                }
                failed_checks = [name for name, passed in tui_checks.items() if not passed]
                require(not failed_checks,
                        "TUI PTY checks failed: {}; terminal fields changed: {}; output tail={!r}".format(
                            failed_checks,
                            changed_terminal_fields,
                            bytes(tui_output[-1200:])))
            finally:
                if tui_process.poll() is None:
                    tui_process.kill()
                    tui_process.wait(timeout=5)
                os.close(master_fd)
                os.close(slave_fd)

            def check_limited_tui(option, expected_rows, label):
                limited_master, limited_slave = pty.openpty()
                fcntl.ioctl(limited_slave, termios.TIOCSWINSZ,
                            struct.pack("HHHH", 24, 100, 0, 0))
                limited_process = subprocess.Popen(
                    [str(binary), str(tui_file), "--format", "jsonl", option, "2", "--tui"],
                    stdin=limited_slave,
                    stdout=limited_slave,
                    stderr=limited_slave,
                    close_fds=True,
                )
                captured = bytearray()
                deadline = time.monotonic() + 5
                try:
                    while b"Scan complete" not in captured and limited_process.poll() is None and \
                            time.monotonic() < deadline:
                        ready, _, _ = select.select([limited_master], [], [], 0.1)
                        if ready:
                            try:
                                captured.extend(os.read(limited_master, 65536))
                            except OSError:
                                break
                    os.write(limited_master, b"q")
                    limited_process.wait(timeout=5)
                    while True:
                        ready, _, _ = select.select([limited_master], [], [], 0.05)
                        if not ready:
                            break
                        try:
                            captured.extend(os.read(limited_master, 65536))
                        except OSError:
                            break
                    require(limited_process.returncode == 0 and
                            b"selected: 2" in captured and all(row in captured for row in expected_rows),
                            "{} TUI should display the requested limited result window: {!r}".format(
                                label, bytes(captured[-1500:])))
                finally:
                    if limited_process.poll() is None:
                        limited_process.kill()
                        limited_process.wait(timeout=5)
                    os.close(limited_master)
                    os.close(limited_slave)

            check_limited_tui("--top", [b'"message":"startup"',
                                         b'"message":"connection timeout"'], "top")
            check_limited_tui("--bottom", [b'"message":"filler 598"',
                                            b'"message":"filler 599"'], "bottom")

            context_master, context_slave = pty.openpty()
            fcntl.ioctl(context_slave, termios.TIOCSWINSZ,
                        struct.pack("HHHH", 24, 100, 0, 0))
            context_process = subprocess.Popen(
                [str(binary), str(tui_file), "--format", "jsonl", "--tui"],
                stdin=context_slave,
                stdout=context_slave,
                stderr=context_slave,
                close_fds=True,
            )
            context_output = bytearray()
            try:
                def context_read_until(needle, timeout, start=0):
                    deadline = time.monotonic() + timeout
                    while needle not in context_output[start:] and \
                            context_process.poll() is None and time.monotonic() < deadline:
                        ready, _, _ = select.select([context_master], [], [], 0.1)
                        if ready:
                            try:
                                context_output.extend(os.read(context_master, 65536))
                            except OSError:
                                break

                context_read_until(b"Scan complete", 3)
                selection_start = len(context_output)
                os.write(context_master, b"\x1b[H")
                context_read_until(b"\033[7m       1", 2, selection_start)
                selection_start = len(context_output)
                os.write(context_master, b"\x1b[B")
                context_read_until(b"\033[7m       2", 2, selection_start)
                selection_start = len(context_output)
                os.write(context_master, b"\x1b[B")
                context_read_until(b"\033[7m       3", 2, selection_start)
                context_start = len(context_output)
                os.write(context_master, b"c")
                context_read_until(b"Context around source line 3", 2, context_start)
                context_read_until(b'2 {"level":"ERROR","message":"connection timeout"}',
                                   2, context_start)
                require(b"Context around source line 3" in context_output[context_start:] and
                        b'2 {"level":"ERROR","message":"connection timeout"}' in
                        context_output[context_start:],
                        "TUI context view should show physical records around the selected row; "
                        "output={!r}".format(bytes(context_output[context_start:])))
                bookmark_start = len(context_output)
                os.write(context_master, b"b\x1b[A B")
                context_read_until(b"Jumped to bookmark: source line 3", 2,
                                   bookmark_start)
                context_size_start = len(context_output)
                os.write(context_master, b"+")
                context_read_until(b"Context size: 5 before, 5 after", 2,
                                   context_size_start)
                os.write(context_master, b"q")
                context_process.wait(timeout=3)
                require(context_process.returncode == 0 and
                        b"Bookmarked source line 3" in context_output[bookmark_start:] and
                        b"\033[7;92m       3" in context_output[bookmark_start:] and
                        b"Context size: 5 before, 5 after" in context_output[bookmark_start:],
                        "TUI should render bookmarked lines in light green and adjust context size")
            finally:
                if context_process.poll() is None:
                    context_process.kill()
                    context_process.wait(timeout=5)
                os.close(context_master)
                os.close(context_slave)

            jump_file = root / "tui-jump.jsonl"
            jump_file.write_bytes(
                b'{"level":"INFO","message":"line one"}\n'
                b'{"level":"ERROR","message":"line two"}\n'
                b'{"level":"WARN","message":"line three"}\n'
                b'{"level":"ERROR","message":"line four"}\n'
            )
            jump_master, jump_slave = pty.openpty()
            fcntl.ioctl(jump_slave, termios.TIOCSWINSZ,
                        struct.pack("HHHH", 24, 100, 0, 0))
            jump_process = subprocess.Popen(
                [str(binary), str(jump_file), "--format", "jsonl",
                 "--filter", "level=ERROR", "--tui"],
                stdin=jump_slave,
                stdout=jump_slave,
                stderr=jump_slave,
                close_fds=True,
            )
            jump_output = bytearray()
            try:
                def jump_read_until(needle, timeout, start=0):
                    deadline = time.monotonic() + timeout
                    while needle not in jump_output[start:] and \
                            jump_process.poll() is None and time.monotonic() < deadline:
                        ready, _, _ = select.select([jump_master], [], [], 0.1)
                        if ready:
                            try:
                                jump_output.extend(os.read(jump_master, 65536))
                            except OSError:
                                break

                jump_read_until(b"Scan complete", 3)
                jump_start = len(jump_output)
                os.write(jump_master, b"J3\n")
                jump_read_until(b"No selected record on line 3; jumped to line 4",
                                2, jump_start)
                require(b"\033[7m       4 {\"level\":\"ERROR\"" in
                        jump_output[jump_start:],
                        "TUI jump should select the next visible record after a filtered source line")
                jump_start = len(jump_output)
                os.write(jump_master, b"J2\n")
                jump_read_until(b"Jumped to source line 2", 2, jump_start)
                require(b"\033[7m       2 {\"level\":\"ERROR\"" in
                        jump_output[jump_start:],
                        "TUI jump should select an exact source line")
                jump_start = len(jump_output)
                os.write(jump_master, b"Jabc\n")
                jump_read_until(b"Enter a positive source line number", 2, jump_start)
                jump_start = len(jump_output)
                os.write(jump_master, b"J5\n")
                jump_read_until(b"beyond the end of the file", 2, jump_start)
                os.write(jump_master, b"q")
                jump_process.wait(timeout=3)
                require(jump_process.returncode == 0 and
                        b"No selected record on line 3; jumped to line 4" in jump_output and
                        b"Jumped to source line 2" in jump_output and
                        b"Enter a positive source line number" in jump_output and
                        b"beyond the end of the file" in jump_output,
                        "TUI jump should report exact, filtered, invalid, and out-of-range requests")
            finally:
                if jump_process.poll() is None:
                    jump_process.kill()
                    jump_process.wait(timeout=5)
                os.close(jump_master)
                os.close(jump_slave)

            navigation_file = root / "tui-search-navigation.jsonl"
            navigation_file.write_bytes(b"".join(
                ('{"message":"navigation record %05d"}\n' % number).encode("ascii")
                for number in range(10000)
            ))
            navigation_master, navigation_slave = pty.openpty()
            fcntl.ioctl(navigation_slave, termios.TIOCSWINSZ,
                        struct.pack("HHHH", 24, 100, 0, 0))
            navigation_process = subprocess.Popen(
                [str(binary), str(navigation_file), "--format", "jsonl", "--tui"],
                stdin=navigation_slave,
                stdout=navigation_slave,
                stderr=navigation_slave,
                close_fds=True,
            )
            navigation_output = bytearray()
            try:
                def navigation_read_until(needle, timeout):
                    deadline = time.monotonic() + timeout
                    while needle not in navigation_output and \
                            navigation_process.poll() is None and time.monotonic() < deadline:
                        ready, _, _ = select.select([navigation_master], [], [], 0.1)
                        if ready:
                            try:
                                navigation_output.extend(os.read(navigation_master, 65536))
                            except OSError:
                                break

                navigation_read_until(b"Scan complete", 3)
                os.write(navigation_master, b"/")
                navigation_read_until(b"\033[24;1H\033[2K/", 2)
                os.write(navigation_master, b"not-present\n\x1b")
                navigation_read_until(b"Search cancelled", 3)
                os.write(navigation_master, b"q")
                navigation_process.wait(timeout=3)
                require(navigation_process.returncode == 0 and
                        b"Searching:" in navigation_output and
                        b"Search cancelled" in navigation_output,
                        "large TUI searches should remain responsive and cancellable")
            finally:
                if navigation_process.poll() is None:
                    navigation_process.kill()
                    navigation_process.wait(timeout=5)
                os.close(navigation_master)
                os.close(navigation_slave)

            boolean_search_file = root / "tui-boolean-search.log"
            boolean_search_file.write_text(
                "66.249 client request 20/May/2015:20\n"
                "66.249 client request 21/May/2015:20\n"
                "other client request 20/May/2015:20\n"
                "20/May/2015:20 request from 66.249\n",
                encoding="ascii",
            )
            boolean_master, boolean_slave = pty.openpty()
            fcntl.ioctl(boolean_slave, termios.TIOCSWINSZ,
                        struct.pack("HHHH", 24, 100, 0, 0))
            boolean_process = subprocess.Popen(
                [str(binary), str(boolean_search_file), "--tui"],
                stdin=boolean_slave,
                stdout=boolean_slave,
                stderr=boolean_slave,
                close_fds=True,
            )
            boolean_output = bytearray()
            try:
                def boolean_read_until(needle, timeout):
                    deadline = time.monotonic() + timeout
                    while needle not in boolean_output and \
                            boolean_process.poll() is None and time.monotonic() < deadline:
                        ready, _, _ = select.select([boolean_master], [], [], 0.1)
                        if ready:
                            try:
                                boolean_output.extend(os.read(boolean_master, 65536))
                            except OSError:
                                break

                boolean_read_until(b"Scan complete", 3)
                os.write(boolean_master, b"/")
                boolean_read_until(b"\033[24;1H\033[2K/", 2)
                os.write(boolean_master, b'"66.249" AND "20/May"\n')
                boolean_read_until(b'Search: "66.249" AND "20/May"', 3)
                os.write(boolean_master, b"\n")
                boolean_read_until(
                    b"\033[7m       4 20/May/2015:20 request from 66.249", 3)
                os.write(boolean_master, b"q")
                boolean_process.wait(timeout=3)
                require(boolean_process.returncode == 0 and
                        b'Search: "66.249" AND "20/May"' in boolean_output and
                        b'1 66.249 client request 20/May/2015:20' in boolean_output and
                        b"\033[7m       4 20/May/2015:20 request from 66.249" in boolean_output,
                        "TUI Boolean AND search should find both terms in either order and navigate between matches")
            finally:
                if boolean_process.poll() is None:
                    boolean_process.kill()
                    boolean_process.wait(timeout=5)
                os.close(boolean_master)
                os.close(boolean_slave)

            (root / "tui_1.jsonl").write_text("existing export 1\n", encoding="utf-8")
            (root / "tui_2.jsonl").write_text("existing export 2\n", encoding="utf-8")
            tui_export = root / "tui_3.jsonl"
            export_master, export_slave = pty.openpty()
            fcntl.ioctl(export_slave, termios.TIOCSWINSZ,
                        struct.pack("HHHH", 24, 100, 0, 0))
            export_process = subprocess.Popen(
                [str(binary), str(tui_file), "--format", "jsonl", "--tui"],
                stdin=export_slave,
                stdout=export_slave,
                stderr=export_slave,
                close_fds=True,
                cwd=root,
            )
            export_output = bytearray()
            try:
                def export_read_until(needle, timeout):
                    deadline = time.monotonic() + timeout
                    while needle not in export_output and export_process.poll() is None and time.monotonic() < deadline:
                        ready, _, _ = select.select([export_master], [], [], 0.1)
                        if ready:
                            try:
                                export_output.extend(os.read(export_master, 65536))
                            except OSError:
                                break

                export_read_until(b"Scan complete", 3)
                export_output.clear()
                os.write(export_master, b"f")
                export_read_until(b"Filter: ", 2)
                os.write(export_master, b"level=ERROR\n")
                export_output.clear()
                export_read_until(b"Scan complete", 3)
                export_output.clear()
                os.write(export_master, b"/")
                export_read_until(b"\033[24;1H\033[2K/", 2)
                os.write(export_master, b"timeout\n")
                export_output.clear()
                export_read_until(b"Search: timeout", 2)
                export_output.clear()
                os.write(export_master, b"x")
                export_read_until(b"Export path (Enter for numbered default): ", 2)
                os.write(export_master, b"\n")
                export_read_until(b"Exported 1 record", 3)
                os.write(export_master, b"q")
                export_process.wait(timeout=5)
                require(export_process.returncode == 0 and tui_export.read_bytes() ==
                        b'{"level":"ERROR","message":"connection timeout"}\n',
                        "blank TUI export path should create the next numbered file with filtered/search matches")
            finally:
                if export_process.poll() is None:
                    export_process.kill()
                    export_process.wait(timeout=5)
                os.close(export_master)
                os.close(export_slave)

            long_tui_file = root / "tui-long-line.jsonl"
            long_tui_file.write_bytes(b'{"level":"INFO","message":"' +
                                      b'x' * 1800 + b'"}\n')
            long_master, long_slave = pty.openpty()
            fcntl.ioctl(long_slave, termios.TIOCSWINSZ,
                        struct.pack("HHHH", 24, 100, 0, 0))
            long_process = subprocess.Popen(
                [str(binary), str(long_tui_file), "--format", "jsonl", "--tui"],
                stdin=long_slave,
                stdout=long_slave,
                stderr=long_slave,
                close_fds=True,
            )
            os.close(long_slave)
            long_output = bytearray()
            try:
                preview_line = b"[preview clipped]\x1b[0m\r\n"
                deadline = time.monotonic() + 8
                while (preview_line not in long_output and
                       long_process.poll() is None and time.monotonic() < deadline):
                    ready, _, _ = select.select([long_master], [], [], 0.1)
                    if ready:
                        try:
                            long_output.extend(os.read(long_master, 65536))
                        except OSError:
                            break
                os.write(long_master, b"q")
                long_process.wait(timeout=5)
                require(long_process.returncode == 0 and
                        b"[preview clipped]" in long_output and
                        preview_line in long_output,
                        "TUI should display bounded previews for long records "
                        "(returncode={}, output tail={!r})".format(
                            long_process.returncode,
                            bytes(long_output[-300:])))
            finally:
                if long_process.poll() is None:
                    long_process.kill()
                    long_process.wait(timeout=5)
                os.close(long_master)

            follow_tui_file = root / "tui-follow.jsonl"
            follow_tui_file.write_bytes(b'{"level":"INFO","message":"initial"}\n')
            follow_master, follow_slave = pty.openpty()
            fcntl.ioctl(follow_slave, termios.TIOCSWINSZ,
                        struct.pack("HHHH", 24, 100, 0, 0))
            follow_process = subprocess.Popen(
                [str(binary), str(follow_tui_file), "--format", "jsonl",
                 "--follow", "--tui"],
                stdin=follow_slave,
                stdout=follow_slave,
                stderr=follow_slave,
                close_fds=True,
            )
            os.close(follow_slave)
            follow_output = bytearray()
            try:
                def read_follow_until(needle, timeout):
                    deadline = time.monotonic() + timeout
                    while needle not in follow_output and follow_process.poll() is None and time.monotonic() < deadline:
                        ready, _, _ = select.select([follow_master], [], [], 0.1)
                        if ready:
                            try:
                                follow_output.extend(os.read(follow_master, 65536))
                            except OSError:
                                break

                read_follow_until(b"Following file; press q to stop", 3)
                with follow_tui_file.open("ab") as stream:
                    stream.write(b'{"level":"INFO","message":"live')
                    stream.flush()
                time.sleep(0.4)
                require(b'"message":"live' not in follow_output,
                        "TUI follow should wait for a complete appended record")
                with follow_tui_file.open("ab") as stream:
                    stream.write(b' event"}\n')
                    stream.flush()
                read_follow_until(b'2 {"level":"INFO","message":"live event"}', 3)
                rotated_file = root / "tui-follow.jsonl.1"
                follow_tui_file.replace(rotated_file)
                follow_tui_file.write_bytes(b'{"level":"ERROR","message":"after rotation"}\n')
                read_follow_until(b'1 {"level":"ERROR","message":"after rotation"}', 3)
                os.write(follow_master, b"q")
                follow_process.wait(timeout=5)
                require(follow_process.returncode == 0 and
                        b'2 {"level":"INFO","message":"live event"}' in follow_output and
                        b'1 {"level":"ERROR","message":"after rotation"}' in follow_output,
                        "TUI follow should flush complete appends and rescan rotated files")
            finally:
                if follow_process.poll() is None:
                    follow_process.kill()
                    follow_process.wait(timeout=5)
                os.close(follow_master)

        text_export = root / "selected.log"
        exported = run(binary, str(json_file), "--format", "jsonl", "--filter",
                       "level=ERROR", "--export", "text", "--output", str(text_export))
        require(exported.returncode == 0 and text_export.read_bytes() ==
                b'{"level":"ERROR","message":"connection timeout"}\n'
                b'{"severity":"ERROR","msg":"timeout"}\n'
                b'{"level":"ERROR","message":"other"}\n'
                b'{"level":"ERROR","message":"nested","details":{"code":3}}\n',
                "text export should preserve selected source records")

        csv_export = root / "selected.csv"
        exported = run(binary, str(json_file), "--format", "jsonl", "--filter",
                       "level=ERROR", "--export", "csv", "--output", str(csv_export),
                       "--columns", "line_number,level,message,raw")
        require(exported.returncode == 0, "CSV export should complete")
        with csv_export.open("r", encoding="utf-8", newline="") as stream:
            csv_rows = list(csv.reader(stream))
        require(csv_rows[0] == ["line_number", "level", "message", "raw"] and
                csv_rows[1] == ["2", "ERROR", "connection timeout",
                                '{"level":"ERROR","message":"connection timeout"}'] and
                csv_rows[2] == ["3", "ERROR", "timeout",
                                '{"severity":"ERROR","msg":"timeout"}'] and
                csv_rows[3] == ["4", "ERROR", "other",
                                '{"level":"ERROR","message":"other"}'] and
                csv_rows[4] == ["7", "ERROR", "nested",
                                '{"level":"ERROR","message":"nested","details":{"code":3}}'],
                "CSV export should add headers and serialize configured fields")
        protected = run(binary, str(json_file), "--format", "jsonl", "--export", "text",
                        "--output", str(text_export))
        require(protected.returncode == 2 and
                text_export.read_bytes().startswith(b'{"level":"ERROR"'),
                "export should refuse to overwrite an existing output without --force")
        forced = run(binary, str(json_file), "--format", "jsonl", "--export", "text",
                     "--output", str(text_export), "--force")
        require(forced.returncode == 0 and text_export.read_bytes().count(b"\n") == 8,
                "--force should replace a different existing destination")
        same_file = run(binary, str(raw_file), "--export", "text", "--output",
                        str(raw_file), "--force")
        require(same_file.returncode == 2 and raw_file.read_bytes().startswith(b"first timeout"),
                "export must refuse to overwrite the input file even with --force")
        missing_directory = root / "missing-output-directory" / "result.csv"
        failed_export = run(binary, str(raw_file), "--export", "csv", "--output",
                            str(missing_directory))
        require(failed_export.returncode == 2,
                "an output path whose parent does not exist should report an I/O error")

        invalid_utf8 = root / "invalid-utf8.log"
        invalid_utf8.write_bytes(b'bad\xff,"quoted"\n')
        invalid_utf8_csv = root / "invalid-utf8.csv"
        invalid_export = run(binary, str(invalid_utf8), "--export", "csv",
                             "--output", str(invalid_utf8_csv), "--columns", "raw")
        require(invalid_export.returncode == 0, "CSV should safely encode invalid UTF-8")
        with invalid_utf8_csv.open("r", encoding="utf-8", newline="") as stream:
            invalid_rows = list(csv.reader(stream))
        require(invalid_rows == [["raw"], ['bad\ufffd,"quoted"']],
                "CSV should replace invalid UTF-8 and escape quotes")

        multiline_json = root / "multiline.jsonl"
        multiline_json.write_bytes(b'{"message":"first\\nsecond"}\n')
        multiline_csv = root / "multiline.csv"
        multiline_export = run(binary, str(multiline_json), "--format", "jsonl",
                               "--export", "csv", "--output", str(multiline_csv),
                               "--columns", "message")
        require(multiline_export.returncode == 0, "CSV should export multiline field data")
        with multiline_csv.open("r", encoding="utf-8", newline="") as stream:
            multiline_rows = list(csv.reader(stream))
        require(multiline_rows == [["message"], ["first\nsecond"]],
                "CSV should quote decoded line breaks inside fields")
        json_diagnostics = run(binary, str(json_file), "--format", "jsonl")
        require(b"skipped 3 invalid" in json_diagnostics.stderr,
                "malformed, duplicate-key, and invalid Unicode JSON should be counted")

        unicode_json = root / "unicode.jsonl"
        escaped_unicode_record = (
            b'{"message":"caf\\u00e9 snowman \\u2603 face \\ud83d\\ude00"}\n'
        )
        literal_unicode_record = (
            '{"message":"café snowman ☃ face 😀"}\n'.encode("utf-8")
        )
        unicode_json.write_bytes(escaped_unicode_record + literal_unicode_record)
        unicode_scan = run(binary, str(unicode_json), "--format", "jsonl",
                           "--regex-field", "message=^caf")
        require(unicode_scan.returncode == 0 and unicode_scan.stdout ==
                escaped_unicode_record + literal_unicode_record,
                "JSON should decode valid Unicode escapes and preserve UTF-8 source records")
        unicode_csv = root / "unicode.csv"
        unicode_export = run(binary, str(unicode_json), "--format", "jsonl",
                             "--export", "csv", "--output", str(unicode_csv),
                             "--columns", "message")
        require(unicode_export.returncode == 0,
                "CSV export should accept decoded BMP and supplementary Unicode characters")
        with unicode_csv.open("r", encoding="utf-8", newline="") as stream:
            unicode_rows = list(csv.reader(stream))
        require(unicode_rows == [
                    ["message"],
                    ["café snowman ☃ face 😀"],
                    ["café snowman ☃ face 😀"],
                ],
                "escaped surrogate pairs and equivalent UTF-8 text should decode identically")

        invalid_unicode_json = root / "invalid-unicode.jsonl"
        invalid_unicode_json.write_bytes(
            b'{"level":"INFO","message":"valid"}\n'
            b'{"message":"overlong \xc0\xaf"}\n'
            b'{"message":"stray continuation \x80"}\n'
            b'{"message":"truncated \xe2\x82"}\n'
            b'{"message":"encoded surrogate \xed\xa0\x80"}\n'
            b'{"message":"above Unicode maximum \xf4\x90\x80\x80"}\n'
        )
        invalid_unicode_scan = run(binary, str(invalid_unicode_json), "--format", "jsonl",
                                   "--filter", "level=INFO")
        require(invalid_unicode_scan.returncode == 0 and
                invalid_unicode_scan.stdout == b'{"level":"INFO","message":"valid"}\n' and
                b"skipped 5 invalid" in invalid_unicode_scan.stderr,
                "JSON should reject malformed UTF-8 classes while keeping valid records "
                "(returncode={}, stdout={!r}, stderr={!r})".format(
                    invalid_unicode_scan.returncode,
                    invalid_unicode_scan.stdout,
                    invalid_unicode_scan.stderr))

        nested_json = root / "employees-formatted.json"
        nested_records = [
            b'{\n'
            b'  "employee": {\n'
            b'    "id": "E1",\n'
            b'    "display": "literal { brace, } brace, and \\"quoted\\" text",\n'
            b'    "department": {"manager": {"contact": {"phone": "829-648-0859x1216"}}}\n'
            b'  },\n'
            b'  "projects": [{"projectId": "P1", "details": {"active": true}}],\n'
            + b''.join(
                ('  "history_%02d": "history item %02d",\n' %
                 (number, number)).encode("ascii")
                for number in range(35))
            + b'  "zoomTail": "zoom-end-marker"\n'
            + b'}',
            b'{\n'
            b'  "employee": {\n'
            b'    "id": "E2",\n'
            b'    "department": {"manager": {"contact": {"phone": "716.512.1322x0504"}}}\n'
            b'  }\n'
            b'}',
        ]
        nested_json.write_bytes(b'[\n' + b',\n'.join(nested_records) + b'\n]\n')
        nested_match = run(binary, str(nested_json), "--format", "jsonl",
                           "--regex-field", "phone=^829")
        require(nested_match.returncode == 0 and
                nested_match.stdout == nested_records[0] + b"\n",
                "multiline JSON arrays should emit objects and search nested leaf fields")
        nested_path_match = run(binary, str(nested_json), "--format", "jsonl",
                                "--filter", "employee.department.manager.contact.phone~^829")
        require(nested_path_match.returncode == 0 and
                nested_path_match.stdout == nested_records[0] + b"\n",
                "nested JSON field paths should be available to compound filters")
        nested_auto = run(binary, str(nested_json), "--format", "auto",
                           "--regex-field", "employee.department.manager.contact.phone=^829")
        require(nested_auto.returncode == 0 and
                nested_auto.stdout == nested_records[0] + b"\n",
                "format detection should recognize a formatted JSON array")
        nested_csv = root / "nested-fields.csv"
        nested_export = run(binary, str(nested_json), "--format", "jsonl",
                            "--regex-field", "phone=^829", "--export", "csv",
                            "--output", str(nested_csv), "--columns",
                            "line_number,employee.department.manager.contact.phone")
        require(nested_export.returncode == 0,
                "CSV export should accept nested JSON field paths")
        with nested_csv.open("r", encoding="utf-8", newline="") as stream:
            nested_csv_rows = list(csv.reader(stream))
        require(nested_csv_rows == [
            ["line_number", "employee.department.manager.contact.phone"],
            ["2", "829-648-0859x1216"],
        ], "nested JSON CSV columns should preserve values and source line numbers")
        if os.name != "nt":
            histogram_file = root / "histogram.jsonl"
            histogram_file.write_bytes(
                b'{"timestamp":"2025-01-01T00:00:00Z","level":"ERROR","message":"first"}\n'
                b'{"timestamp":"2025-01-01T00:02:00Z","level":"INFO","message":"last"}\n'
                b'{"timestamp":"2025-01-01T00:01:00Z","level":"WARN","message":"middle"}\n'
                b'{"timestamp":"unsupported","level":"INFO","message":"unknown"}\n'
            )
            histogram_master, histogram_slave = pty.openpty()
            fcntl.ioctl(histogram_slave, termios.TIOCSWINSZ,
                        struct.pack("HHHH", 20, 100, 0, 0))
            histogram_tui = subprocess.Popen(
                [str(binary), str(histogram_file), "--format", "jsonl", "--tui"],
                stdin=histogram_slave,
                stdout=histogram_slave,
                stderr=histogram_slave,
                close_fds=True,
            )
            histogram_output = bytearray()
            try:
                def histogram_read_until(needle, timeout, start=0):
                    deadline = time.monotonic() + timeout
                    while needle not in histogram_output[start:] and \
                            histogram_tui.poll() is None and time.monotonic() < deadline:
                        ready, _, _ = select.select([histogram_master], [], [], 0.1)
                        if ready:
                            try:
                                histogram_output.extend(os.read(histogram_master, 65536))
                            except OSError:
                                break

                histogram_read_until(b"Scan complete", 3)
                os.write(histogram_master, b"h")
                histogram_read_until(b"Time histogram", 3)
                histogram_read_until(b"Bucket 000 | 1 records", 3)
                histogram_read_until(
                    b"Left/Right: bucket  Enter: jump  Esc or h: records", 3)
                histogram_screen = bytes(histogram_output[
                    histogram_output.rfind(b"\x1b[H\x1b[2J"):])
                require(b"UTC time: oldest" in histogram_screen and
                        b"Relative volume" in histogram_screen and
                        b"tallest column =" in histogram_screen and
                        b"Legend: # normal  + warning  ! error" in histogram_screen and
                        histogram_screen.count(
                            b"Left/Right: bucket  Enter: jump  Esc or h: records") == 1,
                        "TUI histogram should explain its axes and show navigation instructions once")
                right_output_start = len(histogram_output)
                os.write(histogram_master, b"\x1b[C")
                histogram_read_until(b"Bucket 126 | 1 records", 3,
                                     right_output_start)
                jump_output_start = len(histogram_output)
                os.write(histogram_master, b"\r")
                histogram_read_until(b"Jumped to first record in bucket 126", 3,
                                     jump_output_start)
                os.write(histogram_master, b"q")
                histogram_tui.wait(timeout=5)
                require(histogram_tui.returncode == 0 and
                        b"3 timestamped | 1 unknown" in histogram_output and
                        b'3 {"timestamp":"2025-01-01T00:01:00Z"' in histogram_output,
                        "TUI histogram should navigate to the first selected record in a bucket; "
                        "output={!r}".format(bytes(histogram_output[-2000:])))
            finally:
                if histogram_tui.poll() is None:
                    histogram_tui.kill()
                    histogram_tui.wait(timeout=5)
                os.close(histogram_master)
                os.close(histogram_slave)

            syslog_histogram_master, syslog_histogram_slave = pty.openpty()
            fcntl.ioctl(syslog_histogram_slave, termios.TIOCSWINSZ,
                        struct.pack("HHHH", 24, 100, 0, 0))
            syslog_histogram_tui = subprocess.Popen(
                [str(binary), str(syslog_timeline_file), "--format", "syslog3164",
                 "--syslog-year", "2025", "--syslog-timezone", "+02:00", "--tui"],
                stdin=syslog_histogram_slave,
                stdout=syslog_histogram_slave,
                stderr=syslog_histogram_slave,
                close_fds=True,
            )
            syslog_histogram_output = bytearray()
            try:
                def syslog_histogram_read_until(needle, timeout, start=0):
                    deadline = time.monotonic() + timeout
                    while needle not in syslog_histogram_output[start:] and \
                            syslog_histogram_tui.poll() is None and time.monotonic() < deadline:
                        ready, _, _ = select.select([syslog_histogram_master], [], [], 0.1)
                        if ready:
                            try:
                                syslog_histogram_output.extend(
                                    os.read(syslog_histogram_master, 65536))
                            except OSError:
                                break

                syslog_histogram_read_until(b"Scan complete", 3)
                os.write(syslog_histogram_master, b"h")
                syslog_histogram_read_until(b"Bucket 000 | 1 records", 3)
                jump_start = len(syslog_histogram_output)
                os.write(syslog_histogram_master, b"\r")
                syslog_histogram_read_until(
                    b"Jumped to first record in bucket 000", 2, jump_start)
                selected_syslog_row = b"\x1b[7m       1 Jun 4 15:16:01 host app: first"
                syslog_histogram_read_until(selected_syslog_row, 2, jump_start)
                os.write(syslog_histogram_master, b"q")
                syslog_histogram_tui.wait(timeout=4)
                require(syslog_histogram_tui.returncode == 0 and
                        b"3 timestamped | 0 unknown" in syslog_histogram_output and
                        selected_syslog_row in syslog_histogram_output[jump_start:] and
                        b"Jumped to first record in bucket 000" in
                        syslog_histogram_output[jump_start:],
                        "configured RFC 3164 timestamps should populate and navigate the TUI histogram; "
                        "returncode={!r}, output={!r}".format(
                            syslog_histogram_tui.returncode,
                            bytes(syslog_histogram_output[-2000:])))
            finally:
                if syslog_histogram_tui.poll() is None:
                    syslog_histogram_tui.kill()
                    syslog_histogram_tui.wait(timeout=5)
                os.close(syslog_histogram_master)
                os.close(syslog_histogram_slave)

            nested_master, nested_slave = pty.openpty()
            fcntl.ioctl(nested_slave, termios.TIOCSWINSZ,
                        struct.pack("HHHH", 24, 120, 0, 0))
            nested_tui = subprocess.Popen(
                [str(binary), str(nested_json), "--format", "jsonl",
                 "--regex-field", "phone=^829", "--tui"],
                stdin=nested_slave,
                stdout=nested_slave,
                stderr=nested_slave,
                close_fds=True,
            )
            nested_tui_output = bytearray()
            try:
                def nested_read_until(needle, timeout, start=0):
                    deadline = time.monotonic() + timeout
                    while needle not in nested_tui_output[start:] and \
                            nested_tui.poll() is None and time.monotonic() < deadline:
                        ready, _, _ = select.select([nested_master], [], [], 0.1)
                        if ready:
                            try:
                                nested_tui_output.extend(os.read(nested_master, 65536))
                            except OSError:
                                break

                deadline = time.monotonic() + 5
                while b"Scan complete" not in nested_tui_output and \
                        nested_tui.poll() is None and time.monotonic() < deadline:
                    ready, _, _ = select.select([nested_master], [], [], 0.1)
                    if ready:
                        try:
                            nested_tui_output.extend(os.read(nested_master, 65536))
                        except OSError:
                            break
                os.write(nested_master, b"z")
                nested_read_until(b"Record zoom | source line 2", 2)
                idle_output_start = len(nested_tui_output)
                idle_deadline = time.monotonic() + 0.4
                while time.monotonic() < idle_deadline and nested_tui.poll() is None:
                    ready, _, _ = select.select([nested_master], [], [], 0.05)
                    if ready:
                        try:
                            nested_tui_output.extend(os.read(nested_master, 65536))
                        except OSError:
                            break
                require(b"\x1b[2J" not in nested_tui_output[idle_output_start:],
                        "zoom view should not redraw while idle")
                os.write(nested_master, b"\x1b[F")
                nested_read_until(b"zoom-end-marker", 2)
                os.write(nested_master, b"\x1b[H")
                nested_read_until(b"829-648-0859x1216", 2)
                restore_output_start = len(nested_tui_output)
                os.write(nested_master, b"\x1b")
                nested_read_until(b"Search: (none)", 2, restore_output_start)
                os.write(nested_master, b"q")
                nested_tui.wait(timeout=5)
                nested_screen = bytes(nested_tui_output).replace(b"\r\n", b"\n")
                require(nested_tui.returncode == 0 and
                        b"selected: 1" in nested_screen and
                        b"829-648-0859x1216" in nested_screen and
                        b'"employee": {\n' in nested_screen and
                        b'"id": "E1"' in nested_screen and
                        b'"display": "literal { brace, } brace, and \\"quoted\\" text"' in nested_screen and
                        b"formatted JSON" in nested_screen and
                        b"Record zoom | source line 2" in nested_screen and
                        b"zoom-end-marker" in nested_screen,
                        "TUI should display the multiline JSON record matching a nested phone field; "
                        "returncode={!r}, output={!r}".format(
                            nested_tui.returncode, nested_screen[-4000:]))
            finally:
                if nested_tui.poll() is None:
                    nested_tui.kill()
                    nested_tui.wait(timeout=5)
                os.close(nested_master)
                os.close(nested_slave)

        access_file = root / "access.log"
        access_line = (
            b'203.0.113.8 - alice [10/Oct/2000:13:55:36 -0700] '
            b'"GET /index.html HTTP/1.1" 200 2326 "https://example.test/" '
            b'"Mozilla/5.0 test agent"\n'
        )
        access_file.write_bytes(access_line)
        result = run(binary, str(access_file), "--format", "apache-combined",
                     "--filter", "status=200", "--regex-field",
                     "client_ip=203\\.0\\.113\\.8")
        require(result.returncode == 0 and result.stdout == access_line,
                "Apache Combined parsing should expose status and client IP")
        result = run(binary, str(access_file), "--format", "auto",
                     "--filter", 'request="GET /index.html HTTP/1.1"')
        require(result.returncode == 0 and result.stdout == access_line,
                "auto detection should identify a consistent access log")
        escaped_access_line = (
            b'203.0.113.8 - alice [10/Oct/2000:13:55:36 -0700] '
            b'"GET /quoted HTTP/1.1" 200 12 "-" '
            b'"Mozilla/5.0 \\"quoted\\" agent"\n'
        )
        access_file.write_bytes(escaped_access_line)
        escaped_access = run(binary, str(access_file), "--format", "apache-combined",
                             "--regex-field", "user_agent=quoted")
        require(escaped_access.returncode == 0 and
                escaped_access.stdout == escaped_access_line,
                "Apache Combined parsing should accept escaped quotes in quoted fields")
        common_line = (
            b'203.0.113.8 - alice [10/Oct/2000:13:55:36 -0700] '
            b'"GET / HTTP/1.1" 204 0\n'
        )
        access_file.write_bytes(common_line)
        common = run(binary, str(access_file), "--format", "apache-common",
                      "--filter", "status=204")
        require(common.returncode == 0 and common.stdout == common_line,
                "Apache Common parsing should accept the Common field layout")

        error_file = root / "apache-error.log"
        error_line = (
            b"[Mon Feb 27 05:41:08 2006] [error] [client 212.34.140.103] "
            b"File does not exist: /var/www/html/missing\n"
        )
        modern_error = (
            b"[Wed Oct 11 14:32:52.123456 2023] [core:error] [pid 123] "
            b"[client 192.0.2.5] Modern error message\n"
        )
        error_file.write_bytes(error_line + modern_error)
        result = run(binary, str(error_file), "--format", "apache-error",
                     "--filter", "level=error and source=212.34.140.103")
        require(result.returncode == 0 and result.stdout == error_line,
                "Apache Error parsing should expose timestamp, level, and client source")
        result = run(binary, str(error_file), "--format", "auto",
                     "--filter", "module=core and source=192.0.2.5")
        require(result.returncode == 0 and result.stdout == modern_error,
                "auto detection should recognize Apache Error and its modern module field")
        result = run(binary, str(error_file), "--format", "apache-error", "--stats")
        require(result.returncode == 0 and
                b"Timestamped records: 2" in result.stdout and
                b"Unknown timestamps: 0" in result.stdout,
                "Apache Error timestamps should populate time statistics")

        syslog_file = root / "syslog.log"
        syslog3164 = b"Oct 11 22:14:15 mymachine su[1234]: 'su root' failed\n"
        syslog_file.write_bytes(syslog3164)
        result = run(binary, str(syslog_file), "--format", "syslog3164",
                     "--filter", "pid=1234")
        require(result.returncode == 0 and result.stdout == syslog3164,
                "RFC 3164 parsing should extract the process ID")
        result = run(binary, str(syslog_file), "--format", "syslog3164",
                     "--filter", 'timestamp="Oct 11 22:14:15" and host=mymachine')
        require(result.returncode == 0 and result.stdout == syslog3164,
                "RFC 3164 should preserve its timestamp without inventing a year")
        result = run(binary, str(syslog_file), "--format", "auto",
                     "--filter", "process=su")
        require(result.returncode == 0 and result.stdout == syslog3164,
                "auto detection should recognize RFC 3164 records")
        legacy_tag = b"Jun 19 04:09:11 combo syslogd 1.4.1: restart.\n"
        syslog_file.write_bytes(legacy_tag)
        legacy_result = run(binary, str(syslog_file), "--format", "syslog3164",
                            "--filter", 'process="syslogd 1.4.1"')
        require(legacy_result.returncode == 0 and legacy_result.stdout == legacy_tag,
                "RFC 3164 tags containing spaces should remain parseable")

        syslog5424 = b"<34>1 2003-10-11T22:14:15.003Z mymachine su 123 ID47 [exampleSDID@32473 iut=\"3\" detail=\"left\\]right\"] BOMAn application event log entry\n"
        syslog_file.write_bytes(syslog5424)
        result = run(binary, str(syslog_file), "--format", "syslog5424",
                     "--filter", 'level=CRIT and structured_data~"detail"')
        require(result.returncode == 0 and result.stdout == syslog5424,
                "RFC 5424 should extract severity and retain structured data")
        malformed_syslog = (
            b'<34>1 2003-10-11T22:14:15.003Z mymachine su 123 ID47 '
            b'[exampleSDID@32473 iut="unterminated] event\n'
        )
        syslog_file.write_bytes(malformed_syslog)
        malformed_syslog_result = run(binary, str(syslog_file), "--format", "syslog5424",
                                      "--filter", "process=su")
        require(malformed_syslog_result.returncode == 1 and
                b"skipped 1 invalid or oversized record(s)" in malformed_syslog_result.stderr,
                "malformed RFC 5424 structured data should be rejected and counted")

        custom_line = b"2025-01-02 ERROR disk timeout\n"
        custom_file = root / "custom.log"
        custom_file.write_bytes(custom_line)
        result = run(binary, str(custom_file), "--format", "custom",
                     "--pattern", "%{TIMESTAMP:ts} %{LEVEL:lvl} %{MESSAGE:msg}",
                     "--filter", "level=ERROR")
        require(result.returncode == 0 and result.stdout == custom_line,
                "custom patterns should support named canonical placeholders")
        invalid_custom = run(binary, str(custom_file), "--format", "custom",
                             "--pattern", "%{LEVEL}%{MESSAGE}")
        require(invalid_custom.returncode == 3,
                "ambiguous adjacent custom placeholders should be rejected")
        duplicate_custom = run(
            binary, str(custom_file), "--format", "custom",
            "--pattern", "%{TIMESTAMP:x} %{LEVEL:x} %{MESSAGE}")
        require(duplicate_custom.returncode == 3,
                "custom patterns should reject duplicate field aliases")

        malformed_file = root / "malformed.jsonl"
        malformed_file.write_bytes(b'{"message":"raw fallback match",}\n')
        raw_fallback = run(
            binary,
            str(malformed_file),
            "--format",
            "jsonl",
            "--regex",
            "raw fallback",
        )
        require(raw_fallback.returncode == 0 and
                raw_fallback.stdout == b'{"message":"raw fallback match",}\n',
                "raw regex should still search malformed JSON records")

        missing = run(
            binary,
            str(json_file),
            "--format",
            "jsonl",
            "--filter",
            "level=WARN",
        )
        require(missing.returncode == 1, "no matches should return status 1")
        invalid = run(binary, str(raw_file), "--regex", "(")
        require(invalid.returncode == 3, "invalid regex should return status 3")
        require(b"Unmatched opening parenthesis" in invalid.stderr,
                "invalid regex should report a useful diagnostic")

        oversized_file = root / "oversized.log"
        oversized_file.write_bytes(b"ok\n1234567\nend")
        oversized = run(
            binary,
            str(oversized_file),
            "--max-line-bytes",
            "4",
        )
        require(oversized.returncode == 0,
                "valid lines around an oversized line remain usable")
        require(oversized.stdout == b"ok\nend\n",
                "oversized lines should be skipped without losing following lines")

        regex_cases = [
            ("literal", b"contains literal text"),
            (".", b"x"),
            ("a*", b"bbb"),
            ("a+", b"baaa"),
            ("colou?r", b"color"),
            ("[a-z]+", b"abc123"),
            ("[^0-9]+", b"123abc"),
            (r"\d+", b"id=123"),
            (r"\w+", b"word"),
            (r"\s+", b"white space"),
            ("^start$", b"start"),
            ("(cat|dog)", b"a dog"),
        ]
        for pattern, contents in regex_cases:
            sample_file = root / "regex.log"
            sample_file.write_bytes(contents + b"\n")
            match = run(binary, str(sample_file), "--regex", pattern)
            require(match.returncode == 0,
                    "regex construct did not match: " + pattern)

        long_pattern = "^" + ("x" * 512) + "$"
        long_line = root / "long-pattern.log"
        long_line.write_bytes(("x" * 512 + "\n").encode("ascii"))
        long_result = run(binary, str(long_line), "--regex", long_pattern)
        require(long_result.returncode == 0,
                "long concatenated regex should compile and match")
        absent = run(binary, str(long_line), "--regex", "^y+$")
        require(absent.returncode == 1,
                "a nonmatching anchored regex should not select the line")

        adversarial = root / "adversarial.log"
        adversarial.write_bytes(b"a" * 20000 + b"\n")
        started = time.perf_counter()
        result = run(binary, str(adversarial), "--regex", "(a+)+b")
        elapsed = time.perf_counter() - started
        require(result.returncode == 1,
                "nested quantifiers should finish with no match")
        require(elapsed < 5.0,
                "NFA matching should avoid catastrophic backtracking")

        sample = Path(__file__).resolve().parents[1] / "SAMPLES" / "enwik8"
        if sample.exists():
            longest_prefix = b""
            longest_size = 0
            with sample.open("rb") as sample_stream:
                for sample_line in sample_stream:
                    if (len(sample_line) > longest_size and
                            all(byte < 0x80 for byte in sample_line[:48]) and
                            b"\x00" not in sample_line[:48]):
                        longest_size = len(sample_line)
                        longest_prefix = sample_line[:48].rstrip(b"\r\n")
            require(longest_size > 4000,
                    "enwik8 should contain a line longer than 4 KB")
            regex_specials = b"\\.^$*+?()[]|"
            escaped_prefix = b"".join(
                (b"\\" + bytes((byte,))) if byte in regex_specials else bytes((byte,))
                for byte in longest_prefix
            )
            started = time.perf_counter()
            long_line_result = run(
                binary,
                str(sample),
                "--regex",
                "^" + escaped_prefix.decode("ascii"),
            )
            long_line_elapsed = time.perf_counter() - started
            require(long_line_result.returncode == 0,
                    "enwik8's longest line should match its literal prefix")
            require(long_line_result.stdout.startswith(longest_prefix) and
                    len(long_line_result.stdout) > 4000,
                    "the reader should preserve and emit enwik8's long lines")
            require(long_line_elapsed < 30.0,
                    "the enwik8 long-line scan exceeded 30 seconds")

            started = time.perf_counter()
            result = run(binary, str(sample), "--filter", "missing=value")
            elapsed = time.perf_counter() - started
            require(result.returncode == 1,
                    "the full enwik8 scan should produce no matches")
            require(elapsed < 30.0,
                    "the 100 MB enwik8 streaming smoke scan exceeded 30 seconds")
            print("enwik8: scanned 100 MB in {:.2f}s".format(elapsed))
            tested_samples.add(sample.resolve())
        else:
            print("enwik8: skipped because SAMPLES/enwik8 is not present")

        large_syslog = Path(__file__).resolve().parents[1] / "SAMPLES" / "Linux_1GB.syslog"
        if large_syslog.exists():
            started = time.perf_counter()
            large_result = run(
                binary,
                str(large_syslog),
                "--format",
                "syslog3164",
                "--filter",
                "pid=__glogviewer_no_such_pid__",
            )
            elapsed = time.perf_counter() - started
            require(large_result.returncode == 1,
                    "the large syslog fixture should have no matching sentinel PID")
            require(large_result.stdout == b"" and large_result.stderr == b"",
                    "the entire large syslog fixture should parse without rejected records")
            require(elapsed < 60.0,
                    "the 1 GB syslog streaming regression exceeded 60 seconds")
            print("Linux_1GB.syslog: parsed {:.2f} GB in {:.2f}s".format(
                large_syslog.stat().st_size / 1_000_000_000, elapsed))
            tested_samples.add(large_syslog.resolve())
        else:
            print("Linux_1GB.syslog: skipped because the local sample is not present")

        http_sample = Path(__file__).resolve().parents[1] / "SAMPLES" / "http_access_500MB.log"
        if http_sample.exists():
            started = time.perf_counter()
            http_result = run(
                binary,
                str(http_sample),
                "--format",
                "apache-combined",
                "--filter",
                "status=__glogviewer_no_such_status__",
            )
            elapsed = time.perf_counter() - started
            require(http_result.returncode == 1,
                    "the large HTTP access fixture should have no sentinel status")
            require(http_result.stdout == b"" and
                    b"skipped 222 invalid or oversized record(s)" in http_result.stderr,
                    "complete access records should parse and known truncated records should be counted")
            require(elapsed < 60.0,
                    "the 500 MB HTTP access streaming regression exceeded 60 seconds")
            print("http_access_500MB.log: scanned {:.2f} GB in {:.2f}s".format(
                http_sample.stat().st_size / 1_000_000_000, elapsed))
            tested_samples.add(http_sample.resolve())
        else:
            print("http_access_500MB.log: skipped because the local sample is not present")

        if sample_directory.is_dir():
            sample_files = sorted(path for path in sample_directory.rglob("*")
                                  if path.is_file())
            for sample_path in sample_files:
                if sample_path.resolve() in tested_samples:
                    continue
                print("Scanning additional sample: {} ({} bytes)".format(
                    sample_path.relative_to(sample_directory),
                    sample_path.stat().st_size), flush=True)
                started = time.perf_counter()
                sample_result = run(binary, str(sample_path), "--format", "raw",
                                    "--filter", "missing=value")
                elapsed = time.perf_counter() - started
                require(sample_result.returncode == 1,
                        "sample should complete a full raw scan with no matches: {}".format(
                            sample_path))
                require(sample_result.stdout == b"",
                        "unmatched sample scan should not write records: {}".format(
                            sample_path))
                print("Scanned {} in {:.2f}s".format(
                    sample_path.relative_to(sample_directory), elapsed), flush=True)
            print("Scanned all {} regular file(s) in SAMPLES".format(
                len(sample_files)), flush=True)
        else:
            print("SAMPLES directory is not present; sample scans were skipped", flush=True)

    print("All GLogViewer CLI checks passed.")


if __name__ == "__main__":
    main()
