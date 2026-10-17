# Exact write windows and a low-latency frame limit (Windows, GTX 1650)

Measured on Windows 11, i5-10400F, GTX 1650 4 GB, 16 GB RAM, 1920x1080, 164 Hz display,
`BB_PRESENT_MODE=Immediate`, `BB_VBLANK_HZ=480`. Each run: a fight picked from the menu
("Fight Now"), 90 s measured from `frames.csv` (`tools`-style marks), the player fighting.

## What was left after UFC-48

With 0.1.5 defaults (`BB_WRITE_WINDOW=1 BB_WRITE_STEP=1 BB_PREUPLOAD=1`) frames alternated
fast/slow: one frame without a GPU wait (~8 ms), the next waiting for the GPU (~25-32 ms), i.e.
51.5 FPS on average but a mean jump of 15.9 ms between consecutive frames and only 16 % of frames
in 12-17 ms. The remaining readbacks (~25/s) came from two sites, both "near GPU" writes the
16-byte margin of `DecideWriteStep` refuses to open a window for:

- guest `+0x431c4f`: return of a `memcpy(dst, src, n * 16)` in guest function `0x431ba0`, which is a
  D3D11-style `Map(resource, 4 = WRITE_DISCARD)` / `memcpy` / `Unmap`: the game fills a fresh dynamic
  buffer placed right after bytes the GPU wrote. The copy never touches those bytes.
- guest `+0x4219b8`: `vmovups %xmm0, (%rax)` in a loop copying 48-byte elements (called through a
  vtable from `+0x409dfe`), the same pattern from guest code.

(Disassembled with `llvm-objdump` over the prepared `eboot.elf`, vaddr 0 at file offset 0x4000.)

## Change

- `src/runtime.c`: `guest_memcpy`/`guest_memmove`/`guest_memset` publish their destination in
  thread-local storage (`runtime_guest_copy_range`, two stores per call; signal fences keep them
  ordered for a fault handler on the same thread).
- `page_manager.cpp`: `DecodeGuestStore` decodes the faulting guest instruction with Zydis (a
  static decoder of its own; `Common::Decoder`'s singleton is created unguarded) and, when it is one
  plain store (one explicit memory destination of at most 32 bytes; no REP string op, push/call,
  `enter`, xsave/fxsave or masked store; 64-bit addressing, no FS/GS), computes its effective
  address from the context and checks that it contains the fault address.
- `DecideWriteStep`: when the exact bytes are known, they decide instead of the margin, both ways:
  a write that covers GPU bytes reads back even if it faults away from them (a same-value write
  would escape the window diff); one beside them can step or open a window. Copies are rounded
  out to 16 bytes, decoded stores to 4.
- `BB_WRITE_WINDOW=2`: windows only for writes with an exact range; anything else is handled as
  with `BB_WRITE_WINDOW=0`. The decoded store range is used only with 0 or 2, so the default (1)
  keeps the margin for guest stores. UFC-48's per-site bar still applies.
- A page whose GPU bytes were all taken over by window writes stops being GPU-modified on the GPU
  side (no read watcher left with nothing to download).
- `BB_GUEST_FPS_LIMIT`: the frame limit waits on the game's thread after `sceGnmSubmitDone`
  (outside the submit lock), so the next frame reads the pad after the wait and finished frames
  flip at once. `BB_FPS_LIMIT` keeps holding finished frames in the flip queue, which adds latency.
- Read fault sites are reported like the write fault sites (cost only on the read fault path).

## Results (90 s, fight picked by the player, so scenes differ between runs)

| Run | FPS | Frames 12-17 ms | Mean jump between frames | Frames > 33 ms | Readbacks/s |
| --- | --- | --- | --- | --- | --- |
| 0.1.5 defaults | 51.5 | 16 % | 15.9 ms | 31 | 25 |
| 0.1.5 + this, `BB_WRITE_WINDOW=2` (3 runs) | 60.0 / 60.5 / 64.5 | 72 / 71 / 81 % | 1.6 / 1.3 / 0.4 ms | 78 / 57 / 16 | ~4 |

The frames over 33 ms depend on the fight: in the first two runs most of them came with a CPU read
of GPU-written bytes on a page whose write no longer read back (the read waits for the GPU
instead); in the third, read faults during the fight were 0.02/s. The remaining read fault sites
are mostly at load time (the shader decoder reading shader code on GPU-written pages, and the
game's asset streaming at `+0x1262a2a`). The GTX 1650's 4 GB also report texture-cache memory
pressure in every run (`Texture cache: memory pressure`), with or without `BB_PREUPLOAD`.

Input latency: on the same build, `BB_FRAMES_AHEAD=1` kept the frame rate (64.4 FPS, 0 frames over
33 ms) and `BB_GUEST_FPS_LIMIT=60` with it lowered the GPU wait per frame from 4.7 to 1.9 ms; the
player preferred it over `BB_GUEST_FPS_LIMIT=62 BB_FRAMES_AHEAD=0` (no GPU wait at all, but a
6 ms mean jump between frames).

Second fights loaded without a crash with `BB_WRITE_WINDOW=2` on the earlier base; with
`BB_WRITE_WINDOW=1` before UFC-48, the same machine crashed loading the second fight
(guest fault -1 in asset streaming).

## Known limits

- A window still lets later CPU writes on the open page through unchecked, as before; a write of
  the same value as the stale copy of a GPU byte is not seen by the diff. The exact ranges only
  decide the first write.
- Built and measured with the Windows (llvm-mingw) toolchain only; the Linux build was not run.
  The Python test suite was run on Windows; its failures there are in Linux-only tests (overlay,
  symlinks, packaged Vulkan) and are unrelated.
