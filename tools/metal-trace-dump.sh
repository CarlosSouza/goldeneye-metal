#!/usr/bin/env bash
# Build the native Metal trace runner and dump one captured GPU frame.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
SOURCE="$ROOT/vendor/GoldenEye-Recomp"
BUILD="${GOLDENEYE_TRACE_BUILD_DIR:-$ROOT/out/tools/metal-trace-dump}"

usage() {
  printf 'Usage: %s TRACE.xtr [OUTPUT_BASE] [FRAME_INDEX] [--best-effort]\n' "$0"
  printf '       --best-effort permits diagnostic replay when EDRAM state is incomplete.\n'
}

BEST_EFFORT=0
POSITIONAL=()
for ARG in "$@"; do
  case "$ARG" in
    --best-effort)
      BEST_EFFORT=1
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    -*)
      printf 'metal-trace-dump: unknown option: %s\n' "$ARG" >&2
      usage >&2
      exit 2
      ;;
    *)
      POSITIONAL+=("$ARG")
      ;;
  esac
done

if (( ${#POSITIONAL[@]} < 1 || ${#POSITIONAL[@]} > 3 )); then
  usage >&2
  exit 2
fi

TRACE="${POSITIONAL[0]}"
FRAME="${POSITIONAL[2]:-0}"
if [[ ! -f "$TRACE" ]]; then
  printf 'metal-trace-dump: trace does not exist: %s\n' "$TRACE" >&2
  exit 2
fi
if [[ ! "$FRAME" =~ ^[0-9]+$ ]]; then
  printf 'metal-trace-dump: frame index must be a non-negative integer\n' >&2
  exit 2
fi

if (( ${#POSITIONAL[@]} >= 2 )); then
  OUTPUT_BASE="${POSITIONAL[1]}"
else
  TRACE_NAME="$(basename "${TRACE%.*}")"
  OUTPUT_BASE="$ROOT/out/trace-dumps/$TRACE_NAME-frame-$FRAME"
fi

JOBS="${GOLDENEYE_TRACE_JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || printf 4)}"
cmake -S "$SOURCE" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DGOLDENEYE_BUILD_METAL_TRACE_DUMP=ON
cmake --build "$BUILD" --config Release --target trace_dump_metal -j "$JOBS"

COMMAND=("$BUILD/trace_dump_metal" "$TRACE" "$OUTPUT_BASE" "$FRAME")
if (( BEST_EFFORT )); then
  COMMAND+=(--best-effort)
fi
exec "${COMMAND[@]}"
