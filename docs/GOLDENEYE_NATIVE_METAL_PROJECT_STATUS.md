# GoldenEye Metal project status

Last updated: July 24, 2026

GoldenEye Metal runs recompiled game code on Apple Silicon and translates the original Xenos GPU
work directly to Metal. It does not use Vulkan or MoltenVK, and it is not a traditional full-system
emulator.

No game files are included. Players must provide a compatible backup they are legally allowed to
use.

## Current state

The game boots through the intro, main menu and Dam briefing, then plays the Dam mission using the
real game command stream, shaders, textures and geometry.

Working now:

- Native Apple Silicon and Metal rendering
- Keyboard, mouse and modern PlayStation/Xbox controllers
- Original/remastered graphics switching
- Fullscreen, V-Sync, scaling, filtering, anti-aliasing and colour controls
- Native launcher with game-data import and verification
- Save management, crash recovery, Safe Mode and diagnostic export
- Controller presets, remapping, rumble and live input testing
- Stable P1-P4 controller ports with live testing and manual reassignment
- Verified 2–4 player local split-screen joins and Metal viewport layouts
- Proper pause while host settings are open
- Testing page with all 14 verified runtime cheats
- Signed and notarized macOS 14+ packaging

This remains an experimental port. Dam is the main tested area; later missions and multiplayer have
not had the same level of validation.

## Metal progress

The production path is fully native ARM64 and Metal. Presentation no longer depends on full-frame
CPU readback, replacement geometry or synthetic frames.

The current v0.3 work adds native D24S8/D24FS8 depth and stencil resolves, correct endian
conversion, selected-sample handling for 1×/2×/4× MSAA, and normal depth-texture sampling. It also
reconstructs GoldenEye's three-part post-processing restore as one native 1280×720 colour/depth
input. Bogus near-maximum descriptors are still rejected; the restore is enabled only for the exact
known producer and consumer sequence.

Metal completion fences now use bounded event wakeups instead of blind polling, and CPU fence waits
back off without weakening the real game dependency. A fresh Dam run on an M3 Ultra averaged
59.6 FPS with a 57.4 FPS window 1% low and no missed fences, restore failures or timeouts.
Lower-power Macs still need focused testing.

## Main limitations

- Performance and frame pacing vary by scene and Mac.
- The reconstructed post-processing restore is currently matched to the verified Dam command
  sequence; unknown variants fall back safely instead of being guessed.
- Visual and stability coverage beyond Dam is still limited.
- Physical controller testing is incomplete across every supported model.
- Longer split-screen matches still need broad physical-controller testing.

## Next priorities

1. Validate the restored post-processing path in more missions and graphics modes.
2. Validate frame pacing and synchronization improvements on lower-power Macs.
3. Expand repeatable mission and local-multiplayer validation beyond Dam.

## Milestones

| Milestone | Status |
| --- | --- |
| Native Metal presentation and real game shaders | Complete for tested scenes |
| Main menu and playable Dam | Complete, with ongoing fidelity work |
| Native launcher, input, saves and diagnostics | Complete |
| Native depth/stencil resolve and normal depth sampling | Complete for tested paths |
| Full-game fidelity and stable 60 FPS | Not complete |

Build and test details are in [DEVELOPMENT.md](DEVELOPMENT.md). Player instructions are in
[PLAYER_GUIDE.md](PLAYER_GUIDE.md).
