# UFC-77: issue #44, menu images load late and the menu is not smooth (Steam Deck)

Issue #44 (@SercanDmr, Steam Deck) has no log. This analysis uses the `Logs.zip` from issue #47
(same user, same machine, v0.1.9): `20261020_090243.log`, `.frames.csv` (one line per flip) and
`.readbacks.csv`. The session starts in the menus (0-96 s), goes into Fight Now and a fight
(96-4820 s) and ends back on the home page (4821-4831 s).

## Setup seen in the log

- Steam Deck OLED (gamescope reports 90 Hz, then `refresh_hz=72`: the screen runs at 72 Hz),
  RADV VANGOGH, Mesa 26.2-devel.
- **A third-party Vulkan frame-generation layer is loaded**: `VK_LAYER_MAKO_render`
  ("MAKO Renderer", lsfg model), `target-driven frame generation enabled; mode=adaptive,
  target=70 fps, maximum multiplier=2x, stable cadence=enabled`, presentation `fifo-ordered`.
  It sits between the game's swapchain and gamescope; nothing it does is in our frame log.
- Launcher: FPS limit 0 (`VideoOut: vblank 480 Hz, frame limit 0 FPS`; the launcher default
  is 120), `BB_FLIP_RATE=0`, present mode Mailbox.

## What the frame log shows

**The menu does not run uncapped.** 0-96 s, 6535 flips: median 13.91 ms (72 FPS, the
screen's refresh), 91% of flips between 12.5 and 15.5 ms, 64 over 20 ms, 2 over 50 ms. The cap
is the present path (72 Hz FIFO through the layer), not our frame limit: with 12-60 draws per
frame the GPU thread's frame is ~13 ms, ~90% of it kernel time, which is stage A spinning
(`pause` then `sched_yield`, `vk_draw_pipe.h` Drain) at the end of the frame
(`stage A waits: OnSubmit 93%`) while the recording thread waits for the present. So the
"240 FPS uncapped" theory does not hold here.

**Shaders and pipelines are not the menu problem.** 94 compiles in the 91 s of menus, a few
tens of ms in total (`34 shader/pipeline compiles, 10.5 ms`). The large compile burst (475
compiles, 912 ms, worst frame 801 ms) is the first load of the fight scene at 112 s.

**No readbacks in the menu.** The first readback is at 93.5 s (fight loading).

**Image uploads: the real hitches are when a screen loads its images.** Menu steady state
uploads ~51 MB/s (a movie texture, ~1.5 MB per frame) for 0.2 s of refresh time in total, which
is fine. The stalls come when a screen brings many new images at once: entering Fight Now /
fighter select (96-101 s) registered 407 images, 919 MB of uploads in 5 s:

| t (s) | frame | images | uploads | GPU thread CPU | create / refresh / protect | guest write faults |
|---|---|---|---|---|---|---|
| 96.25 | 117 ms | 14 | 23 MB | 76 ms | 0.4 / 4.4 / 1.5 ms | - |
| 98.97 | 195 ms | 64 | 214 MB | 175 ms | 66 / 48 / 13 ms | 76 ms |
| 99.59 | 541 ms | 35 | 375 MB | 470 ms | 109 / 68 / 53 ms | 440 ms |
| 112.40 | 801 ms | 991 | 452 MB | 666 ms | 192 / 150 / 83 ms | 3 ms (+307 compiles) |

The uploads run synchronously on the GPU command thread when a draw first uses the image
(image create, memory allocation, copy, write protection), so the frame waits for all of them;
and while the game's streaming threads write the new texture data, pages that held tracked
textures before fault (440 ms of write-fault handling during the 541 ms frame), which slows
the game's own loading. On a desktop CPU this is a short hitch; on the Deck it is half a second.
That matches "the images take a moment to appear and the menu jerks when they do". It is not
decoding: the game decodes/streams its textures itself (AssetStream threads); we only copy them.

**Other hitches are the game's own.** The 225 ms frame at 45 s (boot flow, `betanotification`)
had the GPU thread idle (2.3 ms of CPU): the game was busy, not the renderer.

**Unknown: the frame-generation layer.** With frames already at 72 FPS on a 72 Hz screen the
layer should not generate frames, but in adaptive mode with a 70 FPS target and a 2 s cadence
probe it can switch the multiplier on and off (it does in the fights, ~35 FPS → 2x). That
cadence switching and interpolated UI frames are a likely source of "not smooth" that our log
cannot see. It is the first thing to rule out.

## Change

None. There is no small, safe fix:

- A default menu frame cap would change nothing: the menu is already held at the screen's 72 Hz.
- Menu pipelines are few and cheap; precompiling them would not be noticed.
- The stalls are the synchronous image path in the texture cache. Making it asynchronous is a
  large change (below).

## Plan if the stalls are to be fixed (large)

1. Measure first on the Deck with the layer off and `BB_FRAME_LOG`: confirm the stalls keep
   their size (they are on our side, so they should).
2. Split image creation from the draw: create the `vk::Image` and its memory on a worker when the
   image is first seen (to check: whether the descriptor is known early enough to gain
   anything), so the GPU thread only binds it. `image create` was 66-192 ms of those frames.
3. Upload large new images (above ~1 MB) through a staging ring on a transfer queue with a
   timeline semaphore, and draw with the previous contents / a placeholder until it signals
   (the game itself streams in this way, so a frame late is acceptable for UI and portraits).
   This needs care with images the GPU also writes and with the readback/write-tracking state.
4. Batch the write protection of a burst of new images into one `mprotect` range per arena
   (protect was 13-83 ms) and look at `BB_UFFD` again on the Deck for the fault cost.
5. Separately: the end-of-frame Drain spins with `sched_yield` for the whole present wait
   (~12 ms per 13.9 ms frame in the menu, a whole CPU of the Deck's 4 cores). A futex wait after
   the spin phase would give that CPU back to the game's streaming threads; needs an A/B on the
   Deck because it touches the draw pipe's hot path.

## Test for the user

1. Turn off the frame-generation layer (the Decky/Lossless-Scaling "MAKO" plugin, or whatever
   loads `VK_LAYER_MAKO_render`) for this game.
2. Steam's performance menu: refresh rate 90 Hz (or 60), framerate limit off. Launcher: FPS
   limit 90 (or 60, to match), present mode Fifo.
3. Start the game, wait on the home page 30 s, open Fight Now, go back. Send the log folder
   (`.log`, `.frames.csv`).

Expected: the menu flips steadily at the screen's rate; the hitch when a screen first shows its
images stays (that is the synchronous upload above), but it should be the only one.

## Draft reply (issue #44)

> Thanks! Your log from #47 covers the menus too, so here is what it shows. The menu is not
> running uncapped: it holds 72 FPS, the refresh rate your Deck is set to, with very little
> jitter on our side. Two things stand out:
>
> 1. A frame-generation layer ("MAKO Renderer", Lossless Scaling) is loaded on top of the game.
>    In adaptive mode it can switch frame generation on and off in menus, which feels like
>    stutter and isn't visible in our log. Could you try once with it disabled, the Deck at
>    90 Hz (or 60) with Steam's frame limit off, and the launcher's FPS limit set to the same
>    value with present mode Fifo?
> 2. When a screen loads many images at once (Fight Now, fighter select) the port uploads them
>    in one go, which costs 0.2-0.5 s on the Deck's CPU. That one is on our side and needs
>    a bigger change to the texture upload path; it's on the list.
>
> If you send the new logs after that test we can tell whether anything else remains.
