Native C17 network anomaly triage, delivered as a CLI and embeddable static library.

- Seeded Isolation Forest with separate held-out score calibration.
- Zeek connection-log TSV importer and strict named numeric CSV input.
- Median-substitution feature sensitivity and per-feature KS drift reports.
- Rudy Evidence native export for informational review leads.
- Versioned, validated model files with CRC32 corruption checks.
- Reproducible synthetic and historical KDD99 comparisons with per-family misses.

Research preview: anomaly scores are investigation leads, not malware probabilities or vulnerability proofs. KDD99 results do not establish modern operational accuracy. Read the limitations and input contracts before choosing a baseline.

The release workflow tests Windows, Linux and macOS, builds an installed-library consumer, and runs Linux ASan/UBSan before publishing. ZIPs include the CLI, library/header, documentation, license and synthetic examples. Windows binaries are unsigned. Verify downloaded files against SHA256SUMS.txt. Python is needed only to run tests or benchmark scripts.
