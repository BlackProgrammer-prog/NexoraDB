"""Embed collect()/prometheus() in the application that owns the live engines."""
import json


def collect(engine, manager=None):
    output = dict(operations=json.loads(engine.operation_metrics()), health=json.loads(engine.health_report()))
    rocks = engine.rocks_metrics()
    output["rocksdb"] = {name: getattr(rocks, name) for name in dir(rocks)
                         if not name.startswith("_") and isinstance(getattr(rocks, name), (int, float, bool))}
    if manager is not None:
        stats = manager.scheduler_stats()
        output["queue"] = {name: getattr(stats, name) for name in ("workers", "queued", "running", "reserved_bytes", "stopping")}
        output["wal"] = {name: manager.get_wal_status(name) for name in manager.list_graphs()}
    return output


def prometheus(sample):
    lines = ["# TYPE nexora_operation_duration_seconds histogram", "# TYPE nexora_operation_errors_total counter"]
    for operation, item in sample["operations"]["operations"].items():
        label = 'operation=' + json.dumps(operation)
        for bucket in item["buckets"]:
            boundary = str(bucket["le_seconds"])
            lines.append(f'nexora_operation_duration_seconds_bucket{{{label},le="{boundary}"}} {bucket["count"]}')
        lines += [f'nexora_operation_duration_seconds_count{{{label}}} {item["calls_total"]}',
                  f'nexora_operation_duration_seconds_sum{{{label}}} {item["duration_seconds_sum"]}',
                  f'nexora_operation_errors_total{{{label}}} {item["errors_total"]}',
                  f'nexora_operation_not_found_total{{{label}}} {item["not_found_total"]}']
    lines.append(f'nexora_ready {int(sample["health"]["ready"])}')
    for group in ("rocksdb", "queue"):
        for name, value in sample.get(group, {}).items():
            lines.append(f"nexora_{group}_{name} {float(value)}")
    for graph, fields in sample.get("wal", {}).items():
        for name, value in fields.items():
            lines.append(f'nexora_wal_{name}{{graph={json.dumps(graph)}}} {float(value)}')
    return "\n".join(lines) + "\n"
