# RDM format v1

All integers are unsigned little-endian; doubles are IEEE binary64 encoded as little-endian bits. No native pointers, struct padding, executable code or external paths are serialized.

| Field | Width |
| --- | --- |
| Magic/version `RDRIFT\0\x01` | 8 bytes |
| Feature count, tree count, sample size, calibration row count | 4 bytes each |
| Seed | 8 bytes |
| For each feature: NUL-terminated name in 49 bytes, training median | 49 + 8 bytes |
| Sorted calibration scores | 8 × calibration rows |
| Sorted calibration values, feature-major | 8 × features × calibration rows |
| For each tree: node count | 4 bytes |
| For each node: feature, left child, right child, observation count, split | 4 + 4 + 4 + 4 + 8 bytes |
| IEEE CRC32 of every preceding byte | 4 bytes |

Leaf feature is `UINT32_MAX`; left, right and split must be zero. Internal children must be distinct forward indices within the tree. Every nonroot node has exactly one incoming edge, every node is reachable, child counts sum to the parent, root count equals sample size, and depth cannot exceed `ceil(log2(sample_size))`. These checks prevent loops, missing children and unsafe traversal.

The loader bounds dimensions before allocating, rejects malformed/duplicate names, invalid scores, unsorted/nonfinite reference data, invalid trees, CRC mismatches, truncation and trailing bytes. Limits match the public API (32 features, 512 trees, sample size 4096, calibration rows 8192). CRC32 catches accidental corruption; it cannot establish provenance or stop an attacker who can rewrite both data and checksum. Transport/store models with the same access controls as their source telemetry. No model authenticity/signature feature is claimed.
