#!/usr/bin/env python3
"""Sequential curl transport probe. Never silently accepts protocol fallback.

Run through a separately provisioned isolated network-emulation environment.
Tokens are supplied through stdin to curl, never command-line arguments.
"""
import argparse
import json
import os
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--protocol", choices=["1.1", "2", "3"], required=True)
    parser.add_argument("--query", default="SHOW COLLECTIONS;")
    parser.add_argument("--samples", type=int, default=100)
    parser.add_argument("--warmup", type=int, default=10)
    parser.add_argument("--network-label", required=True, help="Record verified RTT/loss configuration, or baseline")
    args = parser.parse_args()
    if args.samples < 3 or args.warmup < 0:
        parser.error("samples >= 3 and warmup >= 0 required")
    token = os.environ.get("NEXORA_BENCH_TOKEN", "")
    if not token or any(c in token for c in '\r\n"\\'):
        parser.error("Set NEXORA_BENCH_TOKEN to a valid bearer token")
    values = []
    for index in range(args.warmup + args.samples):
        command = ["curl", "--config", "-", "--silent", "--show-error", "--max-time", "30",
                   "--http" + ("3-only" if args.protocol == "3" else args.protocol),
                   "--output", os.devnull, "--write-out", "%{http_version} %{http_code} %{time_total}",
                   "--header", "Content-Type: application/json", "--data", json.dumps({"query": args.query}), args.url]
        result = subprocess.run(command, input=f'header = "Authorization: Bearer {token}"\n',
                                text=True, capture_output=True, timeout=35)
        if result.returncode:
            raise SystemExit("curl failed; verify protocol support, TLS, UDP and endpoint (no result recorded)")
        version, code, elapsed = result.stdout.split()
        if version != args.protocol or code != "200":
            raise SystemExit(f"Rejected sample: HTTP/{version}, status {code}; expected HTTP/{args.protocol} 200")
        if index >= args.warmup:
            values.append(float(elapsed) * 1000)
    ordered = sorted(values)
    print(json.dumps({"timestamp": time.time(), "protocol": args.protocol,
                      "network": args.network_label, "samples": len(values),
                      "median_ms": statistics.median(values),
                      "p99_ms": ordered[min(len(ordered)-1, int(len(ordered)*.99))],
                      "connection_mode": "fresh curl process per request; includes handshake",
                      "curl": subprocess.check_output(["curl", "--version"], text=True).splitlines()[0]}))


if __name__ == "__main__":
    main()
