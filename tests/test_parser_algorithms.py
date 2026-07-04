"""Parser/semantic coverage for the graph algorithms exposed by current pybind."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "NexoraDB" / "src"
sys.path.insert(0, str(SRC))

from nexoradb.parser import parse_one  # noqa: E402
from nexoradb.parser import ast_nodes as N  # noqa: E402
from nexoradb.parser.errors import NexoraQLSemanticError  # noqa: E402
from nexoradb.parser.semantic import (  # noqa: E402
    Executor,
    Validator,
    algo_params_to_positional,
)


class FakeAlgoResult:
    def __init__(self, method: str, graph: str, params: list[str]):
        self.success = True
        self.error_msg = ""
        self.elapsed_ms = 1.25
        self.result_json = json.dumps({
            "method": method,
            "graph": graph,
            "params": params,
        })


class FakeGraphManager:
    def __init__(self):
        self.calls: list[tuple[str, str, list[str]]] = []

    def _record(self, method: str, graph: str, params: list[str]) -> FakeAlgoResult:
        self.calls.append((method, graph, params))
        return FakeAlgoResult(method, graph, params)

    def run_mutual_friends(self, graph: str, params: list[str]) -> FakeAlgoResult:
        return self._record("run_mutual_friends", graph, params)

    def run_most_connected(self, graph: str, params: list[str]) -> FakeAlgoResult:
        return self._record("run_most_connected", graph, params)

    def run_network_stats(self, graph: str, params: list[str]) -> FakeAlgoResult:
        return self._record("run_network_stats", graph, params)

    def run_connected_components(self, graph: str, params: list[str]) -> FakeAlgoResult:
        return self._record("run_connected_components", graph, params)

    def run_community_detection(self, graph: str, params: list[str]) -> FakeAlgoResult:
        return self._record("run_community_detection", graph, params)

    def run_all_distances(self, graph: str, params: list[str]) -> FakeAlgoResult:
        return self._record("run_all_distances", graph, params)


class ParserAlgorithmTests(unittest.TestCase):
    def test_parses_current_cpp_algorithm_statements(self):
        cases = [
            (
                "RUN LOCK MutualFriends ON social "
                "WITH user1='u1', user2='u2', edge_type='FOLLOWS';",
                N.RunLock,
                "MutualFriends",
                {"user1": "u1", "user2": "u2", "edge_type": "FOLLOWS"},
            ),
            (
                "RUN LOCK MostConnected ON social "
                "WITH metric='out', node_type='User' LIMIT 2;",
                N.RunLock,
                "MostConnected",
                {"metric": "out", "node_type": "User"},
            ),
            (
                "RUN LOCK NetworkStats ON social WITH mode='full';",
                N.RunLock,
                "NetworkStats",
                {"mode": "full"},
            ),
            (
                "RUN JOB ConnectedComponents ON social WITH node_type='User';",
                N.RunJob,
                "ConnectedComponents",
                {"node_type": "User"},
            ),
            (
                "RUN JOB CommunityDetection ON social "
                "WITH max_iterations=10, min_community_size=2, "
                "members=true, node_type='User';",
                N.RunJob,
                "CommunityDetection",
                {
                    "max_iterations": 10,
                    "min_community_size": 2,
                    "members": True,
                    "node_type": "User",
                },
            ),
            (
                "RUN JOB AllDistances ON social "
                "WITH source='u1', all=true, max_hops=2, node_type='User';",
                N.RunJob,
                "AllDistances",
                {"source": "u1", "all": True, "max_hops": 2, "node_type": "User"},
            ),
        ]

        for query, expected_type, algo, params in cases:
            with self.subTest(algo=algo):
                stmt = parse_one(query)
                self.assertIsInstance(stmt, expected_type)
                self.assertEqual(stmt.algo, algo)
                self.assertEqual(stmt.graph, "social")
                self.assertEqual(stmt.params, params)

    def test_params_follow_cpp_order_for_current_algorithms(self):
        self.assertEqual(
            algo_params_to_positional(
                "MutualFriends",
                {"user2": "u2", "edge_type": "FOLLOWS", "user1": "u1"},
            ),
            ["u1", "u2", "FOLLOWS"],
        )
        self.assertEqual(
            algo_params_to_positional(
                "MostConnected",
                {"metric": "out", "node_type": "User"},
                limit=2,
            ),
            ["2", "out", "User"],
        )
        self.assertEqual(
            algo_params_to_positional("NetworkStats", {"mode": "full"}),
            ["full"],
        )
        self.assertEqual(
            algo_params_to_positional("ConnectedComponents", {"node_type": "User"}),
            ["User"],
        )
        self.assertEqual(
            algo_params_to_positional(
                "CommunityDetection",
                {
                    "max_iterations": 10,
                    "min_community_size": 2,
                    "members": True,
                    "node_type": "User",
                },
            ),
            ["10", "2", "members", "User"],
        )
        self.assertEqual(
            algo_params_to_positional(
                "AllDistances",
                {"source": "u1", "all": True, "max_hops": 2, "node_type": "User"},
            ),
            ["u1", "all", "2", "User"],
        )

    def test_validator_rejects_wrong_run_mode(self):
        validator = Validator()

        with self.assertRaises(NexoraQLSemanticError):
            validator.validate(parse_one("RUN JOB MostConnected ON social;"))

        with self.assertRaises(NexoraQLSemanticError):
            validator.validate(parse_one("RUN LOCK ConnectedComponents ON social;"))

    def test_executor_dispatches_lock_algorithms_to_current_pybind_methods(self):
        gm = FakeGraphManager()
        executor = Executor(engine=None, graph_manager=gm)

        result = executor.execute(parse_one(
            "RUN LOCK MostConnected ON social "
            "WITH metric='out', node_type='User' LIMIT 2;"
        ))

        self.assertTrue(result["success"])
        self.assertEqual(
            gm.calls,
            [("run_most_connected", "social", ["2", "out", "User"])],
        )
        self.assertEqual(result["result"]["method"], "run_most_connected")

    def test_executor_dispatches_job_algorithms_to_current_pybind_methods(self):
        gm = FakeGraphManager()
        executor = Executor(engine=None, graph_manager=gm)

        submitted = executor.execute(parse_one(
            "RUN JOB AllDistances ON social "
            "WITH source='u1', all=true, max_hops=2, node_type='User';"
        ))
        result = executor.execute(N.JobResult(job_id=submitted["job_id"]))

        self.assertTrue(submitted["success"])
        self.assertEqual(submitted["status"], "done")
        self.assertEqual(
            gm.calls,
            [("run_all_distances", "social", ["u1", "all", "2", "User"])],
        )
        self.assertTrue(result["success"])
        self.assertEqual(result["result"]["method"], "run_all_distances")


if __name__ == "__main__":
    unittest.main()
