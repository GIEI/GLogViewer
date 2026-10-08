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
        invalid_filter = run(binary, str(json_file), "--filter", "(level=ERROR")
        require(invalid_filter.returncode == 3,
                "unclosed filter parentheses should return syntax status")
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

    print("All GLogViewer CLI checks passed.")


if __name__ == "__main__":
    main()
