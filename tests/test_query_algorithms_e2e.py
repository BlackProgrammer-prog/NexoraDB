"""End-to-end correctness tests for every NexoraQL graph algorithm.

Unlike the parser/dispatch tests, this module uses the compiled NexoraDB
engine.  Every algorithm is invoked from NexoraQL text and its mathematical
result is checked against a small graph with known answers.

Reference graph (edge storage is directed):

    a -> b <- c -> d       z

Most social algorithms intentionally treat edges as undirected, so their
projected topology is the path ``a--b--c--d`` plus isolated node ``z``.
AllDistances follows outgoing edges and is checked with directed expectations.
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "NexoraDB" / "src"
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
class QueryAlgorithmCorrectnessTests(unittest.TestCase):
    """Run all 12 algorithms through ``Executor.execute_text``."""

    @classmethod
    def setUpClass(cls) -> None:
        cls._temp_dir = tempfile.TemporaryDirectory(prefix="nexora-query-algos-")
        temp_root = Path(cls._temp_dir.name)

        cls.engine = nexoradb.DocEngine(str(temp_root / "database"))
        cls.graph_manager = nexoradb.GraphManager(cls.engine, str(temp_root / "graphs"))
        if not cls.graph_manager.startup():
            raise RuntimeError("GraphManager failed to start")

        cls.executor = Executor(cls.engine, cls.graph_manager)
        setup_results = cls.executor.execute_text(
            """
            CREATE COLLECTION users;
            CREATE COLLECTION follows;

            INSERT INTO users BATCH VALUES
                ('{"_id":"a"}'),
                ('{"_id":"b"}'),
                ('{"_id":"c"}'),
                ('{"_id":"d"}'),
                ('{"_id":"z"}');

            INSERT INTO follows BATCH VALUES
                ('{"_id":"e1","from_id":"a","to_id":"b"}'),
                ('{"_id":"e2","from_id":"c","to_id":"b"}'),
                ('{"_id":"e3","from_id":"c","to_id":"d"}');

            CREATE LIVE GRAPH social HETEROGENEOUS DIRECTED;
            MAP NODE User FROM users KEY _id;
            MAP EDGE FOLLOWS FROM follows
                SOURCE from_id AS User
                TARGET to_id AS User
                DIRECTED;
            BUILD GRAPH social;
            """
        )

        failures = [result for result in setup_results if not result.get("success")]
        if failures:
            raise AssertionError(f"NexoraQL graph setup failed: {failures}")

        build = setup_results[-1]
        if (build["nodes_built"], build["edges_built"]) != (5, 3):
            raise AssertionError(f"Unexpected reference graph: {build}")

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

        job_id = submitted[0]["job_id"]
        completed = self.executor.execute_text(f"JOB RESULT '{job_id}';")
        self.assertEqual(len(completed), 1, completed)
        self.assertTrue(completed[0]["success"], completed[0])
        return completed[0]["result"]

    def test_get_friends_returns_both_incoming_and_outgoing_neighbors(self) -> None:
        result = self.run_query(
            "RUN LOCK GetFriends ON social WITH user='b', edge_type='FOLLOWS' LIMIT 10;"
        )

        self.assertEqual(result["user_id"], "b")
        self.assertEqual(result["friend_count"], 2)
        self.assertEqual(set(result["friends"]), {"a", "c"})
        self.assertFalse(result["limit_applied"])

    def test_are_connected_reports_exact_hops_and_disconnection(self) -> None:
        connected = self.run_query(
            "RUN LOCK AreConnected ON social "
            "WITH user1='a', user2='d', edge_type='FOLLOWS';"
        )
        disconnected = self.run_query(
            "RUN LOCK AreConnected ON social "
            "WITH user1='a', user2='z', edge_type='FOLLOWS';"
        )

        self.assertEqual(connected["connected"], True)
        self.assertEqual(connected["hops"], 3)
        self.assertEqual(disconnected["connected"], False)
        self.assertEqual(disconnected["hops"], -1)

    def test_shortest_path_reconstructs_the_only_shortest_route(self) -> None:
        path = self.run_query(
            "RUN LOCK ShortestPath ON social "
            "WITH from='a', to='d', edge_type='FOLLOWS';"
        )
        missing = self.run_query(
            "RUN LOCK ShortestPath ON social "
            "WITH from='a', to='z', edge_type='FOLLOWS';"
        )

        self.assertTrue(path["found"])
        self.assertEqual(path["hops"], 3)
        self.assertEqual(path["path"], ["a", "b", "c", "d"])
        self.assertFalse(missing["found"])
        self.assertEqual(missing["path"], [])

    def test_mutual_friends_intersects_filtered_out_neighbors(self) -> None:
        result = self.run_query(
            "RUN LOCK MutualFriends ON social "
            "WITH user1='a', user2='c', edge_type='FOLLOWS';"
        )

        self.assertEqual(result["count"], 1)
        self.assertEqual(result["mutual_friends"], ["b"])

    def test_friend_suggestion_scores_friend_of_friend(self) -> None:
        result = self.run_query(
            "RUN LOCK FriendSuggestion ON social "
            "WITH user='a', edge_type='FOLLOWS' LIMIT 10;"
        )

        self.assertEqual(result["suggestion_count"], 1)
        self.assertEqual(
            result["suggestions"],
            [{"user_id": "c", "mutual_friends": 1}],
        )

    def test_most_connected_reports_cached_degrees(self) -> None:
        result = self.run_query(
            "RUN LOCK MostConnected ON social "
            "WITH metric='total', node_type='User' LIMIT 5;"
        )
        by_id = {item["id"]: item for item in result["results"]}

        self.assertEqual(result["metric"], "total")
        self.assertEqual(result["total_scanned"], 5)
        self.assertEqual(
            {node_id: item["total"] for node_id, item in by_id.items()},
            {"a": 1, "b": 2, "c": 2, "d": 1, "z": 0},
        )
        self.assertEqual((by_id["b"]["in"], by_id["b"]["out"]), (2, 0))
        self.assertEqual((by_id["c"]["in"], by_id["c"]["out"]), (0, 2))

    def test_network_stats_matches_reference_graph(self) -> None:
        result = self.run_query("RUN LOCK NetworkStats ON social WITH mode='full';")

        self.assertEqual(result["basic"]["active_nodes"], 5)
        self.assertEqual(result["basic"]["active_edges"], 3)
        self.assertEqual(result["degree"]["isolated_nodes"], 1)
        self.assertEqual(result["node_types"], {"User": 5})
        self.assertEqual(result["edge_types"], {"FOLLOWS": 3})
        self.assertAlmostEqual(result["density"], 0.15)

    def test_connected_components_finds_path_and_isolated_node(self) -> None:
        result = self.run_job(
            "RUN JOB ConnectedComponents ON social WITH node_type='User';"
        )
        components = {frozenset(item["members"]) for item in result["components"]}

        self.assertEqual(result["total_components"], 2)
        self.assertEqual(result["total_nodes"], 5)
        self.assertEqual(result["largest_component_size"], 4)
        self.assertEqual(
            components, {frozenset({"a", "b", "c", "d"}), frozenset({"z"})}
        )

    def test_all_distances_sssp_respects_direction_and_max_hops(self) -> None:
        result = self.run_job(
            "RUN JOB AllDistances ON social "
            "WITH source='c', max_hops=1, node_type='User';"
        )
        distances = {item["id"]: item["distance"] for item in result["distances"]}

        self.assertEqual(result["mode"], "sssp")
        self.assertEqual(result["source"], "c")
        self.assertEqual(result["max_hops"], 1)
        self.assertEqual(distances, {"b": 1, "d": 1})
        self.assertEqual(result["reached"], 2)
        self.assertEqual(result["unreachable"], 2)

    def test_all_distances_all_pairs_uses_outgoing_paths(self) -> None:
        result = self.run_job(
            "RUN JOB AllDistances ON social "
            "WITH source='a', all=true, max_hops=10, node_type='User';"
        )
        by_source = {item["source"]: item for item in result["sources"]}

        self.assertEqual(result["mode"], "all_pairs")
        self.assertEqual(result["node_count"], 5)
        self.assertEqual(by_source["a"]["distances"], [{"id": "b", "distance": 1}])
        self.assertEqual(by_source["b"]["distances"], [])
        self.assertEqual(
            by_source["c"]["distances"],
            [{"id": "b", "distance": 1}, {"id": "d", "distance": 1}],
        )

    def test_betweenness_centrality_matches_brandes_known_values(self) -> None:
        result = self.run_job("RUN JOB BetweennessCentrality ON social RETURNS TOP 5;")
        scores = {item["user_id"]: item["betweenness"] for item in result["nodes"]}

        self.assertEqual(result["total_nodes"], 5)
        self.assertEqual(result["showing"], 5)
        self.assertAlmostEqual(scores["b"], 1.0 / 3.0, places=6)
        self.assertAlmostEqual(scores["c"], 1.0 / 3.0, places=6)
        self.assertEqual(scores["a"], 0.0)
        self.assertEqual(scores["d"], 0.0)
        self.assertEqual(scores["z"], 0.0)

    def test_community_detection_groups_path_and_excludes_isolated_node(self) -> None:
        result = self.run_job(
            "RUN JOB CommunityDetection ON social "
            "WITH max_iterations=20, min_community_size=2, "
            "members=true, node_type='User';"
        )

        self.assertEqual(result["algorithm"], "label_propagation")
        self.assertEqual(result["total_communities"], 1)
        self.assertEqual(result["total_nodes_assigned"], 4)
        self.assertEqual(result["isolated_nodes"], 1)
        self.assertEqual(result["communities"][0]["size"], 4)
        self.assertEqual(set(result["communities"][0]["members"]), {"a", "b", "c", "d"})

    def test_influence_maximization_reaches_entire_connected_component(self) -> None:
        result = self.run_job(
            "RUN JOB InfluenceMaximization ON social "
            "WITH k=1, simulations=3, probability=1.0;"
        )

        self.assertEqual(result["k_seeds"], 1)
        self.assertEqual(result["simulations"], 3)
        self.assertEqual(result["propagation_prob"], 1.0)
        self.assertEqual(result["estimated_reach"], 4.0)
        self.assertEqual(result["reach_percentage"], 80.0)
        self.assertIn(result["seeds"][0]["user_id"], {"a", "b", "c", "d"})
        self.assertEqual(result["seeds"][0]["marginal_gain"], 4.0)
        self.assertEqual(result["seeds"][0]["selection_order"], 1)


if __name__ == "__main__":
    unittest.main()
