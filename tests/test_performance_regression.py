import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "analysis"))

from performance_regression import lower_cusum, lower_ewma


class PerformanceRegressionTests(unittest.TestCase):
    def test_cusum_detects_sustained_drop(self):
        values = [100.0, 101.0, 99.0, 100.5, 99.5] * 6 + [90.0] * 20
        result = lower_cusum(values, calibration_batches=20)
        self.assertTrue(any(index >= 30 for index in result.alarms))

    def test_ewma_detects_sustained_drop(self):
        values = [100.0, 101.0, 99.0, 100.5, 99.5] * 6 + [90.0] * 20
        result = lower_ewma(values, calibration_batches=20)
        self.assertTrue(any(index >= 30 for index in result.alarms))

    def test_detectors_do_not_read_future_values(self):
        prefix = [100.0, 101.0, 99.0, 100.5, 99.5] * 8
        first_suffix = [90.0] * 10
        second_suffix = [10.0] * 10
        first_cusum = lower_cusum(prefix + first_suffix, calibration_batches=20)
        second_cusum = lower_cusum(prefix + second_suffix, calibration_batches=20)
        first_ewma = lower_ewma(prefix + first_suffix, calibration_batches=20)
        second_ewma = lower_ewma(prefix + second_suffix, calibration_batches=20)
        self.assertEqual(first_cusum.scores[: len(prefix)], second_cusum.scores[: len(prefix)])
        self.assertEqual(first_ewma.scores[: len(prefix)], second_ewma.scores[: len(prefix)])


if __name__ == "__main__":
    unittest.main()
