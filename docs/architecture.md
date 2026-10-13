# Architecture

```
Zeek conn.log ----> C TSV adapter ----> named numeric CSV
                                            |
trusted training ----> Isolation Forest      |
                          |                 |
separate calibration ---> scores + reference distributions
                          |
                    immutable .rdm model
                          |
new observations ---------+---> score / tail rank / median sensitivity
                          +---> per-feature KS distance
                          +---> Rudy Evidence native findings
```

`src/forest.c` is the standalone library. It has no global mutable state, callbacks, file discovery, threads, or network access. Concurrent reads of a fitted/loaded model are safe provided callers use independent output buffers; free and file operations require caller synchronization. Inputs are row-major doubles. Public API errors use return values and an optional 256-byte error buffer. Invalid-input functions do not promise to preserve caller output buffers. The caller owns input arrays and FILE handles; `rd_free` owns model cleanup.

`src/data.c` validates bounded CSV/Zeek batches and owns their allocations. `src/main.c` enforces the persisted feature schema, parses options and renders reports. Observations are held in memory, capped at 100,000; this release is intentionally not a live packet sensor or streaming database.

## Forest

Each tree samples without replacement using a seeded SplitMix64 generator with rejection sampling for bounded integers. At a node, the implementation chooses uniformly among nonconstant features and then chooses a split uniformly over the sampled range. Constant features are skipped instead of terminating an otherwise splittable node. The `<=` split convention and a rounding guard prevent empty children. Maximum depth is `ceil(log2(sample_size))`.

A leaf's path length includes `c(n) = 2 H(n-1) - 2(n-1)/n`, with `c(0)=c(1)=0`, computed from harmonic sums. The score is `2^(-mean_path_length/c(sample_size))`. Longer paths are less anomalous. The design follows the published [Isolation Forest / Isolation-Based Anomaly Detection research](https://cs.nju.edu.cn/zhouzh/zhouzh.files/publication/tkdd11.pdf); the C implementation here is original. This is not a claim of a new algorithm.

Calibration scores and each calibration feature's marginal values are stored sorted. A binary search computes tie-inclusive upper-tail ranks. Training medians support sensitivity probes; the sorted reference supports exact empirical KS distances. The model never learns from scored observations automatically. Retraining requires a deliberate new fit with curated normal inputs and a new output path.

Training work is approximately O(trees × sample × features × log(sample)), plus median sorting and sampling-index initialization. Scoring is O(trees × log(sample)); alert sensitivity adds one scoring pass per feature. Worst-case memory is bounded by 512 trees × (2 × 4096 − 1) nodes, 8,192 × 32 calibration values, and input matrices. This is a batch library, not a promise of millions of events per second.

## Limitations

Random seeds produce identical bytes on repeated runs of the same build. Different compilers/platform math libraries may differ in last-bit floating-point values; cross-platform model reading is supported, bit-identical training across platforms is not promised. IEEE binary64 is required.

Axis-aligned splits are sensitive to feature selection. Novel values in a feature constant during training cannot affect tree paths. Extreme values beyond the training range may receive equal scores. Normal behavior changes over time and false positives are expected. Median substitutions can leave a correlated data manifold. Univariate drift cannot detect every multivariate shift. Calibration does not repair poisoned training data or feature leakage.
