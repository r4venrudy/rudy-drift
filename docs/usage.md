# Usage and data contracts

All production commands are implemented in C. Run `rudy-drift --help` for syntax. Input files are read-only; reports go to stdout and diagnostics to stderr. Paths can be absolute or relative to the current working directory. No network access is performed by the executable.

## Fit

```
rudy-drift fit train.csv calibration.csv model.rdm [--trees N] [--sample N] [--seed N]
```

Defaults: 128 trees, sample size 256 (reduced to training row count when omitted), seed 42. Limits: 1–512 trees, 2–4096 rows per sample, sample size no larger than training set. The seed is an unsigned 64-bit decimal integer. An existing output file is never replaced. A failed write may leave a partial new file; discard that file before retrying.

Training requires 2–100,000 observations; calibration requires 20–8,192. Use considerably more than the minimum and test on your own environment. Separate trusted normal periods are required. The CLI rejects the same path and identical matrices, but cannot establish that different inputs are independent, normal, or free of duplicates. Avoid repeated sessions across train/calibration/test and use time-separated evaluation for deployment.

## Score

```
rudy-drift score model.rdm observations.csv [--alpha 0.05] [--alerts-only]
```

One JSON object per observation. `score` is higher for shorter average isolation paths. `p_value = (1 + number of calibration scores >= score) / (n_calibration + 1)`. `alert` is `p_value <= alpha`; default alpha is 0.05, allowed interval `(0,1)`. Minimum possible p-value is `1/(n+1)`; smaller alpha produces a warning because alerts are impossible. A p-value is **not** the probability that traffic is benign or malicious.

Under clean exchangeable normal calibration/test samples and a model fitted independently, this rank calibration controls marginal false-alert probability. Real network traffic can violate these assumptions through time dependence, seasonality, population mixing and drift. The observed false-positive rate can differ from alpha. Fit separate baselines for genuinely different populations and validate the intended operating threshold on later traffic.

For alerts, `sensitivity` lists each feature, its observed value, training median, and score reduction after median substitution. Negative deltas are valid. These are sensitivity probes, not causal explanations. Non-alerts contain an empty array. `--alerts-only` leaves non-alert rows out of the output.

## Info

```
rudy-drift info model.rdm
```

One JSON object with the feature names and order the model expects, each feature's training median, the number of calibration rows and the smallest p-value the model can produce (`1/(n+1)`). Use it to check a CSV header or choose an alpha before scoring.

## Drift

```
rudy-drift drift model.rdm observations.csv
```

One JSON object containing row counts and per-feature `ks_distance` in `[0,1]` between the calibration reference and current batch. Zero means identical empirical distributions; one means completely separated distributions. Ties are handled at each shared value. This is a distance, with **no p-value or automatic significance verdict**. It detects marginal distribution changes only; joint relationships may change while all marginal distances remain small. Small batches are noisy. Drift does not change the model or turn a distribution change into a malware finding.

## CSV

```
id,log_bytes,log_duration
connection-1,7.18,0.42
"connection,2",8.01,1.13
```

- Header: literal `id` followed by 1–32 unique feature names. Names start with ASCII letter or underscore; remaining characters may also be digits. Maximum 48 characters.
- Scoring/calibration feature names **and order** must exactly match the model/training schema.
- Numbers: finite decimal, optional sign/exponent, absolute value at most `1e100`. No blanks, whitespace, NaN, infinity, hexadecimal, or out-of-range underflow/overflow.
- IDs: 1–255 printable ASCII characters. Unique IDs are recommended; they are labels, not ML inputs. IPs, domains and other identifiers are not learned as numbers.
- Supports LF/CRLF and CSV double-quote escaping, including commas in quoted IDs. No embedded physical newlines, UTF-8/BOM, NUL or non-ASCII data. Empty lines are rejected.
- Maximum 32,767 content bytes per physical line (32,766 before CRLF), 100,000 rows per input. The parser rejects malformed input before report output starts.
- Read and preserve exit status. Output device failures or an out-of-memory error during drift reporting can leave partial output; discard reports whenever exit status is nonzero.

On Windows, use PowerShell 7.4+ native output redirection or `cmd /c` for ASCII report redirection. Windows PowerShell 5.1's `>` may produce UTF-16, which this CSV reader intentionally rejects. Alternatively: `rudy-drift.exe zeek conn.log | Set-Content -Encoding ascii train.csv` in Windows PowerShell. Do not round feature values or change the header between fit and scoring.

## Zeek connection logs

```
rudy-drift zeek conn.log > observations.csv
```

Accepts ASCII Zeek TSV with `#separator \x09` and a `#fields` header. Required fields: `uid`, `proto`, `duration`, `orig_bytes`, `resp_bytes`, `orig_pkts`, `resp_pkts`, `missed_bytes`, `conn_state`. Extra fields are ignored. Header order is arbitrary and repeated headers are supported. If supplied, `#path` must be `conn` and `#unset_field` must be `-`. JSON logs, alternate delimiters, compressed files, and non-default unset markers are not supported in this release; convert/decompress locally first.

Numeric unset values (`-`) become zero and increment `missing_measurements`. A legitimate zero stays zero without incrementing it. Packet and byte counters must be nonnegative integers; duration may be fractional. Values remain bounded by the CSV numeric limit. UIDs must be present, printable ASCII, at most 255 characters; escaped Zeek UID text is retained literally. Protocols: tcp, udp, icmp, unknown. Standard Zeek connection-state labels are accepted; unknown labels fail explicitly.

| Feature | Definition |
| --- | --- |
| `log_duration` | `ln(1 + duration_seconds)` |
| `log_orig_bytes`, `log_resp_bytes` | `ln(1 + application_payload_bytes)` per direction |
| `log_orig_pkts`, `log_resp_pkts` | `ln(1 + packets)` per direction |
| `log_missed_bytes` | `ln(1 + missed_bytes)` |
| `orig_byte_ratio` | orig / (orig + resp) bytes, zero if total zero |
| `orig_packet_ratio` | orig / (orig + resp) packets, zero if total zero |
| `log_byte_rate` | `ln(1 + total_payload_bytes / max(duration, 0.001))` |
| `is_tcp`, `is_udp` | Protocol indicator, 0 or 1 |
| `is_failed` | 1 for S0, REJ, RSTOS0, SH; otherwise 0 |
| `is_reset` | 1 when conn_state begins with RST |
| `missing_measurements` | Count of unset values among six numeric inputs, 0–6 |

No addresses, ports, payloads or usernames are copied into the feature table. The UID and numeric measurements can still be sensitive. The generic CSV CLI does not enforce these Zeek-specific ranges; use one consistent producer for training and scoring.

Zeek field semantics: [connection logs](https://docs.zeek.org/en/master/reference/logs/conn.html), [TSV format](https://docs.zeek.org/en/current/tutorial/logs.html).

## Evidence and return codes

`evidence` accepts the same arguments as `score`; only alerts become informational native findings. A clean batch produces `{"findings":[]}`. Retain your input/model alongside the report so reviewers can reproduce the decision. Model files store calibration score and feature distributions; protect them as telemetry-derived artifacts. CRC32 is not authentication.

Exit 0: successful command, even if alerts are present. Exit 1: input/model/I/O/allocation failure. Exit 2: command syntax or option error. Scripts should parse `alert` rather than interpreting exit status as detection status.
