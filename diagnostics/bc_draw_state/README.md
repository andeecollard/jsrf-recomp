# BC draw-state regression

Builds the current Metal renderer in isolation. No game assets, generated code,
player bundle, save changes, or game window. Requires a working Metal device;
device initialization failure is a failure, never a silent pass.

From the repository root:

```sh
cmake -S diagnostics/bc_draw_state -B /tmp/jsrf-bc-draw-state -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/jsrf-bc-draw-state -j 4
ctest --test-dir /tmp/jsrf-bc-draw-state --output-on-failure
```

Run with a clean `RECOMP_*` environment. The test matrix sets batching and
hardware fragment state independently to 0/1 and disables GPU vertex programs.
It does not exercise the player's GPU fixed-function vertex transform.
Sandboxed processes may not see Metal; run the tests with GPU access.

Each process checks four phases against independent, exact RGB565 pixel values:

1. Alternate two 256x256 DXT3 pages across 256 draws.
2. Change the contents at the same texture address between every draw.
3. Submit 256 distinct textures, exceeding the current 128-entry cache.
4. Overdraw all 256 tiles with different textures; the later draw must win.

Each phase submits all draws before explicitly synchronizing. Caller-owned
state and texture bytes are overwritten before that sync. Every phase checks
all 16,384 output pixels, including tile edges. These are synthetic opaque
pages: this does not test glyph UV generation, transparency, multiple texture
units, render-to-texture debt, guest pushbuffer decoding, or concurrent writers.

## Measured on 20 September 2026

Renderer source at `b408c18`, Apple M1 Max: **4/4 CTest cases pass** (16 phase
checks, 262,144 exact pixel comparisons). Hardware/batched counters confirm
1,280 hardware draws, no mixed-path draws, 256 texture cache hits and 1,024
uploads. Only the four final phase readbacks drained the GPU in that case.

Two deliberately broken renderer copies were built under `/tmp`, without
editing production source. Each failed all four configurations:

- Remove the content comparison on a same-address cache hit: alternating pages
  still pass, but same-address replacement fails on **12,288 pixels**.
- Match cache entries by size alone and accept their buffer without a content
  comparison: alternating pages fail on **8,192 pixels**.

The first mutation replaces
`if(entry->buffer&&!memcmp(entry->buffer.contents,data,size))`
with `if(entry->buffer)` inside `texture_buffer`. The second also replaces
`if(entry->source==data&&entry->size==size)` with `if(entry->size==size)`.
Apply only to scratch copies. These controls establish sensitivity to stale
content and page aliasing; they do not establish sensitivity to every GPU race.

This is a regression boundary for future cache/batching changes, **not a
reproduction or fix of JSRF's corrupt text**. See the dated Codex handover.
