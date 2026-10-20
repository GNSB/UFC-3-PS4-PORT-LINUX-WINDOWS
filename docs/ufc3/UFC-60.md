# UFC-60 — Career menu at 15–22 FPS after the 2nd belt (GitHub #36, #18)

## What the log says (i36-20261020_082653.log, v0.1.7, RTX 5070, Windows)

The whole session is the Career menu: save loaded (`CAREERSAVE1`, line 416), menu built
(lines 421–475), the player stays in it (476–528) and exits to the home page (536+). No fight.

| Phase | Stepped writes | Write windows | Buffer downloads |
|---|---|---|---|
| Title / home (240 FPS) | 240/s (one per frame) | 0–0.8/s | — |
| Career menu, slow (lines 500–528) | 83–113/s | 8.4–11.2/s | none for 25 s |
| Fight, other users (i26 l.533–567) | 200–800/s | 145–517/s | hundreds per 5 s |

- The slow menu has **fewer** CPU↔GPU syncs than a smooth fight: no readbacks are reported
  ("readbacks kept 0 CPU bytes"), downloads are 9–16 per 5 s (1.8–2.2 KB). Stepped writes and
  write windows are not what costs 30–50 ms per frame. `BB_WRITE_WINDOW` is not the cause.
- Arena binds happen only while the menu loads (lines 426–472), not while it is slow.
- What the menu does render: the 6-layer 1024×1024 Z16 spot-light shadow map read as raw
  `R16_UINT` (lines 438–443, "Depth as R16_UINT … layer 0..5"), 4× MSAA draws with
  alpha-to-mask (lines 464–468).

## Best-supported hypothesis

The port converts a depth layer read as `R16_UINT`/`R32_UINT` (DepthAsColor,
`vk_rasterizer.cpp`) **at every bind**: each read ends the render pass, runs a full 1024×1024
compute dispatch and two global barriers, and turns off the texture-set memo for the draw.
In a fight the octagon floor's cs 34617c9f reads it 6 times per frame (one per light). A
scene whose materials read the shadow map from many draws pays it per draw: hundreds of render
pass breaks and full-image conversions per frame, with the GPU serialised between them. That
fits a scene that only appears after the belts (more lit objects → a small drop after the 1st
belt, a big one after the 2nd) and leaves fights and other menus alone.

The log cannot prove it (the "Depth as R16_UINT" line only prints when a copy is created), so
this change adds the counter that does.

## Change

- `Image::write_count` (`texture_cache/image.h`): bumped by every `Runtime::Transit` that asks
  for a write access (render/depth target, storage, copy and upload destinations, scene proxy
  resolves) and by `FindDepthTarget`.
- DepthAsColor remembers the source image's `write_count` at conversion; a later bind of the
  same image, layer and backing with the same count reuses the converted image: no render pass
  break, no dispatch. Same pixels: the conversion is a pure function of the depth contents and
  every write to them changes the count. A new backing, size or copy image converts again.
- Every 5 s while depth is read as colour:
  `Depth as color: X conversions/s, Y reused/s (BB_DEPTH_AS_COLOR_REUSE on|off)`.
- **Off switch: `BB_DEPTH_AS_COLOR_REUSE=0`** (convert at every bind, the old behaviour; the
  line still prints, so the "before" number comes from the same build).

## Test for the tester

Windows, Career save after the 2nd belt (the white/gold/black menu), FPS counter on
(overlay, Insert):

1. `set BB_DEPTH_AS_COLOR_REUSE=0` in a .bat before `UFC3.exe`; load the save, stay 30 s in
   the Career main menu. Note the FPS and the `Depth as color:` lines in the log.
2. Same without the variable (default). Note FPS and `Depth as color:` lines.
3. Start one fight from the save (or Fight Now) with the default: FPS as usual, and the
   octagon floor without grain or gray blocks under the spot lights.

Reading it:
- Run 1 shows hundreds or thousands of conversions/s in the menu and run 2 shows them as
  reused/s with the FPS back → this was the cause.
- Run 1 shows only tens of conversions/s (≈ 6 × FPS) → not the cause; keep the change (it is
  harmless) and repeat run 2 with `BB_FRAME_STATS=1 BB_GPU_PROFILE=1`: the `Stall:` lines and
  the `GPU profile:` top labels say where the 50 ms go (GPU passes vs. GPU thread CPU time).
