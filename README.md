# GoldenEye Metal

GoldenEye 007 gameplay on macOS, recompiled for Apple Silicon and rendered directly with Apple
Metal.

> [!NOTE]
> **This fork adds an experimental iPadOS port** — see [iPadOS Port](#ipados-port-this-fork)
> below. The macOS version is unchanged and both targets build from the same source tree.

## iPadOS Port (this fork)

The `ipad-port` branch runs GoldenEye natively on iPad (tested on an A17 Pro iPad via
LiveContainer). It is the same runtime as the macOS version — the ahead-of-time recompiled
game code, Xenos-to-Metal graphics path, kernel and audio are all shared; only the platform
layer differs, selected at build time by CMake:

| Layer | macOS | iPadOS |
| --- | --- | --- |
| Window/presentation | AppKit (`window_macos.mm`) | UIKit + CAMetalLayer (`window_ios.mm`) |
| Entry point | custom AppKit event loop | `UIApplicationMain` + CADisplayLink |
| Guest memory (4.5 GB) | POSIX `shm_open` | Mach named memory entry + `vm_map` views |
| Fibers | arm64 asm context switch (this fork, both platforms) | same |
| Launcher / game import | native launcher app | none — import on a Mac, drop `Game Data` into the app's Documents |
| Input | keyboard/mouse + controllers | controllers, on-screen touch gamepad, gyro aim |

Extras added for handheld play:

- **Gyro aiming** while the left trigger is held — uses the controller's gyro when it has one
  (DualShock 4/DualSense/Switch Pro) or the iPad's own motion sensor for grip controllers
  (GameSir G8 and similar). Default *hold* mode maps sustained tilt to crosshair offset,
  matching GoldenEye's positional aim; clicking R3 re-centers.
  Cvars: `controller_gyro_aim`, `controller_gyro_mode`, `controller_gyro_sensitivity`,
  `controller_gyro_deadzone`.
- **On-screen touch controls** (MeloNX-style layout) that appear only while no physical
  controller is connected, feeding an SDL virtual gamepad.
- **Host Settings by touch or controller** — hold L3+R3 for ~0.75 s (upstream feature) and
  navigate by touch or gamepad.
- **Suspend-safe lifecycle** — backgrounding auto-pauses the game and freezes guest-visible
  time so the title's GPU-hang watchdog never trips across an iOS process suspension.

### Building for iPad

Everything cross-compiles from a Mac (the recompiler itself always runs on the host):

```sh
# one-time host tooling + codegen (see "Generate and build GoldenEye" below)
cmake --preset macos-arm64-release
cmake --build out/build/macos-arm64-release --target rexglue --parallel
./out/macos-arm64/rexglue codegen vendor/GoldenEye-Recomp/ge_manifest.toml

# iOS SDK + game
cmake --preset ios-arm64-release
cmake --build out/build/ios-arm64-release --parallel
cmake -S vendor/GoldenEye-Recomp --preset ios-arm64-release
cmake --build vendor/GoldenEye-Recomp/out/build/ios-arm64-release --target ge --parallel

# SideStore/LiveContainer-installable ipa (out/ios-arm64/GoldenEye-iPad.ipa)
./scripts/build/ios/package-ipa.sh
```

The iOS build additionally needs a static SPIRV-Cross for iOS installed under
`out/ios-deps/spirv-cross` (same tag the launcher uses; see `docs/ipad/PLAN.md` for the
exact commands, the full bring-up notes and known issues).

To install: sideload the ipa with SideStore (the signature already declares the
extended-virtual-addressing and increased-memory-limit entitlements) or import it into
LiveContainer, then import your legally owned game backup with the macOS launcher once and
copy the resulting `Game Data` folder into the app's Documents via the Files app. As with
the macOS version, no game data is included or downloaded.

**[Download v0.4.1 for macOS (.dmg)](https://github.com/ysrdevs/goldeneye-metal/releases/download/v0.4.1/GoldenEye-Metal-0.4.1-macos-arm64.dmg)** ·
[Release notes](https://github.com/ysrdevs/goldeneye-metal/releases/tag/v0.4.1) ·
[Watch gameplay](https://youtu.be/VkbwbXw2tPw) ·
[Discord](https://discord.gg/2AKEFgR7)

[![GoldenEye Metal gameplay on macOS](https://img.youtube.com/vi/VkbwbXw2tPw/maxresdefault.jpg)](https://youtu.be/VkbwbXw2tPw)

> [!WARNING]
> This is an experimental release, not a finished port. The first Dam mission runs and is
> playable, but frame rate and graphics are not yet consistent in every scene.

## Play on macOS

You need:

- an Apple Silicon Mac;
- macOS 14 or newer; and
- a compatible game backup that you are legally authorized to use.

To start playing:

1. [Download the DMG](https://github.com/ysrdevs/goldeneye-metal/releases/download/v0.4.1/GoldenEye-Metal-0.4.1-macos-arm64.dmg).
2. Open it and drag **GoldenEye Metal.app** into Applications.
3. Launch the app. On first use, select your compatible local game backup and wait for the
   one-time import.
4. Choose **Play GoldenEye**. Future launches open the same launcher with your private local copy
   ready.

The first-run launcher accepts:

- the compatible original game-backup ZIP;
- the Xbox LIVE/STFS package stored inside that backup; or
- an extracted folder containing `default.xex`, `files/`, `music.xwb`, and `sfx.xwb`.

The launcher verifies the supported game revision before importing it. Your files stay on your
Mac: the app does not include, download, or upload game data. You can choose a different source
from the launcher later.

If a game session does not close cleanly, the launcher offers **Start in Safe Mode** for one run or
**Play Normally**. You can also choose **Export Diagnostic Bundle…** and send the resulting ZIP
with a report; it excludes game data, saves, cache, and settings.

Choose **Manage Saves…** to create a portable `.gesave` backup, restore a backup, or reset local
progress. Restore and reset preserve the previous data so the action can be undone immediately.

The release is Developer ID signed, Apple-notarized, and stapled. See the
[player guide](docs/PLAYER_GUIDE.md) for detailed controls, controller setup, import behavior, and
troubleshooting.

## Controls

| Action | Keyboard and mouse |
| --- | --- |
| Move | WASD |
| Look | Mouse |
| Fire | Left click |
| Aim | Right click |
| A / confirm | Space |
| B / back | Shift |
| Start | Return |
| D-pad | Arrow keys |
| Original / remastered graphics | F |
| Host settings / release cursor | Escape |
| Quit | Command-Q |

Modern controllers work over USB or Bluetooth, including DualShock 4, DualSense, Xbox One, and
Xbox Series X|S controllers. Controllers can be connected or removed while the game is running,
and keyboard/mouse can remain active at the same time. The Controls page includes Modern,
Classic, and Southpaw layouts plus per-button remapping.

## What this project is

GoldenEye Metal is not a traditional full-system emulator. The game code is recompiled ahead of
time for ARM64, while a compatibility runtime provides the Xbox 360 APIs and GPU behavior the game
expects.

The graphics path consumes the game's real command stream and shaders, translates the shaders,
and renders the result directly through Metal:

```text
game commands -> Xenos shader translation -> Metal rendering -> game resolve -> presentation
```

There is no Vulkan or MoltenVK graphics path in the macOS release. The repository contains the
runtime, recompilation toolchain, Metal backend, native launcher, and game integration source.

## Current state

Working today:

- the classification, gun-barrel, RARE, menu, briefing, and first Dam gameplay sequences;
- native Metal presentation on Apple Silicon;
- native keyboard/mouse input, stable P1–P4 gamepad ports, local split-screen, presets, and remapping;
- a local game-data importer, crash-aware Safe Mode, save management, true local-mission pause,
  and diagnostic export;
- live performance presets, MetalFX/Sharp output scaling, filtering, FXAA and colour controls;
- optional FPS or detailed performance overlays and a diagnostics-ready 60-second report;
- a guarded Testing page with all 14 verified retail runtime cheats and graphics-mode switching;
- clean macOS window/menu quitting.

Metal renders the game internally at its native **1280x720**. Bilinear, Sharp, and MetalFX Spatial
can scale that image to the window or Retina display, but they do not increase the game's internal
rendering resolution.

A recent repeatable Dam run averaged **59.6 FPS** with a **57.4 FPS window 1% low**, but this is not
a claim of locked 60 FPS on every Mac or scene. The main work now is broader mission coverage,
lower-power Mac performance, physical controller acceptance testing, and local multiplayer.

For implementation details, evidence, milestones, known gaps, and the exact next priorities, read
the [native Metal technical status](docs/GOLDENEYE_NATIVE_METAL_PROJECT_STATUS.md).

| Platform | Status |
| --- | --- |
| Apple Silicon macOS | Active development; v0.4.1 reaches first-mission gameplay |
| Windows and Linux | Backend code exists, but this project's current changes are not verified there |

## Build and contribute

Source builds require an Apple Silicon Mac, current Xcode Command Line Tools, CMake, Git, and a
compatible local `default.xex` that you are authorized to use. Game data and generated game code
are intentionally excluded from Git.

- [Development guide](docs/DEVELOPMENT.md) — configure, build, code generation, tests, manual runs,
  input diagnostics, and Metal profiling
- [macOS distribution](docs/MACOS_DISTRIBUTION.md) — app packaging, Developer ID signing,
  notarization, ZIP, and DMG creation
- [Contributing guide](CONTRIBUTING.md) — development expectations and pull requests
- [Technical status](docs/GOLDENEYE_NATIVE_METAL_PROJECT_STATUS.md) — architecture, proof,
  milestones, blockers, and roadmap

Developers with a completed local source build can also double-click
[Launch GoldenEye.command](<Launch GoldenEye.command>) to run the build-tree version with Metal and
native input selected automatically.

## Game data, licensing, and trademarks

This repository does not include the original XEX, generated recompiled C++, audio banks,
captures, or extracted game assets. Do not commit or request downloads for those files. Each user
is responsible for supplying compatible files they are legally authorized to use.

This is a multi-license source tree. See [LICENSE](LICENSE),
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and the license files inside vendored and
submodule directories.

This project is unofficial and is not affiliated with or endorsed by any game publisher, console
manufacturer, or trademark owner. Product names are used only to identify compatibility targets.
No trademark or game-content rights are granted by this repository.
