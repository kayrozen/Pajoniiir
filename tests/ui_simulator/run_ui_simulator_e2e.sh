#!/usr/bin/env bash
set -euo pipefail

UPDATE_BASELINES=0
KEEP_ARTIFACTS=0
LVGL_PATH=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -UpdateBaselines|-UpdateBaselines=?*)
            UPDATE_BASELINES=1
            ;;
        -KeepArtifacts|-KeepArtifacts=?*)
            KEEP_ARTIFACTS=1
            ;;
        -LvglPath=?*)
            LVGL_PATH="${1#*=}"
            ;;
        -LvglPath)
            shift
            LVGL_PATH="$1"
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
    shift
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CACHE_ROOT="$REPO_ROOT/.cache/ui_simulator"
BUILD_DIR="$CACHE_ROOT/build"
OUTPUT_DIR="$CACHE_ROOT/screenshots"
MANIFEST_PATH="$SCRIPT_DIR/baselines.json"
LVGL_COMMIT="263ae5e13dec1e525109aed556cec1bbdfdecd5a"

# Resolve tools — must be on PATH
for tool in git cmake ninja gcc g++; do
    if ! command -v "$tool" &>/dev/null; then
        echo "Required tool '$tool' was not found." >&2
        exit 1
    fi
done

if [[ -n "$LVGL_PATH" ]]; then
    RESOLVED_LVGL="$(cd "$LVGL_PATH" && pwd)"
else
    RESOLVED_LVGL="$CACHE_ROOT/lvgl-${LVGL_COMMIT:0:12}"
    if [[ ! -d "$RESOLVED_LVGL/.git" ]]; then
        mkdir -p "$CACHE_ROOT"
        git clone --filter=blob:none --no-checkout https://github.com/lvgl/lvgl.git "$RESOLVED_LVGL"
        git -C "$RESOLVED_LVGL" fetch --depth 1 origin "$LVGL_COMMIT"
        git -C "$RESOLVED_LVGL" checkout --detach "$LVGL_COMMIT"
    fi
fi

ACTUAL_LVGL_COMMIT="$(git -C "$RESOLVED_LVGL" rev-parse HEAD | tr -d '[:space:]')"
if [[ "$ACTUAL_LVGL_COMMIT" != "$LVGL_COMMIT" ]]; then
    echo "LVGL checkout mismatch: expected $LVGL_COMMIT, found $ACTUAL_LVGL_COMMIT" >&2
    exit 1
fi

LVGL_CHANGES="$(git -C "$RESOLVED_LVGL" status --porcelain)"
if [[ -n "$LVGL_CHANGES" ]]; then
    echo "LVGL checkout has local changes and is not reproducible: $RESOLVED_LVGL" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR" "$OUTPUT_DIR"

export PATH="$(dirname "$(command -v gcc)"):$(dirname "$(command -v ninja)"):$PATH"

CMAKE_C_COMPILER="$(command -v gcc)"
CMAKE_CXX_COMPILER="$(command -v g++)"
CMAKE_MAKE_PROGRAM="$(command -v ninja)"

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G Ninja \
    "-DLVGL_DIR=$RESOLVED_LVGL" \
    "-DCMAKE_MAKE_PROGRAM=$CMAKE_MAKE_PROGRAM" \
    "-DCMAKE_C_COMPILER=$CMAKE_C_COMPILER" \
    "-DCMAKE_CXX_COMPILER=$CMAKE_CXX_COMPILER" \
    -DCMAKE_BUILD_TYPE=Release

cmake --build "$BUILD_DIR" --target ui_simulator_e2e

EXECUTABLE="$BUILD_DIR/ui_simulator_e2e"
if [[ ! -x "$EXECUTABLE" ]]; then
    echo "Executable not found: $EXECUTABLE" >&2
    exit 1
fi

"$EXECUTABLE" "$OUTPUT_DIR"

# --- Screenshot comparison ---
CAPTURES=(
    overview_deck1
    overview_deck2
    library
    hot_cues
    settings
    screensaver
    settings_restored
)

declare -A ACTUAL
for name in "${CAPTURES[@]}"; do
    path="$OUTPUT_DIR/$name.ppm"
    if [[ ! -f "$path" ]]; then
        echo "Missing screenshot: $path" >&2
        exit 1
    fi
    ACTUAL[$name]="$(sha256sum "$path" | awk '{print $1}')"
done

if [[ $UPDATE_BASELINES -eq 1 ]]; then
    # Build JSON manifest with Python (avoids jq dependency for nested objects)
    python3 -c "
import json, sys
captures = ${UPDATE_BASELINES@+{
}"$(for name in "${CAPTURES[@]}"; do
    echo "    '$name': '${ACTUAL[$name]}',"
done)"}
manifest = {
    'schema': 1,
    'width': 800,
    'height': 480,
    'lvgl_commit': '${LVGL_COMMIT}',
    'captures': captures,
}
with open('${MANIFEST_PATH}', 'w', encoding='utf-8') as f:
    json.dump(manifest, f, indent=2, ensure_ascii=False)
    f.write('\n')
print('Updated UI screenshot baselines: ${MANIFEST_PATH}')
"
else
    if [[ ! -f "$MANIFEST_PATH" ]]; then
        echo "Baseline manifest is missing. Review captures, then run with -UpdateBaselines." >&2
        exit 1
    fi

    MISMATCH=0
    while IFS= read -r line; do
        name="$(echo "$line" | cut -d'=' -f1)"
        expected="${ACTUAL[$name]}"
        actual="${ACTUAL[$name]}"
        if [[ -z "$expected" || "$expected" != "$actual" ]]; then
            echo "Screenshot mismatch: $name expected=$expected actual=$actual" >&2
            MISMATCH=1
        else
            echo "PASS screenshot $name $actual"
        fi
    done < <(
        python3 -c "
import json
with open('${MANIFEST_PATH}', 'r', encoding='utf-8') as f:
    expected = json.load(f)
if expected['schema'] != 1 or expected['lvgl_commit'] != '${LVGL_COMMIT}' or expected['width'] != 800 or expected['height'] != 480:
    print('Baseline metadata does not match the pinned simulator configuration.', file=sys.stderr)
    sys.exit(1)
for name, h in expected['captures'].items():
    print(f'{name}={h}')
"
    )

    if [[ $MISMATCH -eq 1 ]]; then
        echo "UI screenshot regression failed. Actual captures: $OUTPUT_DIR" >&2
        exit 1
    fi
fi

if [[ $KEEP_ARTIFACTS -eq 1 ]]; then
    echo "UI screenshots retained at $OUTPUT_DIR"
fi

echo 'UI simulator build, scripted navigation and screenshot gate passed.'
