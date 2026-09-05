# C++ Build, Test, and Benchmark Guide

This guide is the command reference for the NexoraDB C++ core. Run every
command from the repository root. On Windows, run them inside Ubuntu WSL.

## Requirements

- CMake 3.22 or newer
- GCC or Clang with C++20 support
- Git
- Python 3.10 and its development headers when building Python bindings
- `make` (the presets currently use `Unix Makefiles`)

Install the Ubuntu/WSL system packages:

```bash
sudo apt update
sudo apt install -y build-essential cmake git pkg-config \
  python3.10 python3.10-dev python3.10-venv
```

Bootstrap the repository-local vcpkg checkout if it has not been bootstrapped:

```bash
git submodule update --init --recursive
./vcpkg/bootstrap-vcpkg.sh -disableMetrics
```

Inspect the available presets:

```bash
cmake --list-presets=configure
cmake --list-presets=build
cmake --list-presets=test
```

## Debug build

Configure and build:

```bash
cmake --preset debug
cmake --build --preset debug --parallel
```

The generated compilation database is available at:

```bash
ls -l build/debug/compile_commands.json
```

Run all Debug tests:

```bash
ctest --preset debug --output-on-failure
```

List tests without running them:

```bash
ctest --preset debug -N
```

Run one test or suite by regular-expression filter:

```bash
ctest --test-dir build/debug \
  -R 'cpp.DocEngineQueryPlanner' \
  --output-on-failure
```

Run tests verbosely or in parallel:

```bash
ctest --preset debug --verbose
ctest --preset debug --output-on-failure --parallel 2
```

## Release build

The Release preset enables portable optimizations and LTO. It deliberately
does not use `-march=native`.

```bash
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release --output-on-failure
```

For an optimized build that retains debugging symbols:

```bash
cmake --preset relwithdebinfo
cmake --build --preset relwithdebinfo --parallel
```

## AddressSanitizer

```bash
cmake --preset asan
cmake --build --preset asan --parallel
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  ctest --preset asan --output-on-failure
```

Run only the document-engine tests under ASan:

```bash
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  ctest --test-dir build/asan -R 'cpp.DocEngine' --output-on-failure
```

## UndefinedBehaviorSanitizer

The preset already supplies `UBSAN_OPTIONS`, but it can also be set explicitly:

```bash
cmake --preset ubsan
cmake --build --preset ubsan --parallel
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ctest --preset ubsan --output-on-failure
```

The variable is `UBSAN_OPTIONS` (plural), not `UBSAN_OPTION`.

## ThreadSanitizer

The TSan tree must be configured and built before CTest can discover its test
executables:

```bash
cmake --preset tsan
cmake --build --preset tsan --parallel
TSAN_OPTIONS=halt_on_error=1:history_size=7 \
  ctest --preset tsan --output-on-failure
```

Run only the selected concurrency test:

```bash
TSAN_OPTIONS=halt_on_error=1:history_size=7 \
  ctest --test-dir build/tsan \
  -R 'cpp.LiveGraphConcurrency' \
  --output-on-failure
```

Do not combine ASan and TSan in one build.

## Native C++ benchmarks

Configure and build the standalone benchmark executable:

```bash
cmake --preset benchmark
cmake --build --preset benchmark --parallel
```

List registered benchmarks:

```bash
./build/benchmark/benchmarks/cpp/nexora_benchmark \
  --benchmark_list_tests=true
```

Run the complete default benchmark set (100K and 1M datasets):

```bash
./build/benchmark/benchmarks/cpp/nexora_benchmark
```

Run a filtered workload:

```bash
./build/benchmark/benchmarks/cpp/nexora_benchmark \
  '--benchmark_filter=BM_DocumentPointRead/documents:100000/manual_time$'
```

Compare indexed equality with the full-scan reference and save JSON results:

```bash
./build/benchmark/benchmarks/cpp/nexora_benchmark \
  '--benchmark_filter=BM_DocumentFindManyEquality.*/documents:100000/manual_time$' \
  --benchmark_min_time=0.05s \
  --benchmark_repetitions=10 \
  --benchmark_report_aggregates_only=true \
  --benchmark_out=docs/benchmarks/query_planner_phase2_100k.json \
  --benchmark_out_format=json
```

Save a complete baseline:

```bash
./build/benchmark/benchmarks/cpp/nexora_benchmark \
  --benchmark_repetitions=10 \
  --benchmark_report_aggregates_only=true \
  --benchmark_out=docs/benchmarks/baseline-100k.json \
  --benchmark_out_format=json
```

The 10M dataset is opt-in because it requires substantial time and disk space:

```bash
NEXORA_BENCH_10M=1 \
  ./build/benchmark/benchmarks/cpp/nexora_benchmark
```

Benchmark output records the seed, NexoraDB version, build type, compiler,
RocksDB version, system, throughput, p50/p95/p99, memory, and dataset size.

## Python binding and Python tests

Build the pybind11 extension as part of the Debug or Release preset:

```bash
cmake --preset release
cmake --build --preset release --target nexoradb_py --parallel
```

Run the Python suite against the build-tree native module:

```bash
PYTHONPATH="$(pwd)/build/release:$(pwd)/NexoraDB/src" \
  python3.10 -m pytest tests -q
```

Run only the native Python smoke suite:

```bash
PYTHONPATH="$(pwd)/build/release:$(pwd)/NexoraDB/src" \
  python3.10 -m pytest tests/test.py -q
```

## Release validation

Verify that CMake, vcpkg, and Python package versions agree:

```bash
python3.10 scripts/check_release_version.py --expected 0.2.0
```

Build and validate the Python wheel without uploading it:

```bash
python3.10 -m venv .release-venv
.release-venv/bin/python -m pip install --upgrade pip build twine
.release-venv/bin/python -m build --wheel --outdir dist NexoraDB
.release-venv/bin/python -m twine check dist/*.whl
.release-venv/bin/python -m pip install dist/*.whl
.release-venv/bin/python scripts/verify_installed_package.py --expected 0.2.0
```

Inspect the resulting artifact and its metadata:

```bash
ls -lh dist/
unzip -p dist/*0.2.0*.whl \
  '*/METADATA' | grep -E '^(Name|Version|Requires-Python):'
```

## Reconfiguring a build tree

CMake normally updates an existing preset safely:

```bash
cmake --preset debug
cmake --build --preset debug --parallel
```

If a build tree must be recreated, remove only that explicit build directory:

```bash
rm -rf -- build/debug
cmake --preset debug
```

Never remove the repository root or the entire home directory.
