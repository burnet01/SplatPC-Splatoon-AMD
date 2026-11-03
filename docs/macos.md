# macOS (Apple Silicon) port

The macOS build is the same engine as Windows and Linux: the same recompiled
game code, the same GX2 → Vulkan renderer and the same caches. It compiles
natively for arm64 (no Rosetta) with Apple Clang and CMake, and runs Vulkan
on Metal through MoltenVK, which the app carries together with the Khronos
Vulkan loader.

## Building

```bash
./setup_macos.sh --dump "/path/to/Splatoon dump"
```

Requirements: an Apple Silicon Mac, macOS 13.3 or newer, the Xcode Command
Line Tools (`xcode-select --install`) and Python 3.12+. CMake and Ninja are
installed into `build/tools` with pip when they are missing, so Homebrew is
not needed. The result is `Game/Splatoon.app`; the game data (`content`,
`meta`, `mlc01`, `save`, `vulkan_cache`, `settings.ini`) lives beside it in
`Game/`, as it does beside `Splatoon.exe` on Windows. Content is copied as an
APFS clone, so it takes no extra disk space on the same volume.
`./build.sh` reruns the setup, which skips finished steps.

When the `Game` folder is inside Desktop, Documents or Downloads, macOS asks
once, on the first launch from Finder, whether Splatoon may access that
folder. The game waits at startup until the prompt is answered: choose
Allow. A game folder elsewhere (for example `~/Games`, via `--output`) needs
no prompt.

The setup checks that the SDK links with the installed linker. A Command
Line Tools update can ship an SDK whose library stubs the installed `ld`
cannot read (seen with the macOS 27 SDK and ld-1267); the setup then uses the
newest SDK that links.

## Vulkan implementation

| | MoltenVK 1.4.2 (bundled) | KosmicKrisp (LunarG SDK) |
|---|---|---|
| Vulkan version | 1.3 core used, 1.4 reported | 1.3/1.4 conformant |
| Geometry shaders | no | no |
| Transform feedback | no | not documented |
| Distribution | Apache-2.0 release archive | only inside the LunarG SDK installer |

Neither exposes geometry shaders, which the Windows renderer requires for
GX2 RECT/QUAD lists, nor transform feedback, which it uses for GX2 stream-out.
The renderer now has a path for each that keys on the device's capabilities,
not on the platform, so it works the same on either driver. MoltenVK is
bundled because it can be shipped inside the app. The renderer does not
depend on MoltenVK: setting `VK_DRIVER_FILES` to KosmicKrisp's ICD manifest
selects that driver instead. KosmicKrisp has not been tested here.

Driver settings applied at startup (an environment variable that is already
set wins): asynchronous queue submission, Metal's concurrent pipeline
compilation, fast math off (Latte shaders depend on IEEE NaN/Inf/signed-zero
behaviour), and error-only driver logging.

## What differs from Windows, and why

**Geometry-stage work in other stages.** The device reports
`geometryShader = false`:
- QUAD/QUAD_STRIP lists are expanded into the same two triangles per quad
  that the assembly geometry shader emits, in the same winding.
- RECT lists are assembled in the vertex stage. Each rectangle is drawn as one
  instance of six vertices. Every vertex invocation runs the original vertex
  program for the rectangle's three corners, whose attributes arrive through
  three instance-rate copies of each binding. It then emits the same strip
  corner (`b + c − a`) the geometry shader builds. Indexed or instanced
  rectangles are skipped with a log line; none were seen in testing.
- Triangle fans (excluded by the portability subset) become their Vulkan list
  decomposition.

**Stream-out without transform feedback.** The vertex program stores its
exports into a storage buffer, the decompiler's existing SSBO mode (Cemu uses
it on Metal drivers). The results are copied back to guest memory at the same
publication points as the transform-feedback path. The binding and uniform
offsets are derived from the translated source, so the on-disk translation
cache format is unchanged. Program keys gain a tag only on devices without
transform feedback, so Windows and Linux caches keep their keys.

**Write tracking (Windows `GetWriteWatch`).** macOS has no dirty-page query.
`gx2_write_watch_apple.h` answers the same queries from page protection. The
shared Windows logic (epochs, windows, memoization) runs unchanged on top of
it. Measured on an M5: a write fault plus unprotect costs about 3 µs per
16 KiB page, while comparing that page costs about 0.3 µs. So pages found
written at three consecutive queries stay writable and are reported written
every time, which is what Windows reports for memory written every frame;
every 64 queries such a page is protected again. Kernel reads into guest
memory (`read(2)`, `recv(2)`) pin their pages writable, because the kernel
cannot take the fault. The display-list sweep needs "every page ever
written", which protection cannot provide, so it stays off (as on Linux).
`RBREW_WRITE_WATCH_DISABLE=1` turns tracking off for comparison.

**ARM64 memory ordering.** x86 keeps the store order that guest `sync`,
`lwsync` and `eieio` request, so the recompiler used to drop them. ARM64 does
not keep it. They now emit `RBREW_GUEST_SYNC/EIEIO/ISYNC`, which become
`dmb ish`/`dmb ishst`/`dmb ishld` on ARM64 and nothing on x86, so x86
machine code is unchanged. Decoding and block boundaries are unchanged too.

**CPU paths.** NEON replaces SSE2/SSSE3 for guest index and word byte-swaps.
`ISB` replaces `PAUSE` in spin-waits; Apple cores retire `YIELD` in a cycle,
which would shorten every calibrated spin window. The runtime is linked with
ThinLTO, the Clang equivalent of the Windows runtime's `/GL` + `/LTCG`.

**Scheduling.** Each Windows `SetThreadPriority` site has a macOS QoS class:
- the render and recorder threads run at `USER_INTERACTIVE`;
- guest threads and demand shader compiles run at `USER_INITIATED`, which
  keeps them on the performance cores;
- warm-up compiles run at `UTILITY`.

A compile worker switches class per job, so a draw waiting on a shader is
never compiled on an efficiency core.

**Paths.** `main` makes the folder that contains `Splatoon.app` the working
directory, because Finder starts apps in `/`. The shader cache and the
controller log go there rather than into the bundle, which an update replaces.

## Measured on this machine (Apple M5, 16 GB, macOS 27.0.1)

- Full build: the 590 generated files plus the runtime compile in 237 s with
  6 jobs.
- Runtime test suite (`RBREW_BUILD_TESTS`): 61/61 pass on the Apple M5 through
  MoltenVK; none skipped. The synthetic Plaza workload (1,664 draws/frame)
  records in 0.95 ms median CPU per frame.
- Game:
  - boots, creates a save, and renders character creation and the tutorial
    (3D scenes, shadows, ink painting, lens flare, UI) at 60 FPS (frame-dump
    timestamps);
  - uses about 1.5 cores in total in tutorial gameplay with diagnostic
    logging on;
  - rendered output was inspected from frame dumps (`RBREW_FRAME_DUMP_DIR`).
- Write tracking on vs. `RBREW_WRITE_WATCH_DISABLE=1`, same boot sequence,
  30 s windows, release build: 26.2 / 26.5 vs 28.3 / 28.7 CPU-seconds
  (about 8% less CPU with tracking).

## Not tested yet

- Inkopolis Plaza, Octo Valley and online-free multiplayer stages (the test
  save only reached the tutorial). Plaza NPC-count CPU time on macOS is
  therefore only covered by the synthetic Plaza/NPC benchmarks above.
- Controllers, gyro, keyboard/mouse by hand (input was scripted through
  `RBREW_INPUT_SCRIPT`), and audio output quality (the device opened).
- Stream-out draws and RECT draws in real game scenes: the paths compile and
  no failure was logged, but no scene that is known to use them was reached.
- Flat-shaded varyings in QUAD lists may take another corner's value: no
  provoking-vertex control on Metal.
- KosmicKrisp.

## Shader cache and pop-in

The driver pipeline cache (`Game/vulkan_cache/pipelines.bin`, which holds
MoltenVK's converted Metal shaders) was written only when the game closed
normally, so a force-quit or crash lost it and the next launch rebuilt every
pipeline. Objects whose pipeline was still being built were skipped by the
asynchronous shaders and appeared late. The cache is now also saved in the
background during play: at most once a minute, and only when new pipelines
were built. The save no longer holds the lock that every pipeline build
takes. With the cache present, the boot warm-up built all 2,149 recorded
pipelines 2.3 s after launch (M5, diagnostic build), and only 3 draws were
skipped in that session. Shaders the game has never shown before still
compile once, in the background.
