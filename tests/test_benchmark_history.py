import copy
import importlib.util
from pathlib import Path
import tempfile
import unittest
SPEC = importlib.util.spec_from_file_location("benchmark_history", Path(__file__).resolve().parents[1] / "scripts/benchmark_history.py")
bench = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bench)


class BenchmarkGate(unittest.TestCase):
    def sample(self):
        return dict(identity={"host": "fixed"}, config=["same"], samples={"baseline/A": {
            "ops_s": [10000, 10100, 9900], "read_p99_us": [100, 101, 99], "write_p99_us": [200, 201, 199]}})

    def test_rejects_meaningful_drop_and_latency_growth(self):
        baseline = self.sample()
        current = copy.deepcopy(baseline)
        current["samples"]["baseline/A"]["ops_s"] = [7000, 7100, 7200]
        current["samples"]["baseline/A"]["write_p99_us"] = [300, 301, 302]
        self.assertEqual(len(bench.compare(current, baseline)), 2)

    def test_ignores_overlapping_noise_and_rejects_incompatible_host(self):
        baseline = self.sample()
        current = copy.deepcopy(baseline)
        current["samples"]["baseline/A"]["ops_s"] = [7000, 12000, 7500]
        self.assertEqual(bench.compare(current, baseline), [])
        current["identity"] = {"host": "another"}
        with self.assertRaises(ValueError):
            bench.compare(current, baseline)
        current = self.sample()
        current["samples"] = {}
        with self.assertRaises(ValueError):
            bench.compare(current, baseline)

    def test_rejects_errors_and_insufficient_samples_and_renders_chart(self):
        row = dict(profile="baseline", workload="A", errors=1, ops_s=1000, read_p99_us=10, write_p99_us=20)
        with self.assertRaises(ValueError):
            bench.summarize([row] * 3)
        row["errors"] = 0
        with self.assertRaises(ValueError):
            bench.summarize([row])
        record = self.sample()
        record["at"] = "2026-10-21T00:00:00Z"
        self.assertIn("<svg", bench.report_history([record]))


if __name__ == "__main__":
    unittest.main()
