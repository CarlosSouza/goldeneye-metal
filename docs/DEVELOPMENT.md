# Development guide

This repository contains the runtime, Metal backend, GoldenEye integration, launcher, and tests.
Generated game code and game data stay local and must never be committed.

## Build

Requirements: Apple Silicon, current Xcode Command Line Tools, CMake, Git, and SPIRV-Cross with
MSL support.

```sh
brew install cmake spirv-cross
git submodule update --init --recursive

cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release --parallel
ctest --preset macos-arm64-release --output-on-failure
```

Put a compatible local XEX at `vendor/GoldenEye-Recomp/assets/default.xex`, then generate and build
the game integration:

```sh
./out/macos-arm64/rexglue codegen vendor/GoldenEye-Recomp/ge_manifest.toml
cmake -S vendor/GoldenEye-Recomp --preset macos-arm64-release
cmake --build vendor/GoldenEye-Recomp/out/build/macos-arm64-release \
  --target goldeneye_macos_app_verify --parallel
```

The verified unsigned app is written to
`vendor/GoldenEye-Recomp/out/build/macos-arm64-release/dist/GoldenEye Metal.app`.

For a developer build with the private input/test harness, use the matching
`macos-arm64-multiplayer-test` preset for both the runtime and GoldenEye integration. Harness code
is verified absent from release builds.

The normal end-user app build is:

```sh
./launcher/build-app.sh
```

Signing and notarization are separate release-owner steps in
[MACOS_DISTRIBUTION.md](MACOS_DISTRIBUTION.md).

## Run and test

Run a build-tree binary with a complete local game-data folder:

```sh
DYLD_LIBRARY_PATH="$PWD/out/macos-arm64" \
REX_INPUT_BACKEND=sdl REX_MNK_MODE=true \
./vendor/GoldenEye-Recomp/out/build/macos-arm64-release/GoldenEye \
  --game_data_root /absolute/path/to/game-data --gpu metal
```

Main gates:

```sh
ctest --preset macos-arm64-release --output-on-failure

cmake --build vendor/GoldenEye-Recomp/out/build/macos-arm64-release \
  --target goldeneye_macos_app_verify --parallel

MTL_DEBUG_LAYER=1 ./out/macos-arm64/metal_pipeline_probe_test
```

UBSan gate (ASan is incompatible with the fixed guest mappings):

```sh
cmake --preset macos-arm64-ubsan
cmake --build --preset macos-arm64-ubsan --parallel
ctest --preset macos-arm64-ubsan --output-on-failure
```

Useful Metal targets include `metal_mrt_probe_test`, `metal_raster_order_probe_test`,
`metal_resolve_test`, `metal_presenter_shader_test`, and `metal_metalfx_scaler_test`.

The isolated live driver can prove real menu, Dam, multiplayer, pause, capture, and native-quit
behavior without touching player saves or settings:

```sh
python3 tools/stability-cycle.py --cycles 1 --mode dam-gameplay
python3 tools/stability-cycle.py --cycles 1 --mode dam-gameplay --validate-host-pause
python3 tools/stability-cycle.py --cycles 1 --mode dam-gameplay --capture-gpu-frame
python3 tools/stability-cycle.py --cycles 1 --mode local-multiplayer --players 4 \
  --fixed-multiplayer-benchmark-arena
```

The GPU-capture run requires a built `trace_dump_metal`; after shutdown it strictly replays the
private frame twice and fails unless the Metal RGBA outputs are byte-identical.

The four-player benchmark uses Stack and the validated 128-draw Metal submission default. Use the
explicit 64/128/256 override only for controlled A/B runs with a saved performance reference.

Use `./tools/benchmark-dam.sh` for repeatable performance measurements. Keep the generated logs,
screenshots, traces, reference images, and benchmark output under `out/`; they may contain game
content and must not be committed or redistributed.

## Engineering rules

- A passing diagnostic triangle is not proof of the production game path. Require real command,
  shader, resolve, and presentation evidence.
- Preserve guest-visible ordering around fences, readback, resolve, swap, resource replacement,
  cache teardown, and shader memory export.
- Never advance a title-owned fence, ring pointer, or presented-frame counter on a timeout.
- Unsupported rendering state must fail closed and produce a bounded diagnostic.
- Keep normal logging cheap; high-volume traces must be opt-in and bounded.
- Record the exact build, hardware, environment, and reproduction path for performance claims.
- Run `git diff --check` and the focused tests for every changed subsystem before committing.

Current progress and remaining gaps are summarized in
[GOLDENEYE_NATIVE_METAL_PROJECT_STATUS.md](GOLDENEYE_NATIVE_METAL_PROJECT_STATUS.md).
