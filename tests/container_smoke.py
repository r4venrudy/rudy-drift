"""Exercise the installed image with no network and a read-only root filesystem."""
import json
import subprocess
import sys

image = sys.argv[1]
command = ["docker", "run", "--rm", "-i", "--network", "none", "--read-only",
           "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--pids-limit", "64",
           "--tmpfs", "/data:rw,uid=10001,gid=10001,mode=0700",
           "--entrypoint", "/bin/sh", image, "-eu"]
script = """
test "$(id -u)" = 10001
test ! -e /src
test ! -e /usr/bin/gcc
examples=/usr/local/share/rudy-drift/examples
rudy-drift fit "$examples/train.csv" "$examples/calibration.csv" /data/model.rdm
rudy-drift score /data/model.rdm "$examples/observations.csv" --alpha 0.05
rudy-drift drift /data/model.rdm "$examples/observations.csv"
rudy-drift evidence /data/model.rdm "$examples/observations.csv" --alpha 0.05
if rudy-drift fit "$examples/train.csv" "$examples/calibration.csv" /data/model.rdm >/dev/null 2>&1; then
  exit 1
fi
"""
result = subprocess.run(command, input=script, text=True, capture_output=True, timeout=120)
if result.returncode:
    raise SystemExit(result.stderr or "Container smoke test failed")
rows = [json.loads(line) for line in result.stdout.splitlines()]
assert len(rows) == 7, rows
fit, scores, drift, evidence = rows[0], rows[1:5], rows[5], rows[6]
assert fit["features"] == 3 and fit["training_rows"] > 0 and fit["calibration_rows"] > 0
assert {s["id"] for s in scores} == {"ordinary-001", "ordinary-002", "review-001", "review-002"}
assert all(0 <= s["score"] <= 1 and 0 < s["p_value"] <= 1 for s in scores)
assert drift["rows"] == 4 and len(drift["features"]) == 3
assert all(0 <= f["ks_distance"] <= 1 for f in drift["features"])
assert {f["asset"]["id"] for f in evidence["findings"]} == {s["id"] for s in scores if s["alert"]}
assert all(f["severity"] == "info" for f in evidence["findings"])
print("Installed container passed fit, score, drift, evidence and overwrite-protection checks.")
