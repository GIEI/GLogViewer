# Feature Catalogue and Product Positioning

**Status:** Version 1.6.0 is the current stable release. It adds Boolean quoted-literal search to TUI regex search. The 1.6.0 tagged workflow verifies platform builds and tests. Exact behavior and known limits are specified in [Technical Specification](TECHNICAL_SPECIFICATION.md).

## 1. Users and jobs

| User | Typical question | Log data | Required workflow |
| --- | --- | --- | --- |
| Application developer | “Which requests failed after deployment?” | Application text/JSON, request IDs, stack-trace summaries | Select errors, search message or request ID, export evidence. |
| SRE or operator | “Did the service begin failing at a particular time?” | Syslog, container output, proxy access/error | Follow a live file, compare level counts and timeline. |
| Web administrator | “Which clients caused a spike in 5xx responses?” | Apache/NGINX access logs | Filter status, inspect client IP and request, export CSV. |
| Security analyst | “Which hosts or processes show repeated suspicious patterns?” | Syslog and HTTP access records | Regex search, field predicates, frequent-value summary. |

Common application fields worth preserving are timestamp, severity, message, service, component/logger, host, process, thread, request or correlation ID, trace/span ID, HTTP method/path/status, client IP, exception type, and duration. These are design priorities inferred from the [OpenTelemetry log data model](https://opentelemetry.io/docs/specs/otel/logs/data-model/), [Log4j structured layout](https://logging.apache.org/log4j/2.x/manual/layouts.html), and [Apache](https://httpd.apache.org/docs/2.4/logs.html)/[NGINX](https://nginx.org/en/docs/http/ngx_http_log_module.html) access-log fields; they are not a claim that every source provides each field.

## 2. Version 1.0 scope

“Must” identifies features included in the 1.0 release. The first MVP was smaller; the [roadmap](ROADMAP.md) records how the implementation grew through each stage.

| Feature | v1 priority | User value | Notes |
| --- | --- | --- | --- |
| Bounded streaming reader | Must | Inspect multi-GB files without loading them whole | 16 MiB line cap; rejected-line counter. |
| Named format presets | Must | Get useful fields immediately | Separate syslog variants and HTTP access formats. |
| Custom placeholder pattern | Must | Work with application-specific text logs | Deterministic delimiter-based extraction. |
| JSON Lines and formatted JSON object arrays | Must | Analyze structured application logs and exported datasets | Bounded nested objects/arrays, dotted field paths, and leaf aliases. |
| Field filters with AND/OR | Must | Reduce noise using structured data | Explicit precedence and missing-field rules. |
| Range filters and record sampling | Should | Inspect record structure and select ordered value windows | Bytewise field comparisons (`<`, `<=`, `>`, `>=`), `--record first|last`, and `--top/--bottom N`. |
| Internal regex on raw line or field | Must | Find text and highlight evidence | Supported subset; iterative NFA execution. |
| Raw-line regex exclusion | Implemented | Remove known noise while retaining other matches | `--exclude-regex` works with batch, TUI, follow, exports, statistics, and query profiles. |
| CLI batch mode | Must | Use in scripts and incident runbooks | Stable exit codes and diagnostics. |
| TUI browse/search/help | Must | Explore filtered records interactively | Bounded viewport cache, full-scan checkpoints, regex and Boolean quoted-literal search, source-line jumps and bookmarks, severity jumps, configurable context, formatted JSON zoom, field-value summary, and navigable time histogram. |
| Query profiles | Must | Repeat investigations without rebuilding command options | Versioned portable settings shared by batch CLI and TUI, with explicit overwrite and validation. |
| Follow mode | Must | Watch a live application file | Append/truncate/rotation behavior documented. |
| Text and CSV export | Must | Share a filtered evidence set | Streamed output with fixed CSV columns. |
| Counts, timeline, frequent values | Must | Spot error spikes and dominant fields | Exact counters; bounded estimated heavy hitters. |
| Multi-file merge | Implemented in 1.4 | Correlate several services | Batch only; each source must be timestamp-monotonic. Unknown timestamps are grouped at the end. TUI, follow, and result-window modes remain single-file. |
| Native journal/Event Log readers | Later | Inspect platform stores directly | Conflicts with a simple portable file-only v1. |
| Kubernetes CRI parser | Implemented in 1.5 | Filter container stdout/stderr and inspect timestamps and fragment tags | One physical CRI row remains one record; `P`/`F` are exposed as tags. |
| Multiline event reconstruction, compressed input | Later | Reassemble fragments and inspect rotated/compressed archives | Needs explicit rules for partial records, rotation, and dependencies. |
| Automatic template discovery | Research | Reduce custom-pattern setup | Related to [Drain](https://pinjiahe.github.io/files/pdf/research/ICWS17.pdf); not required for v1. |

## 3. Format priorities

1. **Application text and JSON:** frequent in services and frameworks; the product's main differentiation is combining raw regex with named fields. Log4j explicitly supports human-readable Pattern Layout and structured JSON output. [Log4j layouts](https://logging.apache.org/log4j/2.x/manual/layouts.html)
2. **HTTP access logs:** Apache documents Common/Combined, and NGINX documents configurable access formats. Status, client IP, request, and referrer support operational and security questions. [Apache logs](https://httpd.apache.org/docs/2.4/logs.html), [NGINX HTTP log module](https://nginx.org/en/docs/http/ngx_http_log_module.html)
3. **Syslog:** RFC 3164 and RFC 5424 differ enough to require distinct parsers, especially around timestamp, header, and structured data. [RFC 3164](https://www.rfc-editor.org/info/rfc3164/), [RFC 5424](https://www.rfc-editor.org/info/rfc5424/)
4. **Container exports:** Kubernetes commonly captures application stdout/stderr in CRI log form, while Docker's JSON-file driver wraps stdout/stderr and timestamp in JSON. CRI files use a timestamp, stream, tag, and message layout; Docker JSON can map through the simple JSON parser. [Kubernetes logging proposal](https://github.com/kubernetes/design-proposals-archive/blob/main/node/kubelet-cri-logging.md), [Docker logging](https://docs.docker.com/engine/logging/drivers/json-file/)

## 4. Existing alternatives and fit

| Product or approach | Documented strength | When it may be preferable | GLogViewer design opportunity |
| --- | --- | --- | --- |
| [lnav](https://docs.lnav.org/en/latest/) | Terminal log navigation, built-in/custom formats, SQL virtual tables, JSON pretty-printing, histograms, and advanced views. [UI documentation](https://docs.lnav.org/en/latest/ui.html), [SQLite interface](https://docs.lnav.org/en/latest/sqlext.html) | Rich established interactive analysis, especially SQL-based exploration. | Small C99, no external runtime libraries, focused regex/filter subset, fixed-memory summaries. |
| [GoAccess](https://goaccess.io/) | Real-time HTTP access analytics in terminal and browser; CSV/JSON/HTML reports. | Web traffic dashboards and detailed HTTP metrics. | Mixed application/syslog/custom logs rather than HTTP specialization. |
| [klogg](https://github.com/voyager42/klogg) | Cross-platform GUI search of huge logs with multithreaded indexing, regex, watch mode, and separate search results/context. [Project documentation](https://github.com/voyager42/klogg) | Graphical desktop exploration and rapid indexed search. | Scriptable CLI plus terminal workflow, structured field filters, and bounded summaries without a persistent index. |
| [GNU grep](https://www.gnu.org/software/grep/manual/grep.html) with text tools | Mature line-oriented pattern search and pipelines. | One-off searches and existing shell pipelines. | Built-in field extraction, combined predicates, TUI, and summaries. |

These products suggest useful interaction patterns: lnav connects aggregate views and structured inspection, while klogg keeps search responsive on very large files with indexing and separates result navigation from source context. GLogViewer 1.2 adopts aggregate-to-record navigation with a bounded two-pass histogram and focused field summaries. It does not claim feature parity or performance superiority; the implementation remains a single-file local analyzer with a deliberately smaller feature set.

## 5. Usability criteria

- A new user can inspect an Apache Combined log with one explicit `--format` option.
- A user can express `level=ERROR and message~"timeout"` without writing a parser script.
- A 1 GB file can be scanned in batch without memory growth proportional to its size; actual throughput is measured with the documented benchmark.
- A malformed line is visible as raw data and reflected in the rejected count.
- A user can tell whether statistics are exact or estimated.
- A user can see active filters, scan progress, and help in the TUI.

## 6. Research inputs and future validation

[Loghub](https://github.com/logpai/loghub) provides documented datasets from distributed systems and other software and can supply format diversity for tests after license/privacy review. The [Loghub paper](https://arxiv.org/abs/2008.06448) and [Loghub 2.0 evaluation](https://arxiv.org/abs/2308.10828) indicate that real-world log parsing spans varied software and formats. These sources motivate a test corpus, not a claim that the planned parser automatically understands all those logs.

Product fit should be tested with real incident tasks on representative files: time to first useful filter, accuracy of parsed fields, responsiveness while scanning, and correctness of exported evidence. No user study or benchmark has been performed in this repository.
