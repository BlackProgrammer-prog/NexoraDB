"""End-to-end correctness tests for all graph algorithms on the professor dataset.

The import represents every undirected friendship as two directed FOLLOWS
edges. Expected results were calculated independently from the 389 unique
pairs in the supplied dataset, rather than copied from NexoraDB output.
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "NexoraDB" / "src"
IMPORT_QUERY = ROOT / "professor_social_import.nql"
sys.path.insert(0, str(SRC))

import nexoradb  # noqa: E402
from nexoradb.parser.semantic import Executor  # noqa: E402


NATIVE_GRAPH_AVAILABLE = (
    hasattr(nexoradb, "DocEngine")
    and hasattr(nexoradb, "GraphManager")
    and getattr(nexoradb, "GRAPH_ENABLED", False)
)


@unittest.skipUnless(
    NATIVE_GRAPH_AVAILABLE,
    "compiled NexoraDB graph extension is required for this integration test",
)
class ProfessorDatasetAlgorithmTests(unittest.TestCase):
    """Run all 12 NexoraQL algorithms against the 100-user dataset."""

    @classmethod
    def setUpClass(cls) -> None:
        cls._temp_dir = tempfile.TemporaryDirectory(prefix="nexora-professor-algos-")
        temp_root = Path(cls._temp_dir.name)

        cls.engine = nexoradb.DocEngine(str(temp_root / "database"))
        cls.graph_manager = nexoradb.GraphManager(
            cls.engine, str(temp_root / "graphs")
        )
        if not cls.graph_manager.startup():
            raise RuntimeError("GraphManager failed to start")

        cls.executor = Executor(cls.engine, cls.graph_manager)
        setup_results = cls.executor.execute_text(
            IMPORT_QUERY.read_text(encoding="utf-8")
        )
        failures = [result for result in setup_results if not result.get("success")]
        if failures:
            raise AssertionError(f"Professor dataset import failed: {failures}")

        build = next(
            result for result in setup_results if "nodes_built" in result
        )
        if (build["nodes_built"], build["edges_built"]) != (100, 778):
            raise AssertionError(f"Unexpected professor graph build: {build}")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.graph_manager.shutdown()
        cls.executor = None
        cls.graph_manager = None
        cls.engine = None
        cls._temp_dir.cleanup()

    def run_query(self, query: str) -> dict:
        results = self.executor.execute_text(query)
        self.assertEqual(len(results), 1, results)
        self.assertTrue(results[0]["success"], results[0])
        return results[0]["result"]

    def run_job(self, query: str) -> dict:
        submitted = self.executor.execute_text(query)
        self.assertEqual(len(submitted), 1, submitted)
        self.assertTrue(submitted[0]["success"], submitted[0])
        self.assertEqual(submitted[0]["status"], "done", submitted[0])

        completed = self.executor.execute_text(
            f"JOB RESULT '{submitted[0]['job_id']}';"
        )
        self.assertEqual(len(completed), 1, completed)
        self.assertTrue(completed[0]["success"], completed[0])
        return completed[0]["result"]

    def test_get_friends_returns_unique_bidirectional_neighbors(self) -> None:
        result = self.run_query(
            "RUN LOCK GetFriends ON professor_social "
            "WITH user='O3', edge_type='FOLLOWS' LIMIT 100;"
        )

        self.assertEqual(result["user_id"], "O3")
        self.assertEqual(result["friend_count"], 16)
        self.assertEqual(
            set(result["friends"]),
            {
                "F3", "G2", "I2", "L", "M1", "O", "P2", "Q1",
                "R3", "U", "U2", "U3", "V3", "W1", "Y", "Z",
            },
        )
        self.assertFalse(result["limit_applied"])

    def test_are_connected_matches_reference_diameter_pair(self) -> None:
        result = self.run_query(
            "RUN LOCK AreConnected ON professor_social "
            "WITH user1='Z1', user2='B1', edge_type='FOLLOWS';"
        )

        self.assertTrue(result["connected"])
        self.assertEqual(result["hops"], 4)

    def test_shortest_path_returns_four_hop_route(self) -> None:
        result = self.run_query(
            "RUN LOCK ShortestPath ON professor_social "
            "WITH from='Z1', to='B1', edge_type='FOLLOWS';"
        )

        self.assertTrue(result["found"])
        self.assertEqual(result["hops"], 4)
        self.assertEqual(result["path"], ["Z1", "E1", "C3", "H1", "B1"])

    def test_mutual_friends_returns_distinct_users(self) -> None:
        result = self.run_query(
            "RUN LOCK MutualFriends ON professor_social "
            "WITH user1='S2', user2='X2', edge_type='FOLLOWS';"
        )

        self.assertEqual(result["count"], 5)
        self.assertEqual(
            set(result["mutual_friends"]), {"A3", "E3", "M1", "Q3", "V"}
        )

    def test_friend_suggestion_counts_each_mutual_friend_once(self) -> None:
        result = self.run_query(
            "RUN LOCK FriendSuggestion ON professor_social "
            "WITH user='W2', edge_type='FOLLOWS' LIMIT 3;"
        )

        self.assertEqual(
            result["suggestions"],
            [
                {"user_id": "O3", "mutual_friends": 4},
                {"user_id": "T3", "mutual_friends": 4},
                {"user_id": "Y", "mutual_friends": 3},
            ],
        )

    def test_most_connected_matches_reference_degrees(self) -> None:
        result = self.run_query(
            "RUN LOCK MostConnected ON professor_social "
            "WITH metric='total', node_type='User' LIMIT 5;"
        )

        self.assertEqual(result["total_scanned"], 100)
        self.assertEqual(
            [(item["id"], item["in"], item["out"], item["total"]) for item in result["results"]],
            [
                ("O3", 16, 16, 32),
                ("N", 15, 15, 30),
                ("S2", 14, 14, 28),
                ("J3", 14, 14, 28),
                ("C", 13, 13, 26),
            ],
        )

    def test_network_stats_matches_imported_topology(self) -> None:
        result = self.run_query(
            "RUN LOCK NetworkStats ON professor_social WITH mode='full';"
        )

        self.assertEqual(result["basic"]["active_nodes"], 100)
        self.assertEqual(result["basic"]["active_edges"], 778)
        self.assertEqual(result["degree"]["isolated_nodes"], 0)
        self.assertEqual(result["degree"]["in"]["max"], 16)
        self.assertEqual(result["degree"]["out"]["max"], 16)
        self.assertAlmostEqual(result["degree"]["in"]["avg"], 7.78)
        self.assertEqual(result["node_types"], {"User": 100})
        self.assertEqual(result["edge_types"], {"FOLLOWS": 778})
        self.assertAlmostEqual(result["density"], 0.078586, places=6)

    def test_connected_components_finds_one_component(self) -> None:
        result = self.run_job(
            "RUN JOB ConnectedComponents ON professor_social "
            "WITH node_type='User';"
        )

        self.assertEqual(result["total_components"], 1)
        self.assertEqual(result["total_nodes"], 100)
        self.assertEqual(result["largest_component_size"], 100)
        self.assertEqual(result["components"][0]["size"], 100)

    def test_all_distances_reaches_every_other_user(self) -> None:
        result = self.run_job(
            "RUN JOB AllDistances ON professor_social "
            "WITH source='A', node_type='User';"
        )
        distance_counts = Counter(
            item["distance"] for item in result["distances"]
        )
        by_id = {item["id"]: item["distance"] for item in result["distances"]}

        self.assertEqual(result["mode"], "sssp")
        self.assertEqual(result["source"], "A")
        self.assertEqual(result["max_hops"], -1)
        self.assertEqual(result["reached"], 99)
        self.assertEqual(result["unreachable"], 0)
        self.assertEqual(result["max_distance_found"], 4)
        self.assertEqual(distance_counts, {1: 4, 2: 20, 3: 61, 4: 14})
        self.assertEqual(
            {node_id for node_id, distance in by_id.items() if distance == 1},
            {"C1", "E1", "L3", "P3"},
        )

    def test_betweenness_centrality_matches_independent_brandes_result(self) -> None:
        result = self.run_job(
            "RUN JOB BetweennessCentrality ON professor_social RETURNS TOP 5;"
        )

        self.assertEqual(result["total_nodes"], 100)
        self.assertEqual(result["showing"], 5)
        self.assertEqual(
            [item["user_id"] for item in result["nodes"]],
            ["O3", "N", "G2", "C", "J3"],
        )
        expected = [0.051043, 0.048241, 0.042476, 0.041788, 0.036483]
        for item, expected_score in zip(result["nodes"], expected, strict=True):
            self.assertAlmostEqual(item["betweenness"], expected_score, places=6)

    def test_community_detection_assigns_all_users(self) -> None:
        result = self.run_job(
            "RUN JOB CommunityDetection ON professor_social "
            "WITH max_iterations=30, min_community_size=2, "
            "members=true, node_type='User';"
        )

        self.assertEqual(result["algorithm"], "label_propagation")
        self.assertEqual(result["total_communities"], 1)
        self.assertEqual(result["total_nodes_assigned"], 100)
        self.assertEqual(result["isolated_nodes"], 0)
        self.assertEqual(result["communities"][0]["size"], 100)
        self.assertEqual(len(result["communities"][0]["members"]), 100)

    def test_influence_maximization_reaches_entire_graph(self) -> None:
        result = self.run_job(
            "RUN JOB InfluenceMaximization ON professor_social "
            "WITH k=1, simulations=3, probability=1.0;"
        )

        self.assertEqual(result["total_nodes"], 100)
        self.assertEqual(result["k_seeds"], 1)
        self.assertEqual(result["simulations"], 3)
        self.assertEqual(result["propagation_prob"], 1.0)
        self.assertEqual(result["estimated_reach"], 100.0)
        self.assertEqual(result["reach_percentage"], 100.0)
        self.assertEqual(result["seeds"][0]["user_id"], "O3")
        self.assertEqual(result["seeds"][0]["marginal_gain"], 100.0)
        self.assertEqual(result["seeds"][0]["selection_order"], 1)


if __name__ == "__main__":
    unittest.main()
