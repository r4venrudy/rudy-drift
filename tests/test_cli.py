"""Integration tests: the production program and ML implementation are native C."""
import csv
import json
import math
import pathlib
import random
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

EXE = str(pathlib.Path(sys.argv.pop(1)).resolve())


class CLI(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        rng = random.Random(42)
        self.write_csv("train.csv", [(f"train-{i}", rng.gauss(0, 1), rng.gauss(0, 1)) for i in range(256)])
        self.write_csv("cal.csv", [(f"cal-{i}", rng.gauss(0, 1), rng.gauss(0, 1)) for i in range(128)])
        self.write_csv("test.csv", [("normal", 0, 0), ('unusual, "connection"', 100, 100)])
        self.run_cli("fit", "train.csv", "cal.csv", "model.rdm", "--trees", "32")

    def tearDown(self):
        self.temp.cleanup()

    def write_csv(self, path, rows, header=("id", "a", "b")):
        with (self.root / path).open("w", newline="", encoding="ascii") as f:
            w = csv.writer(f)
            w.writerow(header)
            w.writerows(rows)

    def run_cli(self, *args, success=True):
        p = subprocess.run([EXE, *args], cwd=self.root, capture_output=True, text=True)
        if success:
            self.assertEqual(p.returncode, 0, p.stderr)
        else:
            self.assertNotEqual(p.returncode, 0, p.stdout)
        return p

    def test_utf8_bom_header(self):
        data = (self.root / "test.csv").read_bytes()
        (self.root / "bom.csv").write_bytes(b"\xef\xbb\xbf" + data)
        plain = self.run_cli("score", "model.rdm", "test.csv").stdout
        self.assertEqual(self.run_cli("score", "model.rdm", "bom.csv").stdout, plain)
        (self.root / "latin.csv").write_bytes(b"\xe9" + data)
        self.run_cli("score", "model.rdm", "latin.csv", success=False)

    def test_version_is_one_line(self):
        out = self.run_cli("--version").stdout
        self.assertRegex(out, r"^rudy-drift \d+\.\d+\.\d+\S*\n$")
        self.assertIn("rudy-drift fit", self.run_cli("--help").stdout)

    def test_info(self):
        info = json.loads(self.run_cli("info", "model.rdm").stdout)
        self.assertEqual([f["name"] for f in info["features"]], ["a", "b"])
        self.assertEqual(info["calibration_rows"], 128)
        self.assertEqual(info["minimum_p_value"], 1 / 129)
        self.run_cli("info", "missing.rdm", success=False)

    def test_score_and_evidence(self):
        out = [json.loads(x) for x in self.run_cli("score", "model.rdm", "test.csv").stdout.splitlines()]
        self.assertFalse(out[0]["alert"])
        self.assertTrue(out[1]["alert"])
        self.assertEqual(out[1]["id"], 'unusual, "connection"')
        self.assertEqual(len(out[1]["sensitivity"]), 2)
        evidence = json.loads(self.run_cli("evidence", "model.rdm", "test.csv").stdout)
        self.assertEqual(len(evidence["findings"]), 1)
        self.assertEqual(evidence["findings"][0]["asset"]["id"], out[1]["id"])
        self.assertEqual(evidence["findings"][0]["severity"], "info")

    def test_alerts_only(self):
        out = self.run_cli("score", "model.rdm", "test.csv", "--alerts-only", "--alpha", "0.05").stdout.splitlines()
        self.assertEqual([json.loads(x)["id"] for x in out], ['unusual, "connection"'])
        self.run_cli("drift", "model.rdm", "test.csv", "--alerts-only", success=False)
        self.run_cli("score", "model.rdm", "test.csv", "--alerts-only", "--alerts-only", success=False)

    def test_calibration_formula(self):
        cal = [json.loads(x)["score"] for x in self.run_cli("score", "model.rdm", "cal.csv").stdout.splitlines()]
        rows = [json.loads(x) for x in self.run_cli("score", "model.rdm", "test.csv").stdout.splitlines()]
        for row in rows:
            self.assertEqual(row["p_value"], (1 + sum(s >= row["score"] for s in cal)) / (1 + len(cal)))

    def test_drift_matches_reference_and_detects_shift(self):
        same = json.loads(self.run_cli("drift", "model.rdm", "cal.csv").stdout)
        self.assertTrue(all(f["ks_distance"] == 0 for f in same["features"]))
        self.write_csv("shift.csv", [(str(i), 50 + i, 50 + i) for i in range(50)])
        shifted = json.loads(self.run_cli("drift", "model.rdm", "shift.csv").stdout)
        self.assertTrue(all(f["ks_distance"] == 1 for f in shifted["features"]))

    def test_schema_order_and_duplicate_names(self):
        self.write_csv("bad.csv", [("a", 1, 2)], ("id", "b", "a"))
        p = self.run_cli("score", "model.rdm", "bad.csv", success=False)
        self.assertEqual(p.stdout, "")
        self.assertIn("schema", p.stderr)
        self.write_csv("bad.csv", [("a", 1, 2)], ("id", "a", "a"))
        self.run_cli("score", "model.rdm", "bad.csv", success=False)

    def test_bad_csv_rejected_before_output(self):
        for text in ["id,a,b\nx,nan,1\n", "id,a,b\nx,1e999,1\n", "id,a,b\nx,0x10,1\n", "id,a,b\nx,1,\n", "id,a,b\nx,1,2,3\n", 'id,a,b\n"oops,1,2\n', "id,a,b\nx,1,2\nbad,1,no\n", "id,a,b\nx,1,2\x00\n", "id,a,b\nx,1,2\n\n", "id,a,b\n" + "x" * 33000 + ",1,2\n"]:
            with self.subTest(text=text[:30]):
                (self.root / "bad.csv").write_text(text, encoding="ascii")
                self.assertEqual(self.run_cli("score", "model.rdm", "bad.csv", success=False).stdout, "")

    def test_no_overwrite_and_no_calibration_reuse(self):
        original = (self.root / "model.rdm").read_bytes()
        self.run_cli("fit", "train.csv", "cal.csv", "model.rdm", success=False)
        self.assertEqual(original, (self.root / "model.rdm").read_bytes())
        self.run_cli("fit", "train.csv", "train.csv", "bad.rdm", success=False)
        self.assertFalse((self.root / "bad.rdm").exists())

    def test_seed_reproducible(self):
        self.run_cli("fit", "train.csv", "cal.csv", "second.rdm", "--trees", "32")
        self.assertEqual((self.root / "model.rdm").read_bytes(), (self.root / "second.rdm").read_bytes())

    def test_options_and_resolution(self):
        for options in [("--alpha", "nan"), ("--alpha", "1"), ("--alpha", "0"), ("--oops", "1"), ("--alpha",)]:
            self.run_cli("score", "model.rdm", "test.csv", *options, success=False)
        result = self.run_cli("score", "model.rdm", "test.csv", "--alpha", "0.0001")
        self.assertIn("resolution", result.stderr)
        self.assertFalse(any(json.loads(s)["alert"] for s in result.stdout.splitlines()))
        for options in [("--sample", "0"), ("--trees", "999"), ("--seed", "-1"), ("--seed", "1", "--seed", "2")]:
            self.run_cli("fit", "train.csv", "cal.csv", "bad.rdm", *options, success=False)

    def test_corrupt_models(self):
        model = (self.root / "model.rdm").read_bytes()
        variants = [model[:n] for n in (0, 7, 16, 32, 100, len(model) - 1)]
        variants += [model + b"trailing"]
        broken = bytearray(model); broken[90] ^= 8; variants.append(broken)
        rng = random.Random(123)
        for _ in range(40):
            broken = bytearray(model); broken[rng.randrange(len(broken) - 4)] ^= 1 << rng.randrange(8)
            variants.append(broken)
        # Valid CRC cannot excuse impossible allocation sizes or cyclic tree links.
        for offset, value in [(8, 0xFFFFFFFF), (12, 0xFFFFFFFF), (16, 0), (20, 0xFFFFFFFF)]:
            broken = bytearray(model); struct.pack_into("<I", broken, offset, value)
            struct.pack_into("<I", broken, len(broken)-4, zlib.crc32(broken[:-4]))
            variants.append(broken)
        first_tree = 32 + 2 * (49 + 8) + 128 * 8 * 3
        broken = bytearray(model)
        struct.pack_into("<I", broken, first_tree + 4 + 4, 0)  # root.left -> root
        struct.pack_into("<I", broken, len(broken)-4, zlib.crc32(broken[:-4]))
        variants.append(broken)
        for n, payload in enumerate(variants):
            with self.subTest(n=n):
                (self.root / "bad.rdm").write_bytes(payload)
                self.assertEqual(self.run_cli("score", "bad.rdm", "test.csv", success=False).stdout, "")

    def test_zeek_reordered_header_and_missing_measurements(self):
        log = "#separator \\x09\n#path\tconn\n#unset_field\t-\n#fields\tproto\tuid\tresp_bytes\tduration\torig_bytes\tresp_pkts\torig_pkts\tmissed_bytes\tconn_state\n"
        log += "tcp\tCexample\t900\t2\t100\t9\t1\t0\tSF\n"
        log += "udp\tCmissing\t-\t-\t-\t0\t0\t0\tS0\n"
        (self.root / "conn.log").write_text(log, encoding="ascii")
        import io
        out = list(csv.DictReader(io.StringIO(self.run_cli("zeek", "conn.log").stdout)))
        self.assertAlmostEqual(float(out[0]["log_duration"]), math.log1p(2))
        self.assertAlmostEqual(float(out[0]["orig_byte_ratio"]), .1)
        self.assertEqual(out[1]["missing_measurements"], "3")
        self.assertEqual(out[1]["is_failed"], "1")
        self.assertNotIn("id.orig_h", out[0])
        for bad in [log.replace("\\x09", "\\x20"), log.replace("#path\tconn", "#path\tdns"), log.replace("900\t2", "900\t-2"), log.replace("900\t2", "nan\t2"), log.replace("\tSF\n", "\tUNKNOWN\n"), log.replace("#fields\tproto", "#fields\twrong"), log.replace("\t9\t1\t", "\t1.5\t1\t")]:
            (self.root / "bad.log").write_text(bad, encoding="ascii")
            self.assertEqual(self.run_cli("zeek", "bad.log", success=False).stdout, "")
        (self.root / "bad.log").write_text(log.replace("900\t2", "900\t-2"), encoding="ascii")
        self.assertIn("in duration", self.run_cli("zeek", "bad.log", success=False).stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
