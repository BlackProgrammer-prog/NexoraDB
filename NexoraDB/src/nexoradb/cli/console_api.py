"""Bounded admin operations shared by the console tabs; no query interpolation."""

import re

from .api_client import AuthSession
from .query_client import QueryClient, QueryClientError

ALGORITHMS = frozenset(
    {
        "GetFriends",
        "AreConnected",
        "ShortestPath",
        "FriendSuggestion",
        "MostConnected",
        "MutualFriends",
        "NetworkStats",
        "ConnectedComponents",
        "AllDistances",
        "BetweennessCentrality",
        "CommunityDetection",
        "InfluenceMaximization",
    }
)
SCOPE = re.compile(r"[a-z][a-z0-9_]*:[a-z][a-z0-9_]*\Z")


def graph_query(algo, graph, params):
    if algo not in ALGORITHMS:
        raise ValueError("Unknown graph algorithm")
    if not isinstance(graph, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,127}", graph):
        raise ValueError("Select a graph with a valid NexoraQL identifier")
    two = {
        "AreConnected": ("user1", "user2"),
        "MutualFriends": ("user1", "user2"),
        "ShortestPath": ("from", "to"),
    }
    one = {"GetFriends": "user", "FriendSuggestion": "user", "AllDistances": "source"}
    names = two.get(algo, (one[algo],) if algo in one else ())
    if (
        not isinstance(params, list)
        or len(params) != len(names)
        or any(not isinstance(p, str) or len(p) > 1024 for p in params)
    ):
        raise ValueError(f"{algo} requires a JSON array of {len(names)} string node IDs")
    bindings = {f"p{i}": value for i, value in enumerate(params)}
    options = [f"{name}=$p{i}" for i, name in enumerate(names)]
    if algo == "AllDistances":
        options += ["all=true", "max_hops=4"]
    if algo == "CommunityDetection":
        options += ["max_iterations=10", "min_community_size=2", "members=true"]
    if algo == "InfluenceMaximization":
        options += ["k=5", "simulations=20", "probability=0.1"]
    kind = (
        "JOB"
        if algo
        in {
            "ConnectedComponents",
            "AllDistances",
            "BetweennessCentrality",
            "CommunityDetection",
            "InfluenceMaximization",
        }
        else "LOCK"
    )
    query = f"RUN {kind} {algo} ON {graph}"
    if options:
        query += " WITH " + ", ".join(options)
    if algo in {"GetFriends", "FriendSuggestion", "MostConnected"}:
        query += " LIMIT 20"
    return query + ";", bindings


class ConsoleApi:
    def __init__(self, base_url, token=None, *, client=None):
        self.client = client or QueryClient(base_url, token)

    async def close(self):
        await self.client.close()

    async def setup_state(self):
        return await self.client.request("GET", "/auth/setup-state")

    async def login(self, username, password):
        result = await self.client.request(
            "POST", "/auth/login", {"username": username, "password": password}
        )
        token = result.get("accessToken")
        if not isinstance(token, str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,16384}", token):
            raise QueryClientError("Invalid login response")
        return AuthSession(token, username)

    async def register(self, first, last, email, password):
        return await self.client.request(
            "POST",
            "/auth/register",
            {"firstName": first, "lastName": last, "email": email, "password": password},
        )

    async def scopes(self):
        result = await self.client.request("GET", "/apps/scopes")
        scopes = result.get("scopes")
        if (
            not isinstance(scopes, list)
            or len(scopes) > 128
            or any(not isinstance(s, str) or len(s) > 64 or not SCOPE.fullmatch(s) for s in scopes)
        ):
            raise QueryClientError("Invalid scope catalogue")
        return list(dict.fromkeys(scopes))

    async def graphs(self):
        result = await self.client.request("GET", "/graphs", allow_list=True)
        if not isinstance(result, list) or len(result) > 500:
            raise QueryClientError("Graph list exceeds 500 entries; use the query workspace")
        graphs = []
        for item in result:
            name = item.get("id", item.get("name")) if isinstance(item, dict) else None
            if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,127}", name):
                raise QueryClientError("Invalid graph identifier in server response")
            if name not in graphs:
                graphs.append(name)
        return graphs

    async def create_token(self, app_id, name, expiry, scopes):
        return await self.client.request(
            "POST",
            "/apps/tokens",
            {"appId": app_id, "appName": name, "expiresInSeconds": expiry, "scopes": scopes},
        )

    async def algorithm(self, algo, graph, params):
        query, bindings = graph_query(algo, graph, params)
        result = await self.client.buffered(query, bindings)
        if result.get("statementErrors"):
            raise QueryClientError(
                "Algorithm failed: " + "; ".join(map(str, result["statementErrors"]))
            )
        return result
