# Phase 3 fuzz harnesses

The two libFuzzer targets cover document JSON/BSON parsing plus field extraction,
and every compiled update operator/value-type combination. Build with Clang:

```bash
cmake -S . -B build/fuzz -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DNEXORA_BUILD_FUZZERS=ON -DNEXORA_BUILD_GRAPH=OFF \
  -DNEXORA_BUILD_PYTHON=OFF -DBUILD_TESTING=OFF
cmake --build build/fuzz --parallel

cp -a tests/fuzz/corpus/document /tmp/nexora-document-corpus
cp -a tests/fuzz/corpus/update /tmp/nexora-update-corpus
ASAN_OPTIONS=quarantine_size_mb=16 \
  build/fuzz/tests/fuzz/nexora_document_parser_fuzz \
  /tmp/nexora-document-corpus -max_total_time=60
ASAN_OPTIONS=quarantine_size_mb=16 \
  build/fuzz/tests/fuzz/nexora_update_fuzz \
  /tmp/nexora-update-corpus -max_total_time=60
```

Crashing inputs are written to the current directory by libFuzzer. Keep any
regression-producing input in the matching corpus after minimizing it with
`-minimize_crash=1`.
