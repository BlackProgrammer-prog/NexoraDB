"""Compare like-for-like native benchmark samples; retain JSONL and SVG history."""
import argparse
import datetime
import hashlib
import html
import json
import platform
from pathlib import Path
import statistics
import collections
import subprocess


def host_identity():
    cpu = next((line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                if line.startswith("model name")), platform.machine())
    # Explicit runner identity prevents comparing different machines of one model.
    import os
    identity = platform.node() + "/" + os.environ.get("RUNNER_NAME", "local")
    data = dict(machine_sha256=hashlib.sha256(identity.encode()).hexdigest(), cpu=cpu,
                kernel=platform.release(), affinity_cpus=len(os.sched_getaffinity(0)))
    return data


def summarize(rows):
    groups = {}
    for row in rows:
        if row["errors"] != 0:
            raise ValueError("benchmark operation errors")
        key = row["profile"] + "/" + row["workload"]
        groups.setdefault(key, []).append(row)
    if not groups or any(len(values) < 3 for values in groups.values()):
        raise ValueError("at least three repetitions required for every group")
    return {key: {metric: [float(row[metric]) for row in values]
                  for metric in ("ops_s", "read_p99_us", "write_p99_us")}
            for key, values in groups.items()}


def compare(current, baseline):
    if current["identity"] != baseline["identity"] or current["config"] != baseline["config"] or current.get("build") != baseline.get("build"):
        raise ValueError("incompatible host/config; explicit baseline approval required")
    if current["samples"].keys() != baseline["samples"].keys():
        raise ValueError("incomplete or changed benchmark group set")
    failures = []
    for key, metrics in current["samples"].items():
        for metric, values in metrics.items():
            old = baseline["samples"][key][metric]
            new_median, old_median = statistics.median(values), statistics.median(old)
            if metric == "ops_s":
                regression = new_median < old_median * .85 and max(values) < min(old) and old_median-new_median >= 100
            else:
                regression = new_median > old_median * 1.25 and min(values) > max(old) and new_median-old_median >= 10
            if regression:
                failures.append(dict(group=key, metric=metric, baseline=old_median, current=new_median))
    return failures


def report_history(records):
    groups = sorted({key for record in records for key in record["samples"]})
    parts = ["<!doctype html><meta charset=utf-8><title>NexoraDB benchmark history</title>",
             "<h1>Native benchmark history</h1><p>Medians; compare only compatible host/config identities.</p>"]
    for group in groups:
        values = [(record["at"], statistics.median(record["samples"][group]["ops_s"]))
                  for record in records if group in record["samples"]]
        maximum = max((value for _, value in values), default=1) or 1
        points = " ".join(f"{20+i*760/max(1,len(values)-1):.1f},{180-value/maximum*150:.1f}" for i, (_, value) in enumerate(values))
        parts.append(f"<h2>{html.escape(group)} throughput ops/s</h2><svg viewBox='0 0 800 200' role='img' aria-label='throughput trend'><polyline fill='none' stroke='steelblue' stroke-width='2' points='{points}'/></svg>")
        parts.append("<table><tr><th>UTC</th><th>Median ops/s</th></tr>" + "".join(f"<tr><td>{html.escape(at)}</td><td>{value:.1f}</td></tr>" for at, value in values) + "</table>")
    return "\n".join(parts)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("input", "baseline", "history", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--accept-baseline", action="store_true")
    parser.add_argument("--build-dir", type=Path, default=Path("build/release"))
    args = parser.parse_args(argv)
    raw = args.input.read_bytes()
    rows = [json.loads(line) for line in raw.decode().splitlines() if line.strip()]
    samples = summarize(rows)
    config = sorted({(row["profile"], row["workload"], row["records"], row["operations"], row["threads"]) for row in rows})
    record = dict(schema=1, at=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  identity=host_identity(), config=[list(value) for value in config], samples=samples)
    record["input_sha256"] = hashlib.sha256(raw).hexdigest()
    cache = {}
    for line in (args.build_dir / "CMakeCache.txt").read_text().splitlines():
        if "=" in line and not line.startswith(("#", "//")):
            key, value = line.split("=", 1)
            cache[key.split(":", 1)[0]] = value
    keys = ("CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_RELEASE",
            "NEXORA_ENABLE_LTO", "NEXORA_ENABLE_ASAN", "NEXORA_ENABLE_UBSAN", "NEXORA_ENABLE_TSAN", "VCPKG_TARGET_TRIPLET")
    record["build"] = {key: cache.get(key) for key in keys}
    record["build"]["compiler_version"] = subprocess.check_output([cache["CMAKE_CXX_COMPILER"], "--version"], text=True).splitlines()[0]
    record["revision"] = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    record["dirty"] = bool(subprocess.check_output(["git", "status", "--porcelain"], text=True).strip())
    failures, comparison_error = [], None
    if args.accept_baseline:
        args.baseline.parent.mkdir(parents=True, exist_ok=True)
        temporary = args.baseline.with_suffix(".pending")
        temporary.write_text(json.dumps(record, sort_keys=True))
        temporary.replace(args.baseline)  # Explicit approval is required for replacement.
    else:
        try:
            failures = compare(record, json.loads(args.baseline.read_text()))
        except (FileNotFoundError, ValueError) as error:
            comparison_error = str(error)
    record["regressions"] = failures
    record["comparison_error"] = comparison_error
    args.history.parent.mkdir(parents=True, exist_ok=True)
    import fcntl
    with args.history.open("a+") as stream:
        fcntl.flock(stream, fcntl.LOCK_EX)
        stream.write(json.dumps(record, sort_keys=True) + "\n")
        stream.flush()
        stream.seek(0)
        history = list(collections.deque((json.loads(line) for line in stream if line.strip()), maxlen=1000))
    # Never overlay different hosts/configs in one trend line.
    history = [value for value in history if value["identity"] == record["identity"] and value["config"] == record["config"] and value.get("build") == record.get("build")]
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(report_history(history))
    print(json.dumps(dict(regressions=failures, comparison_error=comparison_error)))
    return 1 if failures or comparison_error else 0


if __name__ == "__main__":
    raise SystemExit(main())
