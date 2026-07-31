# NexoraQL social-media benchmark

`social_media_benchmark.py` uses the public NexoraDB Python driver to generate
a realistic, deterministic social-media workload with Faker. It tests document
DDL/DML, indexes, foreign keys, traversal operations, and all 12 graph
algorithms, then prints client/server timing totals.

Install the development dependencies and start the NexoraDB API, then run:

```bash
python -m pip install -e '.[dev]'
python -m pip install Faker
export NEXORADB_TOKEN='<app-token-with-query:execute-scope>'
python examples/social_media_benchmark.py
```

The default workload contains 500 users, 3,000 posts, 12,000 comments, 6,000
follow edges, and 20,000 reactions. Sizes are configurable, for example:

```bash
python examples/social_media_benchmark.py \
  --users 1000 --posts 10000 --comments 50000 \
  --follows 20000 --likes 100000 --batch-size 250
```

Every resource is prefixed with a unique run ID. The graph and all temporary
collections are dropped in a `finally` block, including after a query error or
keyboard interrupt. A failed cleanup is included as `FAIL` in the final table.
