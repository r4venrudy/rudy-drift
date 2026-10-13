"""Regenerate committed synthetic examples; not used by the detector."""
import csv
import pathlib
import random

root = pathlib.Path(__file__).resolve().parent
rng = random.Random(20261013)
for name, count in [("train", 256), ("calibration", 128)]:
    with (root / (name + ".csv")).open("w", newline="", encoding="ascii") as f:
        w = csv.writer(f); w.writerow(["id", "log_orig_bytes", "log_duration", "orig_byte_ratio"])
        w.writerows([f"{name}-{i:03d}", f"{rng.gauss(7, .5):.8f}", f"{max(0, rng.gauss(1, .2)):.8f}", f"{rng.uniform(.1, .4):.8f}"] for i in range(count))
