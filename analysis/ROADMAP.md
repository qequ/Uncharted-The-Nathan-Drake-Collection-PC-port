# Roadmap: our plan against this port

16 October 2026. Our own port (`D:\ps4_ports\uncharted_pc`: FelipePrado0/gow3_pc at `769dec6` plus
branch `uncharted`, Phase 1 done) is no longer the base. Work continues on this repository,
BilalReloaded's port ([bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port](https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port),
remote `upstream`), on branch `uncharted`. This file maps our roadmap onto what the port already
does and lists what is left. The port's own documents: [PLAN.md](PLAN.md), [STATUS.md](STATUS.md).

The port is GPL v2 or later. Its author asks that reuse credits the project and links back to it.

## Our roadmap item by item

| Our item | In this port | What is left |
|---|---|---|
| **1.1** Runtime functions found by name (`runtime_nid.c`) | Not here: every NID is listed by hand in `gow3_pc/src/import_names.inc` | Port `runtime_nid.c` (our commit `f718485`) when a missing import has to be written |
| **1.2** Prepare any executable | Done: `tools/probe_uncharted.py --executable`; `prepare.py` and `link_modules.py` take `--executable` | - |
| **1.3** `sceSystemServiceLoadExec` | Done: the runtime exits with code 30 and writes `GOW3_LOADEXEC_FILE`; `probe_uncharted.py` prepares and starts the requested executable with its argv (`eboot.bin ispartofuccollection`). Tested 15 Oct: Collection menu to Uncharted 2 | - |
| **1.4** Title table | Done differently: `GAMES` in the launcher, `GAME_KEYS`/`PATCHES` in `probe_uncharted.py` | - |
| **2.1-2.3** Imports: 66 missing | 51 implemented (fibers and Videodec2 in `gow3_pc/gpu/shim`, sockets, PlayGo, NpTus, NpSignaling, NpScore and Remoteplay in `gow3_pc/src/runtime_services.c`) | 15 missing, never called in tests: `sceKernelLoadStartModule`, `Dlsym`, `GetModuleInfo`, `GetModuleInfoForUnwind`, `GetModuleList`, `ConvertUtcToLocaltime`, `ConvertLocaltimeToUtc`, `Utimes`; `sceAppContentAddcontMount`, `AddcontEnqueueDownload`; the four `sceErrorDialog*`; `sceInvitationDialogTerminate` |
| **2.4** Dump check | Ported: `gow3_pc/scripts/verify_dump.py` (tests: `gow3_pc/tests/test_verify_dump.py`). `sce_sys/psreserved.dat` is zero by design and no longer reported | Correction: the "damaged" CUSA02344 dump came from the old PKG extractor (LibOrbisPkg's PFSC reader returned parts of 64 KiB blocks), not from the console. The CUSA02343 dump extracted on 15 Oct passes: 15,174 files, no damage |
| Known problem **1**: submission lock | Open: the upstream `submission_lock` after `WaitGpuIdle` is kept (`gnmdriver.cpp`). No black-screen symptom is reported | Keep in mind for heavy Uncharted 2 scenes (GPU ticks block ~44% of the time) |
| Known problem **2**: early flip signal | Partly worked around: the flip interrupt goes through `RunInOrder(SignalFlip)` after the deferred signals, but still before the flip command buffer's end-of-pipe write. `GPU_WAIT_PATCH` (`probe_uncharted.py`) stops the game's 2-second wait from asserting | Uncharted 2 `m_gfxEopTick` assert with multithreaded rendering and 2 frames queued (STATUS.md) |
| Known problem **3**: unbounded buffer descriptors | Worked around: a V# whose clamped size is 1 GiB or more is bound as null (`vk_rasterizer.cpp`) | - |
| Known problem **4**: invisible scene after the first cutscene | Fixed without precise readbacks: small GPU writes are copied back every submission (`SyncSmallGpuWrites`, `GOW3_SYNC_GPU_CONSTANTS=1`) | - |
| Known problem **5**: `FindBuffer` assertion | Cannot happen: the buffer cache was rewritten around sparse arenas | - |
| **3A** Fibers, **3B** Videodec2 | Done | - |
| **3C** Kernel and sockets, **3D** services and PSN | Partly done | The 15 imports above |
| **4** Graphics, Uncharted 1 playable | Plays at 60 FPS in the tested areas (our test: the boat level) | Not played start to finish; smoke blob and bullet-hit circles; native 4K (renders at 1620p + FSR 1) |
| **5** Uncharted 2 and 3 through LoadExec | Done: game switching, saves, audio, movies, input | Uncharted 3 crashes with the default asynchronous, unprotected readbacks (their issues #1 and #2; our test: the London pub fight after ~2 minutes; it held with `GOW3_ASYNC_READBACK=0`). Heavy Uncharted 2 scenes at 40-45 FPS. Memory grows ~0.8 MB/s |
| **6** Patches and packaging | Done: render resolution and memory map patches found by pattern (720p to 4K), launcher with presets, Windows package, releases with an updater, update 1.02 | - |
| **7** PC controls | Partly: per-game key and mouse remapping in the launcher's Controls tab, mouse look | 7.1 key glyphs instead of PS4 buttons; 7.2 mapping in the game's Options, Controls; 7.3 the runtime's dialogs (see our old roadmap text below) |

## Added on our branch

- `verify_dump.py` (above).

## What is left, in order

1. **Uncharted 3 stability**: readback modes 0 and 1 in longer sessions; the pub fight crash.
2. **Play testing** with crash reports that show where a crash happened, then fixing what they
   point at.
3. **Phase 7, PC controls**:
   1. *Key glyphs.* Where the game draws a button prompt (Uncharted 3's pub fight: "Press □ to
      attack"), show the bound key or mouse button, and Xbox glyphs on an Xbox pad. Lead:
      upstream gow3_pc's controller-dependent button icons for God of War III (`9155cdc`,
      PR #192 `feat/controller-glyphs`), merged after this port branched.
   2. *Mapping in the game's Options, Controls* instead of the launcher tab and the in-game
      overlay, saved per game. The games' Controls page has no remapping, so this means extending
      that page (patching its menu code) or a page drawn in the game's own menu style.
   3. *The runtime's dialogs* (save slots, messages) follow the same bindings and the game's look.
4. **Newer upstream gow3_pc**: this port branched at `08b300a` (13 Oct) and is 70+ commits behind
   (AMD fixes, PR #106; controller glyphs, PR #192). `gow3_pc/` is a plain folder here, so the
   merge is a subtree merge.
5. **The 15 missing imports**, with `runtime_nid.c`, if a test reaches one of them.
6. The port's own open items: heavy Uncharted 2 scenes, the multithreaded-rendering assert,
   memory growth, Uncharted 1's smoke and bullet-hit circles, Uncharted 1 at native 4K.
7. **Update 1.02 for CUSA02343**: extract it next to the game folder as `<game folder>-patch`;
   the port finds it there.
