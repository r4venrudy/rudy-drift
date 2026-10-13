# Benchmarks: what was measured

These are reproducible checks of this implementation, not evidence of state-of-the-art detection or readiness for unattended blocking. All published runs use 128 trees, sample size 256 and alpha 0.05. Seeds 42, 43 and 44 vary data generation/splits and forest randomness. No hyperparameter search or selection of the best seed was performed.

## Results

Arithmetic mean across three seeds, rounded here; full precision and per-family results are in [results/](results/).

| Dataset | Detector | AUROC | Attack alert rate | Normal alert rate |
| --- | --- | ---: | ---: | ---: |
| Synthetic two-cluster stress test | Native Isolation Forest | 0.9687 | 65.10% | 3.30% |
| Synthetic two-cluster stress test | Calibrated median/MAD | 0.7961 | 52.33% | 4.67% |
| KDD99 numeric subset | Native Isolation Forest | 0.9902 | 93.20% | 4.71% |
| KDD99 numeric subset | Calibrated median/MAD | 0.9167 | 10.91% | 4.87% |

**The KDD99 aggregate is dominated by one attack family.** In seed 42, 5,452 of 6,000 attack rows are `neptune`, all flagged by the forest. The same run catches only 6 of 80 `back`, 2 of 22 `nmap`, and 5 of 82 `warezclient` observations. Several tiny categories have zero detections. The simple baseline performs better on some of those small categories. Inspect [the complete breakdown](results/kdd99-42.json) before drawing conclusions.

The baseline has good KDD ranking but much lower detection at the calibrated operating threshold. Those are different properties. Zero-MAD features can dominate its maximum score; its scale floor of `1e-9` is fixed and documented. This comparison is against one simple baseline, not a modern IDS or a tuned competing ML system. Neither aggregate metric says anything about discovering new vulnerabilities.

## Protocol

Both detectors receive the same training, calibration and test rows. The forest never receives labels. The baseline is `max_j |x_j - median_j| / max(1.4826 × MAD_j, 1e-9)`, with medians/MAD fitted only on training. Both calibrate a tie-inclusive upper-tail rank on the same held-out normal observations and alert at p ≤ 0.05. AUROC uses average ranks for tied scores; metric code has known-answer tests.

Synthetic: 2,048 training, 1,024 calibration, 1,000 normal test and 1,000 anomalous test observations. Normal data has two correlated clusters plus independent noise. Half the anomalies are obvious tail values; half combine individually familiar coordinates in an unfamiliar way. This stress test deliberately exposes the limitations of a univariate baseline. It is not realistic network traffic.

KDD99: 494,021 source rows transformed to 16 fixed numeric flow features. Deduplicate **transformed feature vectors before splitting**, discard the two normal/attack label conflicts, then shuffle with the recorded seed. There are 84,672 distinct normal and 52,958 distinct attack vectors after that filter. Train on 10,000 normal rows, calibrate on 4,096 different normal rows, test on 3,000 further normal and 6,000 attack rows. Distinct attack labels that map to the same vector retain the first source label for the breakdown. Train/calibration/test share no identical transformed vector. This random historical split does not simulate future traffic or a previously unseen attack family.

The selected KDD features and transformations are listed directly in [evaluate.py](../benchmarks/evaluate.py). They are a **different schema from the Zeek adapter**. Scores from this benchmark do not validate Zeek deployment performance. Source traffic dates to 1999, contains legacy laboratory attack patterns, and is heavily imbalanced. Real deployment requires contemporary, consented telemetry and chronological evaluation in the intended environment.

## Reproduce

Build the CLI first. Only standard-library Python is needed; benchmark data is opt-in, downloaded with a fixed SHA-256, and remains local. Each command needs a fresh output directory because model files are never overwritten.

```sh
python benchmarks/evaluate.py --exe build/rudy-drift --out benchmark-results/synthetic-42 --seed 42
python benchmarks/evaluate.py --exe build/rudy-drift --dataset kdd99 --out benchmark-results/kdd99-42 --seed 42
```

Repeat with `--seed 43` and `--seed 44`, changing output directories. For Visual Studio builds use `build/Release/rudy-drift.exe`. JSON output includes row counts, per-family alert rates, data hash, model size and CLI timing. Timing includes loading/CSV parsing/output and explanations for alerts; it is hardware-dependent and is not a cross-system performance claim. Published measurements were taken locally on Windows with LLVM-MinGW Clang 23.1.3, release optimization.

Data source: Stolfo, Fan, Lee, Prodromidis and Chan, [KDD Cup 1999 Data, UCI](https://doi.org/10.24432/C51C7N), listed by UCI under CC BY 4.0. The downloader uses the [archive and checksum recorded by scikit-learn](https://github.com/scikit-learn/scikit-learn/blob/main/sklearn/datasets/_kddcup99.py). Downloaded rows and generated benchmark matrices are git-ignored and not distributed in this repository. Aggregate results and synthetic examples are included.
