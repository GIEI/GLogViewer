import subprocess
import csv
import io
import os
import signal
import subprocess
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


def main():
    binary = Path(sys.argv[1]).resolve()
    scratch_root = Path(__file__).resolve().parents[1] / "build" / "test-tmp"
    scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="glogviewer-test-",
        dir=scratch_root,
    ) as temporary:
        root = Path(temporary)

        raw_file = root / "raw.log"
        raw_file.write_bytes(b"first timeout\r\nsecond line\nlast timeout")
        result = run(binary, str(raw_file), "--regex", "timeout$")
        require(result.returncode == 0, "raw regex should find matching records")
        require(result.stdout == b"first timeout\nlast timeout\n",
                "CRLF and unterminated final lines should be handled")

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
                b"Parsed records: 4\n" in statistics.stdout and
                b"Rejected records: 4\n" in statistics.stdout and
                b"Selected records: 3\n" in statistics.stdout and
                b"  ERROR: 3\n" in statistics.stdout,
                "statistics should report exact record and selected-level counts")
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
                b"service=api: 2 (estimated; error <= 0)\n" in timeline.stdout,
                "timeline should normalize UTC offsets, and heavy hitters should count field values")
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

        if os.name != "nt":
            import fcntl
            import pty
            import select
            import struct
            import termios

            tui_file = root / "tui.jsonl"
            tui_file.write_bytes(
                b'{"level":"INFO","message":"startup"}\n'
                b'{"level":"ERROR","message":"connection timeout"}\n'
            )
            master_fd, slave_fd = pty.openpty()
            fcntl.ioctl(slave_fd, termios.TIOCSWINSZ,
                        struct.pack("HHHH", 24, 100, 0, 0))
            tui_process = subprocess.Popen(
                [str(binary), str(tui_file), "--format", "jsonl", "--tui"],
                stdin=slave_fd,
                stdout=slave_fd,
                stderr=slave_fd,
                close_fds=True,
            )
            os.close(slave_fd)
            tui_output = bytearray()
            try:
                def read_until(needle, timeout):
                    deadline = time.monotonic() + timeout
                    while needle not in tui_output and tui_process.poll() is None and time.monotonic() < deadline:
                        ready, _, _ = select.select([master_fd], [], [], 0.1)
                        if ready:
                            try:
                                tui_output.extend(os.read(master_fd, 65536))
                            except OSError:
                                break

                read_until(b"GLogViewer", 2)
                os.write(master_fd, b"/")
                read_until(b"\033[24;1H\033[2K/", 2)
                os.write(master_fd, b"timeout\n")
                read_until(b"Search: timeout", 2)
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
                require(tui_process.returncode == 0 and
                        b"connection timeout" in tui_output and
                        b"Search: timeout" in tui_output,
                        "TUI should render records and apply an interactive regex search")
            finally:
                if tui_process.poll() is None:
                    tui_process.kill()
                    tui_process.wait(timeout=5)
                os.close(master_fd)

        text_export = root / "selected.log"
        exported = run(binary, str(json_file), "--format", "jsonl", "--filter",
                       "level=ERROR", "--export", "text", "--output", str(text_export))
        require(exported.returncode == 0 and text_export.read_bytes() ==
                b'{"level":"ERROR","message":"connection timeout"}\n'
                b'{"severity":"ERROR","msg":"timeout"}\n'
                b'{"level":"ERROR","message":"other"}\n',
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
                                '{"level":"ERROR","message":"other"}'],
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
        require(b"skipped 4 invalid" in result.stderr,
                "malformed, duplicate-key, nested, and invalid Unicode JSON should be counted")

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
        common_line = (
            b'203.0.113.8 - alice [10/Oct/2000:13:55:36 -0700] '
            b'"GET / HTTP/1.1" 204 0\n'
        )
        access_file.write_bytes(common_line)
        common = run(binary, str(access_file), "--format", "apache-common",
                      "--filter", "status=204")
        require(common.returncode == 0 and common.stdout == common_line,
                "Apache Common parsing should accept the Common field layout")

        syslog_file = root / "syslog.log"
        syslog3164 = b"Oct 11 22:14:15 mymachine su[1234]: 'su root' failed\n"
        syslog_file.write_bytes(syslog3164)
        result = run(binary, str(syslog_file), "--format", "syslog3164",
                     "--filter", "pid=1234")
        require(result.returncode == 0 and result.stdout == syslog3164,
                "RFC 3164 parsing should extract the process ID")
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
                     "--filter", "level=CRIT")
        require(result.returncode == 0 and result.stdout == syslog5424,
                "RFC 5424 should extract severity from PRI")

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
        else:
            print("http_access_500MB.log: skipped because the local sample is not present")

    print("All GLogViewer CLI checks passed.")


if __name__ == "__main__":
    main()
