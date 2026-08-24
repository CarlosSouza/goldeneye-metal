# GoldenEye Metal project status

Last updated: August 23, 2026

GoldenEye Metal recompiles the game for Apple Silicon and renders its real GPU work directly with
Metal. It does not use Vulkan, MoltenVK, or a full-system emulator. Game files are not included.

## Where it stands

The intro, menus, briefing, and Dam are playable. The native launcher handles local game-data
import, saves, crash recovery, Safe Mode, settings, and diagnostic export. Keyboard/mouse and up to
four modern controllers work, including remapping and local split-screen.

The current development tree also has repeatable automated checks for:

- real Dam movement, camera, firing, graphics switching, and clean quit;
- real two-, three-, and four-player joins, independent movement, and reconnects;
- native Metal command submission, resolves, multiple render targets, MSAA, and presentation;
- one-frame private GPU capture for difficult rendering reports; and
- pausing the guest while Host Settings remains responsive.

Recent local runs held about 60 FPS in Dam and two- or three-player tests. A repeatable four-player
Stack test now holds roughly 56-59 FPS with a 56-58 FPS 1% low on the M3 Ultra test machine. That
workload selected a 128-draw Metal submission batch as the new default. These are development
measurements, not a promise for every Mac or scene.

## What is still unfinished

- Later missions need the same depth of real-player coverage as Dam.
- Four-player performance still needs validation on lower-power Macs and more maps.
- A default-off exact Xenos output-merger path now keeps validated work in Metal through resolve;
  broader title coverage is still needed before it can become the default.
- Physical controller, lower-power Mac, long-session, display, and Gatekeeper testing still needs
  a wider tester pool.

The project is therefore playable and substantially native, but not yet a finished full-game
release. Local engineering can continue without testers; final compatibility and campaign
acceptance cannot.

Build and test commands are in [DEVELOPMENT.md](DEVELOPMENT.md). Player setup is in
[PLAYER_GUIDE.md](PLAYER_GUIDE.md).
