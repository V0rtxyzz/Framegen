#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELEASE_DIR="${ROOT_DIR}/release"
mkdir -p "${RELEASE_DIR}"

echo "==> [1/3] Host unit tests (planner logic)..."
g++ -std=c++20 -O2 -Wall -Wextra -I"${ROOT_DIR}/include" \
    "${ROOT_DIR}/tests/test_framegen.cpp" "${ROOT_DIR}/src/FrameGen.cpp" "${ROOT_DIR}/src/EglHooks.cpp" \
    -o /tmp/test_framegen
/tmp/test_framegen

echo "==> [2/3] Compiling Android arm64-v8a library (libframegen.so)..."
NDK_DIR="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [[ -z "${NDK_DIR}" || ! -f "${NDK_DIR}/build/cmake/android.toolchain.cmake" ]]; then
    echo "Error: ANDROID_NDK_HOME is not set (the GitHub Actions workflow sets it)." >&2
    exit 1
fi
BUILD_DIR="/tmp/framegen-cmake-android"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${NDK_DIR}/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DANDROID_STL=c++_static
cmake --build "${BUILD_DIR}" --target framegen -j"$(nproc)"
cp "${BUILD_DIR}/libframegen.so" "${RELEASE_DIR}/libframegen.so"

echo "==> [3/3] Packaging FrameGen.levipack..."
STAGE_DIR="/tmp/framegen-levipack-stage"
rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}"
cp "${ROOT_DIR}/manifest.json" "${STAGE_DIR}/manifest.json"
cp "${RELEASE_DIR}/libframegen.so" "${STAGE_DIR}/libframegen.so"

python3 - <<'PY'
import os, zipfile

stage = "/tmp/framegen-levipack-stage"
outputs = [os.path.abspath("FrameGen.levipack"), os.path.abspath("release/FrameGen.levipack")]
for out_path in outputs:
    if os.path.exists(out_path):
        os.remove(out_path)
    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for root, _, files in os.walk(stage):
            for f in sorted(files):
                full = os.path.join(root, f)
                zf.write(full, os.path.relpath(full, stage).replace(os.sep, "/"))
    print(f"Created {out_path} ({os.path.getsize(out_path)} bytes)")
PY

readelf -h "${RELEASE_DIR}/libframegen.so" | grep -E "Class:|Machine:|Type:"
echo "Build complete!"
