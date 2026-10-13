# Security

Rudy Drift is an offline research preview for authorized network analysis. It reads local input/model files and writes user-requested reports or a new model. It does not execute imported content, scan hosts, collect credentials or send telemetry.

Please report memory-safety defects, model/CSV parser crashes, unsafe output handling or demonstrated security boundary failures using this repository's **Security → Report a vulnerability** when available. If private reporting is unavailable, open an issue requesting a private contact without exploit details or sensitive telemetry. Never include real credentials, packet captures or customer logs in a public issue; a synthetic reproducer is preferable.

Keep normal training/calibration sources curated. A poisoned baseline can suppress detections. An anomaly score is not proof of malware, and this preview must not make autonomous blocking decisions. IDs, feature values and model distributions can reveal operational information; review artifacts before sharing. The binary model has corruption checks but no cryptographic signature. Local input files and output directories should be controlled by the operator.

The parser uses explicit size limits and rejects nonfinite values. CI exercises sanitizer builds and malformed model/CSV cases; this is not a formal memory-safety proof or an independent security audit. Only the latest published preview is maintained.
