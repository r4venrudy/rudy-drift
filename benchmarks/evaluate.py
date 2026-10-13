"""Reproduce native-C detector vs. median/MAD baseline; Python is evaluation only.

No third-party Python modules required. KDD data is opt-in and never uploaded.
"""
import argparse
import bisect
import csv
import gzip
import hashlib
import io
import json
import math
import pathlib
import random
import statistics
import subprocess
import time
import urllib.request

URL = "https://ndownloader.figshare.com/files/5976042"
SHA256 = "8045aca0d84e70e622d1148d7df782496f6333bf6eb979a1b0837c42a9fd9561"


def synthetic(seed):
    rng = random.Random(seed)
    def normal():
        cluster = rng.choice((-1, 1))
        return [cluster * 3 + rng.gauss(0, .45), cluster * 3 + rng.gauss(0, .45), rng.gauss(0, 1)]
    train = [normal() for _ in range(2048)]
    calibration = [normal() for _ in range(1024)]
    normal_test = [normal() for _ in range(1000)]
    # Two anomaly types: obvious tails, and unfamiliar combinations of individually normal values.
    tails = [[rng.uniform(8, 12), rng.uniform(8, 12), rng.gauss(0, 1)] for _ in range(500)]
    combinations = [[-3 + rng.gauss(0, .45), 3 + rng.gauss(0, .45), rng.gauss(0, 1)] for _ in range(500)]
    return ["cluster_x", "cluster_y", "noise"], train, calibration, normal_test + tails + combinations, [0] * 1000 + [1] * 1000, ["normal"] * 1000 + ["tail"] * 500 + ["combination"] * 500, {"seed": seed, "dataset": "synthetic two-cluster stress test"}


def kdd(cache, seed):
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / "kddcup.data_10_percent.gz"
    if not archive.exists():
        with urllib.request.urlopen(URL, timeout=60) as response:
            payload = response.read(10_000_001)
        if len(payload) > 10_000_000 or hashlib.sha256(payload).hexdigest() != SHA256:
            raise ValueError("dataset size or SHA-256 mismatch")
        archive.write_bytes(payload)
    payload = archive.read_bytes()
    if hashlib.sha256(payload).hexdigest() != SHA256:
        raise ValueError("cached dataset SHA-256 mismatch")
    # A flow-oriented numeric subset; this is not the Zeek feature schema.
    columns = [(0,"log_duration",True),(4,"log_src_bytes",True),(5,"log_dst_bytes",True),
               (22,"log_count",True),(23,"log_srv_count",True),(24,"serror_rate",False),
               (25,"srv_serror_rate",False),(26,"rerror_rate",False),(27,"srv_rerror_rate",False),
               (28,"same_srv_rate",False),(29,"diff_srv_rate",False),(30,"srv_diff_host_rate",False),
               (31,"log_dst_host_count",True),(32,"log_dst_host_srv_count",True),
               (33,"dst_host_same_srv_rate",False),(34,"dst_host_diff_srv_rate",False)]
    unique, ambiguous = {}, set()
    raw_rows = 0
    with gzip.GzipFile(fileobj=io.BytesIO(payload)) as zipped:
        for row in csv.reader(io.TextIOWrapper(zipped, encoding="ascii")):
            raw_rows += 1
            vector = tuple(math.log1p(float(row[i])) if log else float(row[i]) for i, _, log in columns)
            label = row[-1].rstrip(".")
            if vector in unique and (unique[vector] == "normal") != (label == "normal"):
                ambiguous.add(vector)
            unique.setdefault(vector, label)
    normal = [v for v, label in unique.items() if label == "normal" and v not in ambiguous]
    attacks = [(v, label) for v, label in unique.items() if label != "normal" and v not in ambiguous]
    rng = random.Random(seed); rng.shuffle(normal); rng.shuffle(attacks)
    if len(normal) < 17096:
        raise ValueError("insufficient distinct normal observations for fixed split")
    train, calibration, normal_test = normal[:10000], normal[10000:14096], normal[14096:17096]
    attack_test = attacks[:6000]
    metadata = {"dataset": "KDD Cup 1999 10 percent, legacy benchmark", "source": URL, "sha256": SHA256,
                "seed": seed, "source_rows": raw_rows, "distinct_feature_vectors": len(unique),
                "discarded_ambiguous_vectors": len(ambiguous), "normal_unique": len(normal), "attack_unique": len(attacks),
                "selection": "Deduplicate transformed feature vectors before splitting; discard normal/attack label conflicts; train/calibrate on normal only."}
    return [name for _, name, _ in columns], train, calibration, normal_test + [v for v, _ in attack_test], [0] * len(normal_test) + [1] * len(attack_test), ["normal"] * len(normal_test) + [label for _, label in attack_test], metadata


def metrics(scores, pvalues, labels, families, alpha):
    npos, nneg = sum(labels), len(labels) - sum(labels)
    # Mann-Whitney AUROC with average ranks for tied scores.
    ranked = sorted(zip(scores, labels)); rank_sum = 0.; start = 0
    while start < len(ranked):
        end = start + 1
        while end < len(ranked) and ranked[end][0] == ranked[start][0]:
            end += 1
        rank_sum += sum(v for _, v in ranked[start:end]) * ((start + 1 + end) / 2)
        start = end
    auc = (rank_sum - npos * (npos + 1) / 2) / (npos * nneg)
    alerts = [p <= alpha for p in pvalues]
    tp = sum(a and y for a, y in zip(alerts, labels)); fp = sum(a and not y for a, y in zip(alerts, labels))
    breakdown = {}
    for family in sorted(set(families)):
        members = [i for i, name in enumerate(families) if name == family]
        breakdown[family] = {"rows": len(members), "alert_rate": sum(alerts[i] for i in members) / len(members)}
    return {"auroc": auc, "true_positive_rate": tp / npos, "false_positive_rate": fp / nneg,
            "precision": tp / (tp + fp) if tp + fp else 0, "alerts": tp + fp, "by_family": breakdown}


def evaluate(exe, directory, bundle, alpha):
    names, train, cal, test, labels, families, metadata = bundle
    directory.mkdir(parents=True, exist_ok=True)
    for label, rows in [("train", train), ("cal", cal), ("test", test)]:
        with (directory / (label + ".csv")).open("w", newline="", encoding="ascii") as f:
            writer = csv.writer(f); writer.writerow(["id", *names]); writer.writerows([f"{label}-{i}", *r] for i, r in enumerate(rows))
    model = directory / "model.rdm"
    if model.exists():
        raise ValueError("use a fresh output directory; existing models are preserved")
    before = time.perf_counter()
    fit = subprocess.run([str(exe), "fit", str(directory / "train.csv"), str(directory / "cal.csv"), str(model), "--seed", str(metadata["seed"])], check=True, capture_output=True, text=True)
    fit_seconds = time.perf_counter() - before
    before = time.perf_counter()
    output = subprocess.run([str(exe), "score", str(model), str(directory / "test.csv"), "--alpha", str(alpha)], check=True, capture_output=True, text=True)
    score_seconds = time.perf_counter() - before
    predictions = [json.loads(s) for s in output.stdout.splitlines()]
    if len(predictions) != len(test):
        raise ValueError("incomplete scoring output")
    # Baseline: maximum absolute robust marginal z-score, calibrated on SAME rows.
    medians = [statistics.median(col) for col in zip(*train)]
    scales = [max(1.4826 * statistics.median(abs(v - medians[j]) for v in col), 1e-9) for j, col in enumerate(zip(*train))]
    def robust(row):
        return max(abs(x - medians[j]) / scales[j] for j, x in enumerate(row))
    baseline_cal = sorted(map(robust, cal)); baseline_scores = list(map(robust, test))
    baseline_p = [(1 + len(cal) - bisect.bisect_left(baseline_cal, s)) / (1 + len(cal)) for s in baseline_scores]
    result = {"metadata": metadata, "fit": json.loads(fit.stdout), "test_rows": len(test), "alpha": alpha,
              "native_iforest": metrics([r["score"] for r in predictions], [r["p_value"] for r in predictions], labels, families, alpha),
              "median_mad": metrics(baseline_scores, baseline_p, labels, families, alpha),
              "local_timing": {"fit_seconds": fit_seconds, "score_with_alert_explanations_seconds": score_seconds, "model_bytes": model.stat().st_size},
              "limitations": "Synthetic and 1999 traffic do not establish modern deployment accuracy. No superiority claim. Timings include CLI I/O and are hardware-dependent."}
    (directory / "results.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--dataset", choices=["synthetic", "kdd99"], default="synthetic")
    parser.add_argument("--cache", type=pathlib.Path, default=pathlib.Path("benchmark-data"))
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--alpha", type=float, default=.05)
    args = parser.parse_args()
    if not 0 < args.alpha < 1 or not 0 <= args.seed < 2**64:
        parser.error("alpha must be in (0,1), seed in uint64 range")
    bundle = synthetic(args.seed) if args.dataset == "synthetic" else kdd(args.cache, args.seed)
    result = evaluate(args.exe.resolve(), args.out.resolve(), bundle, args.alpha)
    print(json.dumps(result, indent=2))
