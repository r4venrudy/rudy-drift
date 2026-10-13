import pathlib
import sys
import unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "benchmarks"))
from evaluate import metrics


class Metrics(unittest.TestCase):
    def test_auroc_perfect_reverse_and_ties(self):
        for scores, expected in [([0, 1], 1), ([1, 0], 0), ([1, 1], .5)]:
            result = metrics(scores, [.5, .01], [0, 1], ["normal", "attack"], .05)
            self.assertEqual(result["auroc"], expected)

    def test_mixed_ties_and_confusion(self):
        result = metrics([1, 2, 2, 3], [.01, .5, .03, .8], [0, 0, 1, 1], ["normal", "normal", "attack", "attack"], .05)
        self.assertEqual(result["auroc"], .875)
        self.assertEqual(result["true_positive_rate"], .5)
        self.assertEqual(result["false_positive_rate"], .5)
        self.assertEqual(result["precision"], .5)
        self.assertEqual(result["alerts"], 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
