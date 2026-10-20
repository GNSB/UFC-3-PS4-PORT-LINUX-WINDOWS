# UFC-65 — FPS slowly drops over a long session (GitHub #24, #32)

Steam Deck (Linux, 0.1.6): about 5–6 FPS lost over an hour of quick fights, no Career, no log.
Radeon 680M (#32): drops after changing arenas. Both are integrated GPUs.

No log of a long session on an integrated GPU exists, so this ticket does two things: it adds a
counter line that a one-hour log can be read with, and it bounds the one structure the logs we
have show growing without limit.

## What grows (ranked)

1. **Texture cache: registered images, never collected below the trigger.** The collector
   (`GarbageCollectImages`) returns at once while the usage is under `trigger_gc_memory`. On
   integrated GPUs that trigger is 70 % of the driver's budget (several GB of shared memory on a
   Deck), so in practice nothing is ever evicted.
   Evidence, the five longest logs we have (RTX 3060 Laptop, 6 GB, trigger 3658 MiB, Windows,
   `Memory:` lines with `BB_FRAME_STATS=1`):

   | log | minutes | cached images first → last | MiB | collector usage | images freed |
   |---|---|---|---|---|---|
   | release-2e700eb-TESTexe | 12 | 25 → 3011 | 176 → 1540 | 767 → 3132 | 0 |
   | release-2e700eb-windows-on | 9 | 11 → 3532 | 102 → 1494 | 667 → 2987 | 0 |
   | UFC48-test | 11 | 11 → 1780 | 102 → 1233 | 655 → 2548 | 0 |
   | UFC48-test5 | 8 | 25 → 2985 | 176 → 1516 | 751 → 3052 | 0 |
   | UFC2-diag | 7 | 11 → 2047 | 102 → 1501 | 663 → 2963 | 0 |

   The count jumps at every new scene and never goes down. The cost that follows the count: the
   1 MiB lookup pages (`page_table`) keep every image registered over them, and each lookup that
   misses the memo (`FindImage`, `FindImageFromRange` after any registration change), each CPU
   write notice (`InvalidateMemory`, thousands a second in fights) and each GPU-written storage
   binding (`InvalidateMemoryFromGPU`) walks those lists under the texture cache mutex. The game
   reuses a texture pool per fighter/arena: the old images over it stay registered next to the
   new ones (ResolveOverlap frees only same-address, incompatible, old ones). On the Deck the
   memory also comes out of the 16 GB the game and the system share.
   **Fix below.** Whether the walk is what costs the 5–6 FPS is what the counter line says
   (`lookup pages … entries, longest …` against `us/draw`).

2. **Guest thread stacks on Linux are never unmapped** (`src/runtime_thread.c`,
   `platform_thread_stack` → `runtime_low_map`). Each guest thread gets its stack + 256 KiB from
   the low region and nothing gives it back; the `GuestThread` record and its TLS block are not
   freed either and stay in the `threads` list (`find_thread` walks it). One Career session
   (users-20261016/20261016_191120.log, Windows, where winpthreads frees the stacks itself)
   created 198 `MoviePlayer2 Decode Thread` (640 KiB each) and 81 `EntityLoader` (1.25 MiB each):
   on Linux that is ~225 MiB never unmapped, resident as far as the stacks were touched. Memory, not frame
   time: not fixed here (unmapping a joined thread's stack is easy, a detached one's needs a reaper
   and glibc keeps its descriptor at the top of a user-supplied stack). Separate ticket if the
   RSS in the counter line grows by hundreds of MiB.

3. **Reduced-resolution scene proxies are never freed** (`SceneTargets::entries`): one Vulkan
   image per 1920×1080/960×540 target, released only when the preset changes (`SetSize`), not
   when the target's image dies. Dormant in UFC 3: every log says
   `Upscaler: FSR 3.1 available (off)`, and with the default preset (native) nothing is reduced.
   Counted in the line (`scene proxies`); fix only if it ever shows a non-zero, growing number.

4. **Looked at, bounded, not suspects:** pipelines and programs (hash maps, grow only with new
   shaders: compiles, not a steady cost), descriptor sets (pools recycled by tick), samplers
   (own collector), buffer-cache arenas (one per 4 GiB of address space; old ones after a
   migration stay in the list but there are a handful), `gpu_write_frames` (pruned at 65536),
   `large_writes` (cleared every 5 s), write-window audit (off by default, capped at 1024),
   DepthAsColor copies (swept when their image dies; UFC-60's area), the draw-pipe keepalive and
   GPU signal deques (popped by tick), `SynchronizeDmaBuffers` (walks every resident range, but
   only for shaders using DMA, off unless `BB_DIRECT_MEMORY_ACCESS=1`). Buffer-cache interval
   lists (`resident_ranges`, `in_place_blocks`, …) are sorted vectors with O(n) inserts; they are
   bounded by the game's memory but could fragment: counted in the line.

## Changes

- **Counter line, every 60 s** (`renderer_vulkan/vk_growth_report.cpp`, called from
  `Rasterizer::OnSubmit`), always on; `BB_GROWTH_SECONDS=N` changes the period, `0` turns it off:

  ```
  Growth (60 s): 59.8 FPS, GPU thread 14.2 us/draw, 712 draws/frame; images 3011 (1540 MiB;
  2100 idle 60 s, 900 CPU-overwritten, 400 GPU-written, 1500 tracked), 12 stale freed
  (BB_GC_STALE_SECONDS); lookup pages 1200 with 8000 entries, longest 140; views 5000,
  samplers 300; collector usage 3132 of 3658 MiB; pipelines 1500 graphics, 40 compute,
  900 programs, 0 compiling; buffer cache 3 arenas, intervals: 200 resident, 50 in place,
  30 GPU-modified, 20 GPU-written, 10 assets; 500 CPU-written blocks, 40 GPU write ranges,
  6 VRAM chunks; scene proxies 0; Vulkan memory 3330 MiB (VMA 1500 MiB, images 1639 MiB);
  RSS 4500 MiB
  ```
  (one line in the log; the numbers above are made up). FPS is game frames shown in the window;
  us/draw is the GPU command thread's CPU time per draw (comparable when the scene differs).

- **Stale images go at any memory usage** (`TextureCache::GarbageCollectImages`): every 5 s, up
  to 64 registered images that the CPU has overwritten since their last use (`CpuDirty`) and
  that nothing used for 60 s are freed. Such an image is uploaded again from memory at its next
  use anyway, so freeing it loses nothing; GPU-written images with data only on the GPU are
  never `CpuDirty`, MSAA ones and bound targets are skipped. Images whose memory is unchanged
  stay cached as before. **Off switch: `BB_GC_STALE_SECONDS=0`** (`=N`, 1..63: idle seconds).
  The `stale freed` count in the line shows how much it does.

## Test (one run, about an hour)

Steam Deck or another integrated GPU (Radeon 680M) if possible, else any PC. FPS counter on.

1. Default settings. One hour of quick fights, changing the arena every fight or two. Keep the
   log.
2. If FPS still drops: the same hour with `BB_GC_STALE_SECONDS=0`, for comparison.

Reading the log: take the second `Growth` line (the first window may include loading) and the
last one from a fight (not a menu), and compare:
- `FPS` and `us/draw` — the drop and whether it is GPU-thread CPU time (us/draw up) or not.
- `images`, `lookup pages … entries, longest` — with the fix they should level off; without it
  (run 2) they climb all hour. If us/draw climbs with `longest`, this was it.
- `intervals: … resident … in place` — climbing with us/draw points at the buffer cache's
  interval lists instead.
- `RSS`, `Vulkan memory` — climbing by hundreds of MiB: memory leak (suspect 2), not frame time.
- `scene proxies` other than 0: suspect 3 is live.
- Nothing grows but FPS still falls: not a port-side cache; next look at the game itself
  (`Arena bind` lines, guest thread counts) or the driver (thermal throttling on a Deck shows
  as GPU time without any of the above changing — check the Deck's performance overlay clock).
