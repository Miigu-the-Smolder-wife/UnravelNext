# Request: one-path VSM, with the page pool as a depth atlas (S track, 2026-09-26; INTERFACES 5.x FrameResources, 7.3)

## Why
- This is M0 item 2 (coordinator order: loop caps, then sun-shadow one-path correctness).
- Design revision 1, 14.2 ④ (adopted by S at 20:00 on 2026-09-25): no page cache, no dirty tracking, no split per-level basis updates, no wind metadata. Every requested page is drawn every frame.
- Since the user's decision that game-day speed is unlimited, the frame budget row already charges a full redraw every frame. A cache cannot lower that budget; it only adds management passes and invalidation bugs.
- 11.4 (6), S idea 1: "atlas = pool". Lookups read the hardware depth atlas directly. The encode pass (clear 0.46 + page max 0.71 ms [measured, city 4K]) goes away, and only the block hierarchy is built.

## Change (core: FrameResources.h; M and R: one line each)
- `FrameResources::vsmPool` (BufferRef, raw) becomes **`TextureRef vsmAtlas`**.
  - It is a Texture2D<float>, depth format D32_FLOAT. The SRV is R32_FLOAT.
  - Later, D16_UNORM / R16_UNORM once 11.4 (6) passes its gate. The format flag goes in VsmConstants; the header decodes it, so consumers do not change.
  - Physical page p sits at pixels ((p % 128) × 128, (p / 128) × 128), 128² texels per page.
  - Value:
    - sun pages: (h − hMin_k)/(hMax_k − hMin_k), the level's caster height range;
    - local-light pages: reversed-Z depth of the face view;
    - 0 means no caster (the clear value).
- `ShadowSrvs.pool` (ShadowVisibility.hlsli) keeps its name and position. It now holds **the SRV of vsmAtlas**. The header reads it as `Texture2D<float>`.
- Consumers:
  - M `ShadingSystem.cpp`: in `for (BufferRef vb : { ..., r.vsmPool, ... })`, take vsmPool out and add `b.use(r.vsmAtlas, Use::SrvCompute)`. Use `c.srv(r.vsmAtlas)` in the words array.
  - R `RayScene.h` `VsmRefs`: the `pool` member becomes `TextureRef` from `r.vsmAtlas`. Ray kernels declare it SrvGraphics / SrvCompute as before.
  - FX particles: `shadowSunVisibilityInAir`, same rule.
- Timing: the switch is one S commit. I'll announce its hash. Core's field change and the M/R one-line changes must land in the same window, because a buffer descriptor read as a texture is undefined. Until then, `vsmPool` stays as it is.

## S side (for reference)
- Per frame:
  1. Page requests as now: pixel, air and local marks, then propagation.
  2. A deterministic prefix sum over the request bits gives each requested slot its physical page. The page table is rewritten in full; unrequested slots get 0.
  3. Cull mask + atlas slot words (`DepthRasterRequest::atlasSlots`).
  4. Clear the atlas (depth fast clear to 0).
  5. V atlas raster: sun levels, local faces. It is depth only, with no pixel kernel.
  6. Block hierarchy and page max height from the atlas.
  7. Search bound grid.
- Removed passes: release, free list, moved, invalidate (sun and local), the dirty clear, the wind rule, the sun-basis staleness schedule, and the two pixel kernels. Every level uses the current sun basis.
- Tests: VsmTests' dirty-rule sections become a check that "the pages of frame N equal a fresh render" (after moving casters, wind and the sun, every page is new). The exactness sections stay.
- Quality: unchanged or better. Without a cache there are no stale pages and no per-level basis error (0.5/255).
