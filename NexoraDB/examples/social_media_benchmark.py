#!/usr/bin/env python3
# ruff: noqa: E501 -- NexoraQL/schema literals are intentionally kept readable as single lines.
"""End-to-end NexoraQL benchmark with generated social-media data.

The benchmark talks to the public NexoraDB Python driver/API, creates uniquely
named temporary collections and a graph, exercises document queries plus all
12 graph algorithms, prints an aggregated timing table, and always cleans up.

Example:
    NEXORADB_TOKEN=... python examples/social_media_benchmark.py --users 500
"""

from __future__ import annotations

import argparse
import json
import os
import random
import sys
import time
from collections import defaultdict
from dataclasses import dataclass
from datetime import timezone
from pathlib import Path
from typing import Any, Iterable

try:
    from faker import Faker
except ImportError as exc:  # pragma: no cover - friendly CLI failure
    raise SystemExit("Faker is required. Install it with: pip install Faker") from exc

# Allow running directly from a source checkout without installing the package.
SRC = Path(__file__).resolve().parents[1] / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

from nexoradb.api import connect  # noqa: E402
from nexoradb_admin.config import AdminApiSettings  # noqa: E402
from nexoradb_admin.native import load_native_module  # noqa: E402


def print_native_build_info() -> None:
    """Print metadata embedded in the local pybind11 C++ extension."""
    try:
        native = load_native_module(AdminApiSettings())
        getter = getattr(native, "build_info", None)
        if getter is None:
            print("Native C++ build info: unavailable (extension is older than build_info API)")
            return
        info = dict(getter())
    except Exception as exc:  # noqa: BLE001 - metadata must not abort a benchmark
        print(f"Native C++ build info: unavailable ({exc})")
        return

    print("Native C++ build info (pybind11):")
    print(json.dumps(info, ensure_ascii=False, sort_keys=True, indent=2))


@dataclass
class Sample:
    category: str
    name: str
    ok: bool
    client_ms: float
    server_ms: float
    rows: int
    error: str = ""


class Recorder:
    def __init__(self, client: Any) -> None:
        self.client = client
        self.samples: list[Sample] = []

    def query(self, category: str, name: str, query: str, *, critical: bool = True) -> Any:
        started = time.perf_counter()
        try:
            result = self.client.execute(query)
        except Exception as exc:  # noqa: BLE001 - benchmark records driver failures
            elapsed = (time.perf_counter() - started) * 1000
            self.samples.append(Sample(category, name, False, elapsed, 0.0, 0, str(exc)))
            if critical:
                raise
            return None

        elapsed = (time.perf_counter() - started) * 1000
        raw = result.raw if isinstance(result.raw, dict) else {}
        statements = raw.get("statements", [])
        semantic_ok = all(item.get("success", True) for item in statements)
        errors = [str(item.get("error")) for item in statements if item.get("error")]
        self.samples.append(
            Sample(
                category,
                name,
                semantic_ok,
                elapsed,
                float(result.execution_time_ms),
                len(result.rows),
                "; ".join(errors),
            )
        )
        if critical and not semantic_ok:
            raise RuntimeError(f"{name} failed: {'; '.join(errors) or 'success=false'}")
        return result


def quote(value: str) -> str:
    return "'" + value.replace("\\", "\\\\").replace("'", "\\'") + "'"


def json_value(document: dict[str, Any]) -> str:
    return quote(json.dumps(document, ensure_ascii=False, separators=(",", ":")))


def chunks(items: list[dict[str, Any]], size: int) -> Iterable[list[dict[str, Any]]]:
    for offset in range(0, len(items), size):
        yield items[offset : offset + size]


def insert_batches(
    recorder: Recorder,
    collection: str,
    documents: list[dict[str, Any]],
    batch_size: int,
) -> None:
    for batch in chunks(documents, batch_size):
        values = ",".join(f"({json_value(document)})" for document in batch)
        recorder.query(
            "load", f"INSERT {collection}", f"INSERT INTO {collection} BATCH VALUES {values};"
        )


def iso_time(fake: Faker) -> str:
    value = fake.date_time_between(start_date="-2y", end_date="now", tzinfo=timezone.utc)
    return value.isoformat().replace("+00:00", "Z")


def generate_data(args: argparse.Namespace) -> dict[str, list[dict[str, Any]]]:
    fake = Faker(args.locale)
    Faker.seed(args.seed)
    rng = random.Random(args.seed)

    users: list[dict[str, Any]] = []
    for index in range(args.users):
        users.append(
            {
                "_id": f"u{index:07d}",
                "username": f"{fake.user_name()}_{index}",
                "email": f"{index}.{fake.ascii_safe_email()}",
                "display_name": fake.name(),
                "age": rng.randint(13, 80),
                "active": rng.random() > 0.08,
                "country": fake.country_code(),
                "city": fake.city(),
                "bio": fake.text(max_nb_chars=180),
                "tags": fake.words(nb=rng.randint(1, 6), unique=True),
                "created_at": iso_time(fake),
            }
        )

    posts: list[dict[str, Any]] = []
    for index in range(args.posts):
        posts.append(
            {
                "_id": f"p{index:08d}",
                "author_id": rng.choice(users)["_id"],
                "title": fake.sentence(nb_words=8),
                "body": fake.paragraph(nb_sentences=rng.randint(2, 7)),
                "tags": fake.words(nb=rng.randint(1, 5), unique=True),
                "likes": rng.randint(0, 20_000),
                "views": rng.randint(0, 500_000),
                "published": rng.random() > 0.05,
                "created_at": iso_time(fake),
            }
        )

    comments: list[dict[str, Any]] = []
    for index in range(args.comments):
        comments.append(
            {
                "_id": f"c{index:09d}",
                "post_id": rng.choice(posts)["_id"],
                "user_id": rng.choice(users)["_id"],
                "text": fake.sentence(nb_words=rng.randint(4, 18)),
                "score": rng.randint(-10, 500),
                "created_at": iso_time(fake),
            }
        )

    max_edges = args.users * max(0, args.users - 1)
    requested_follows = min(max_edges, max(args.follows, args.users))
    edge_pairs = {(index, (index + 1) % args.users) for index in range(args.users)}
    if args.users >= 3:
        edge_pairs.update({(0, 2), (1, 2)})
    while len(edge_pairs) < requested_follows:
        source, target = rng.randrange(args.users), rng.randrange(args.users)
        if source != target:
            edge_pairs.add((source, target))
    follows = [
        {
            "_id": f"f{index:09d}",
            "from_id": users[source]["_id"],
            "to_id": users[target]["_id"],
            "weight": round(rng.uniform(0.1, 1.0), 4),
            "created_at": iso_time(fake),
        }
        for index, (source, target) in enumerate(sorted(edge_pairs))
    ]

    max_likes = args.users * args.posts
    like_pairs: set[tuple[int, int]] = set()
    while len(like_pairs) < min(args.likes, max_likes):
        like_pairs.add((rng.randrange(args.users), rng.randrange(args.posts)))
    likes = [
        {
            "_id": f"l{index:09d}",
            "user_id": users[user]["_id"],
            "post_id": posts[post]["_id"],
            "reaction": rng.choice(["like", "love", "insightful", "funny"]),
            "created_at": iso_time(fake),
        }
        for index, (user, post) in enumerate(sorted(like_pairs))
    ]
    return {
        "users": users,
        "posts": posts,
        "comments": comments,
        "follows": follows,
        "likes": likes,
    }


def create_schema(recorder: Recorder, names: dict[str, str]) -> None:
    schemas = {
        "users": "(_id STRING REQUIRED UNIQUE, username STRING REQUIRED UNIQUE, email STRING REQUIRED UNIQUE, display_name STRING REQUIRED, age INT32 REQUIRED, active BOOL REQUIRED, country STRING, city STRING, bio STRING, tags ARRAY, created_at STRING REQUIRED) STRICT",
        "posts": "(_id STRING REQUIRED UNIQUE, author_id STRING REQUIRED, title STRING REQUIRED, body STRING REQUIRED, tags ARRAY, likes INT64 REQUIRED, views INT64 REQUIRED, published BOOL REQUIRED, created_at STRING REQUIRED) STRICT",
        "comments": "(_id STRING REQUIRED UNIQUE, post_id STRING REQUIRED, user_id STRING REQUIRED, text STRING REQUIRED, score INT32 REQUIRED, created_at STRING REQUIRED) STRICT",
        "follows": "(_id STRING REQUIRED UNIQUE, from_id STRING REQUIRED, to_id STRING REQUIRED, weight FLOAT64 REQUIRED, created_at STRING REQUIRED) STRICT",
        "likes": "(_id STRING REQUIRED UNIQUE, user_id STRING REQUIRED, post_id STRING REQUIRED, reaction STRING REQUIRED, created_at STRING REQUIRED) STRICT",
    }
    for key, schema in schemas.items():
        recorder.query("setup", f"CREATE {key}", f"CREATE COLLECTION {names[key]} {schema};")


def add_indexes_and_relations(recorder: Recorder, names: dict[str, str], prefix: str) -> None:
    statements = [
        f"CREATE INDEX {prefix}_user_age ON {names['users']} (age);",
        f"CREATE INDEX {prefix}_post_author ON {names['posts']} (author_id);",
        f"CREATE INDEX {prefix}_comment_post ON {names['comments']} (post_id);",
        f"CREATE INDEX {prefix}_follow_pair ON {names['follows']} (from_id, to_id);",
        f"ADD FOREIGN KEY {prefix}_post_user_fk ON {names['posts']} (author_id) REFERENCES {names['users']} (_id);",
        f"ADD FOREIGN KEY {prefix}_comment_post_fk ON {names['comments']} (post_id) REFERENCES {names['posts']} (_id);",
        f"ADD FOREIGN KEY {prefix}_comment_user_fk ON {names['comments']} (user_id) REFERENCES {names['users']} (_id);",
    ]
    for index, statement in enumerate(statements, 1):
        recorder.query("setup", f"INDEX/FK {index}", statement)


def document_queries(
    recorder: Recorder, names: dict[str, str], data: dict[str, list[dict[str, Any]]]
) -> None:
    user_id = data["users"][0]["_id"]
    post_id = data["posts"][0]["_id"]
    queries = [
        (
            "SELECT filtered",
            f"SELECT username, age, country FROM {names['users']} WHERE active = true AND age >= 18 LIMIT 100;",
        ),
        (
            "SELECT paging",
            f"SELECT * FROM {names['posts']} WHERE published = true LIMIT 50 SKIP 25;",
        ),
        (
            "SELECT lookup join",
            f"SELECT * FROM {names['posts']} LOOKUP JOIN {names['users']} ON author_id = _id LIMIT 50;",
        ),
        ("COUNT users", f"COUNT FROM {names['users']} WHERE active = true;"),
        (
            "COUNT popular posts",
            f"COUNT FROM {names['posts']} WHERE likes >= 1000 AND views >= 10000;",
        ),
        ("EXISTS user", f"EXISTS IN {names['users']} WHERE _id = {quote(user_id)};"),
        (
            "TEXT contains",
            f"SELECT title, body FROM {names['posts']} WHERE body CONTAINS 'the' LIMIT 50;",
        ),
        (
            "UPDATE set/increment",
            f"UPDATE {names['posts']} INCREMENT views BY 1 SET published = true WHERE _id = {quote(post_id)};",
        ),
        (
            "UPDATE array",
            f"UPDATE {names['users']} ADD TO SET tags = 'benchmark' WHERE _id = {quote(user_id)};",
        ),
        ("SHOW indexes", f"SHOW INDEXES ON {names['posts']};"),
        ("SHOW foreign keys", f"SHOW FOREIGN KEYS ON {names['comments']};"),
        ("DESCRIBE collection", f"DESCRIBE COLLECTION {names['users']};"),
    ]
    for name, query in queries:
        recorder.query("document", name, query, critical=False)


def build_graph(recorder: Recorder, names: dict[str, str], graph: str) -> None:
    # USE and MAP must share one request because the API creates one Executor per request.
    query = f"""
        CREATE LIVE GRAPH {graph} DIRECTED;
        USE GRAPH {graph};
        MAP NODE User FROM {names["users"]} KEY _id
            PROPERTIES username, display_name, age, country, city, active;
        MAP EDGE FOLLOWS FROM {names["follows"]}
            SOURCE from_id AS User TARGET to_id AS User DIRECTED
            PROPERTIES weight, created_at;
        BUILD GRAPH {graph};
    """
    recorder.query("graph", "CREATE/MAP/BUILD graph", query)


def graph_queries(recorder: Recorder, graph: str, data: dict[str, list[dict[str, Any]]]) -> None:
    user0, user1 = data["users"][0]["_id"], data["users"][1]["_id"]
    queries = [
        ("DESCRIBE graph", f"DESCRIBE GRAPH {graph};"),
        ("GRAPH stats", f"GRAPH STATS {graph};"),
        (
            "TRAVERSE out",
            f"USE GRAPH {graph}; TRAVERSE User({quote(user0)}) OUT FOLLOWS DEPTH 3 LIMIT 100;",
        ),
        (
            "TRAVERSE both",
            f"USE GRAPH {graph}; TRAVERSE User({quote(user0)}) BOTH FOLLOWS DEPTH 2 LIMIT 100;",
        ),
        ("GET node", f"USE GRAPH {graph}; GET NODE User({quote(user0)});"),
        (
            "EDGE exists",
            f"USE GRAPH {graph}; EDGE EXISTS User({quote(user0)}) -[FOLLOWS]-> User({quote(user1)});",
        ),
    ]
    for name, query in queries:
        recorder.query("graph", name, query, critical=False)


def algorithm_queries(
    recorder: Recorder, graph: str, data: dict[str, list[dict[str, Any]]]
) -> None:
    user0, user1 = data["users"][0]["_id"], data["users"][1]["_id"]
    algorithms = [
        (
            "MutualFriends",
            f"RUN LOCK MutualFriends ON {graph} WITH user1={quote(user0)}, user2={quote(user1)}, edge_type='FOLLOWS';",
        ),
        (
            "MostConnected",
            f"RUN LOCK MostConnected ON {graph} WITH metric='both', node_type='User' LIMIT 20;",
        ),
        ("NetworkStats", f"RUN LOCK NetworkStats ON {graph} WITH mode='summary';"),
        ("ConnectedComponents", f"RUN JOB ConnectedComponents ON {graph} WITH node_type='User';"),
        (
            "AllDistances",
            f"RUN JOB AllDistances ON {graph} WITH source={quote(user0)}, all=true, max_hops=8, node_type='User';",
        ),
        (
            "CommunityDetection",
            f"RUN JOB CommunityDetection ON {graph} WITH max_iterations=25, min_community_size=2, members=true, node_type='User';",
        ),
        (
            "GetFriends",
            f"RUN LOCK GetFriends ON {graph} WITH user={quote(user0)}, edge_type='FOLLOWS' LIMIT 50;",
        ),
        (
            "AreConnected",
            f"RUN LOCK AreConnected ON {graph} WITH user1={quote(user0)}, user2={quote(user1)}, edge_type='FOLLOWS';",
        ),
        (
            "ShortestPath",
            f"RUN LOCK ShortestPath ON {graph} WITH from={quote(user0)}, to={quote(user1)}, edge_type='FOLLOWS';",
        ),
        (
            "FriendSuggestion",
            f"RUN LOCK FriendSuggestion ON {graph} WITH user={quote(user0)}, edge_type='FOLLOWS' LIMIT 25;",
        ),
        ("BetweennessCentrality", f"RUN JOB BetweennessCentrality ON {graph} RETURNS TOP 25;"),
        (
            "InfluenceMaximization",
            f"RUN JOB InfluenceMaximization ON {graph} WITH k=10, simulations=100, probability=0.1;",
        ),
    ]
    for name, query in algorithms:
        recorder.query("algorithm", name, query, critical=False)


def cleanup(recorder: Recorder, names: dict[str, str], graph: str) -> None:
    recorder.query("cleanup", "DROP graph", f"DROP GRAPH {graph};", critical=False)
    for key in ("likes", "comments", "posts", "follows", "users"):
        recorder.query("cleanup", f"DROP {key}", f"DROP COLLECTION {names[key]};", critical=False)


def print_table(samples: list[Sample]) -> None:
    grouped: dict[tuple[str, str], list[Sample]] = defaultdict(list)
    for sample in samples:
        grouped[(sample.category, sample.name)].append(sample)

    rows: list[list[str]] = []
    for (category, name), values in grouped.items():
        client_total = sum(value.client_ms for value in values)
        server_total = sum(value.server_ms for value in values)
        errors = [value.error for value in values if value.error]
        rows.append(
            [
                category,
                name,
                "PASS" if all(value.ok for value in values) else "FAIL",
                str(len(values)),
                f"{client_total:.2f}",
                f"{client_total / len(values):.2f}",
                f"{server_total:.2f}",
                str(sum(value.rows for value in values)),
                (errors[0][:70] if errors else ""),
            ]
        )

    headers = [
        "category",
        "query",
        "status",
        "runs",
        "client total ms",
        "client avg ms",
        "server ms",
        "rows",
        "error",
    ]
    widths = [max(len(headers[i]), *(len(row[i]) for row in rows)) for i in range(len(headers))]
    rule = "+-" + "-+-".join("-" * width for width in widths) + "-+"

    print("\nNexoraQL benchmark results")
    print(rule)
    print("| " + " | ".join(headers[i].ljust(widths[i]) for i in range(len(headers))) + " |")
    print(rule)
    for row in rows:
        print("| " + " | ".join(row[i].ljust(widths[i]) for i in range(len(headers))) + " |")
    print(rule)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate social data and benchmark NexoraQL end to end"
    )
    parser.add_argument("--url", default=os.getenv("NEXORADB_URL", "http://localhost:8000"))
    parser.add_argument("--token", default=os.getenv("NEXORADB_TOKEN"))
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--seed", type=int, default=20260717)
    parser.add_argument("--locale", default="en_US")
    parser.add_argument("--users", type=int, default=500)
    parser.add_argument("--posts", type=int, default=3_000)
    parser.add_argument("--comments", type=int, default=12_000)
    parser.add_argument("--follows", type=int, default=6_000)
    parser.add_argument("--likes", type=int, default=20_000)
    parser.add_argument("--batch-size", type=int, default=200)
    args = parser.parse_args()
    if not args.token:
        parser.error("--token or NEXORADB_TOKEN is required")
    if args.users < 3 or args.posts < 1 or min(args.comments, args.follows, args.likes) < 0:
        parser.error("users must be >= 3, posts >= 1, and other sizes must be non-negative")
    if not 1 <= args.batch_size <= 1_000:
        parser.error("batch-size must be between 1 and 1000")
    return args


def main() -> int:
    args = parse_args()
    print_native_build_info()
    prefix = f"nxbench_{int(time.time())}_{os.getpid()}"
    names = {key: f"{prefix}_{key}" for key in ("users", "posts", "comments", "follows", "likes")}
    graph = f"{prefix}_social"
    client = connect(url=args.url, token=args.token, timeout=args.timeout)
    recorder = Recorder(client)
    failed: BaseException | None = None

    print(f"Connecting to {args.url} ...")
    if not client.ping():
        raise SystemExit("NexoraDB health check failed")

    print("Generating fake data in memory ...")
    generation_started = time.perf_counter()
    data = generate_data(args)
    generation_ms = (time.perf_counter() - generation_started) * 1000
    counts = ", ".join(f"{key}={len(value):,}" for key, value in data.items())
    print(f"Generated {counts} in {generation_ms:.2f} ms")

    try:
        create_schema(recorder, names)
        for key in ("users", "posts", "comments", "follows", "likes"):
            insert_batches(recorder, names[key], data[key], args.batch_size)
        add_indexes_and_relations(recorder, names, prefix)
        document_queries(recorder, names, data)
        build_graph(recorder, names, graph)
        graph_queries(recorder, graph, data)
        algorithm_queries(recorder, graph, data)
    except BaseException as exc:  # cleanup must also happen on Ctrl-C
        failed = exc
    finally:
        print("Cleaning temporary graph and collections ...")
        cleanup(recorder, names, graph)
        print_table(recorder.samples)

    failures = [
        sample for sample in recorder.samples if not sample.ok and sample.category != "cleanup"
    ]
    if failed is not None:
        print(f"\nBenchmark stopped: {failed}", file=sys.stderr)
    if failures:
        print(f"{len(failures)} query group(s) failed.", file=sys.stderr)
        return 1
    print("\nAll document, graph, and 12 algorithm tests passed; temporary data was removed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
