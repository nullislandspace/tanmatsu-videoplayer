# H264BSD Decoder Rewrite for ESP32-P4

## Overview

The h264bsd software H.264 decoder component has been restructured to optimize
for the ESP32-P4 platform. Changes fall into three categories:

1. **Dead code removal** — ARM/OMXDL/NEON code paths stripped
2. **Residual memory layout** — i32 block-order changed to i16 row-order
3. **PPA-packed output** — decoder produces PPA-ready YUV420 directly

All changes are validated against a PC test harness that decodes 24,336 frames
across two test videos (Sintel 400x160, Tears of Steel 400x160) with
byte-identical I420 output and verified PPA output.

---

## 1. Dead Code Removal

Removed all `#ifdef H264DEC_OMXDL`, `#ifdef H264DEC_ARM11`, and
`#ifdef H264DEC_NEON` code paths across 15 source files (~74 occurrences).
These were ARM-specific optimizations (OMX DL acceleration, ARM11 assembly,
NEON SIMD) that do not apply to the ESP32-P4's RISC-V architecture.

Only the C reference implementations are retained. The ESP32-P4's PIE SIMD
instructions use a completely different ISA and will be added via
`CONFIG_USE_SIMD_OPTIMIZATIONS` guards.

**Files affected:** All `.c` and `.h` files in `components/h264bsd/src/`.

---

## 2. Residual Memory Layout

### Problem

The original decoder stored transform residuals as `i32 level[26][16]` in
4x4 block order. This prevented SIMD optimization of `h264bsdWriteOutputBlocks`
because assembling 16 contiguous pixels for one output row required gathering
values from 4 non-contiguous blocks. SIMD testing showed this made the function
0.79x (slower) with SIMD vs scalar.

The `i32` type also wasted bandwidth — residuals are guaranteed to be in the
range [-512, 511] after the inverse transform, which fits in `i16`.

### Solution

The `residual_t` structure now has three additional fields:

```c
typedef struct residual_tag {
    i16 totalCoeff[27];
    i32 level[26][16];          /* CAVLC + transform workspace (i32) */
    u32 coeffMap[24];
    i16 levelI16[24][16];       /* narrowed i16 residuals, block-order */
    i16 lumaRows[16][16];       /* row-ordered luma, 16-byte aligned */
    i16 chromaRows[2][8][8];    /* row-ordered chroma, 16-byte aligned */
} residual_t;
```

The data flow is:

1. **CAVLC decode** writes quantized coefficients to `level[26][16]` (i32)
2. **ProcessResidual** runs inverse quantization + IDCT on `level` in-place,
   then narrows each block's 16 values from i32 to i16 into `levelI16[24][16]`
3. **For inter prediction path:** `h264bsdReorderResidualToRows` scatters the
   block-order `levelI16` into row-order `lumaRows[16][16]` and
   `chromaRows[2][8][8]` using `h264bsdBlockX/Y` lookup tables. Empty blocks
   produce zeros.
4. **h264bsdWriteOutputBlocks** reads contiguous i16 rows and adds them to u8
   predictions with clipping: `imgRow[col] = clp[predRow[col] + resRow[col]]`
5. **For intra prediction path:** `h264bsdAddResidual` reads `levelI16` directly
   in block-order (per-block access, no reorder needed).

### Memory alignment

- `lumaRows[16][16]`: 16-byte aligned — one row of 16 x i16 = 32 bytes fills
  two 128-bit PIE registers (8 elements each)
- `chromaRows[2][8][8]`: 16-byte aligned — one row of 8 x i16 = 16 bytes fills
  one 128-bit register exactly
- `levelI16[24][16]`: naturally aligned following i32 arrays

### Sentinel values

- i32 path: `EMPTY_RESIDUAL_INDICATOR` = `0xFFFFFF` (unchanged)
- i16 path: `EMPTY_RESIDUAL_INDICATOR_I16` = `0x7FFF` (INT16_MAX, outside
  valid [-512, 511] range)

### Files modified

| File | Change |
|------|--------|
| `h264bsd_util.h` | Added i16 sentinel macros |
| `h264bsd_macroblock_layer.h` | New fields in `residual_t`, struct tag added |
| `h264bsd_macroblock_layer.c` | `ProcessResidual` narrows i32 to i16 per block |
| `h264bsd_image.c` | New `h264bsdReorderResidualToRows`, rewritten `h264bsdWriteOutputBlocks` |
| `h264bsd_image.h` | Updated signatures, forward declaration for `residual_t` |
| `h264bsd_intra_prediction.c` | `h264bsdAddResidual` uses i16, all callers updated |
| `h264bsd_intra_prediction.h` | Updated function signatures to i16 |
| `h264bsd_inter_prediction.c` | Calls reorder + new WriteOutputBlocks signature |

---

## 3. PPA-Packed Output

### Problem

After decoding, the videoplayer ran a separate CPU pass (`i420_to_ppa_yuv420`)
to reformat I420 planar data into PPA-packed YUV420, consuming 11.3ms per
frame at 800x336 (12% of the 66ms frame budget at 15fps).

### Solution

The decoder now produces PPA-packed YUV420 as a side output alongside the
I420 reference frames. A PPA buffer is allocated in the DPB for each frame.
After the deblocking filter runs (which modifies the I420 image in-place),
`h264bsdConvertImageToPpa` converts the full frame from I420 to PPA format.

The videoplayer's `main.c` checks for the PPA buffer and feeds it directly
to the PPA hardware via `ppa_to_framebuffer`, skipping the reformat step.

### PPA format (ESP32-P4 TRM Table 37.5-1)

Per 8 luma pixels, 12 bytes:
```
Even rows (0-indexed): [U0][Y0][Y1][U2] [Y2][Y3][U4][Y4] [Y5][U6][Y6][Y7]
Odd rows (0-indexed):  [V0][Y0][Y1][V2] [Y2][Y3][V4][Y4] [Y5][V6][Y6][Y7]
```
Stride: `width * 3 / 2` bytes per row (1.5 bytes per pixel).

### Why full-frame, not per-macroblock

The deblocking filter modifies pixel values at block boundaries after all
macroblocks are decoded. A per-macroblock PPA write would capture
pre-deblocking values, producing visible artifacts. The full-frame conversion
runs after deblocking and produces correct output.

### DPB changes

`dpbPicture_t` gains `ppa_data` and `pAllocatedPpaData` pointers. The PPA
buffer is allocated alongside the I420 buffer in `h264bsdInitDpb`, sized
identically (1.5 bytes/pixel = same total as I420). All DPB buffer swaps
and output buffer assignments propagate both pointers.

`dpbOutPicture_t` gains a `ppa_data` field so `h264bsdNextOutputPicturePpa`
can return the PPA buffer for the most recently output picture.

`image_t` gains `ppa_data` and `ppa_stride` fields, set when a DPB image
is allocated for decoding.

### API additions

- `h264bsdConvertImageToPpa(image_t *image)` — full-frame I420 to PPA conversion
- `h264bsdNextOutputPicturePpa(storage_t *pStorage, u32 *ppaStride)` — get PPA
  buffer for last output picture
- `video_decoder_decode` — new `ppa_out` and `ppa_stride` output parameters
- `ppa_to_framebuffer` — feeds PPA buffer directly to PPA hardware

### Files modified

| File | Change |
|------|--------|
| `h264bsd_dpb.h` | PPA fields in `dpbPicture_t`, `dpbOutPicture_t`, `dpbStorage_t` |
| `h264bsd_dpb.c` | PPA allocation, free, pointer propagation, `picWidthInMbs` parameter |
| `h264bsd_image.h` | `h264bsdConvertImageToPpa` declaration, `ppa_data`/`ppa_stride` in `image_t` |
| `h264bsd_image.c` | `h264bsdConvertImageToPpa` implementation |
| `h264bsd_decoder.c` | Calls `h264bsdConvertImageToPpa` after deblocking, `h264bsdNextOutputPicturePpa` |
| `h264bsd_decoder.h` | `h264bsdNextOutputPicturePpa` declaration |
| `h264bsd_storage.c` | Passes `picWidthInMbs` to `h264bsdResetDpb` |
| `video_decoder.c/.h` | PPA output parameters |
| `yuv_convert.c/.h` | `ppa_to_framebuffer` function |
| `main.c` | Uses PPA path for H.264 when available |

---

## 4. Test Infrastructure

### PC test harness (`test/`)

- `test_decode.c` — standalone decoder that reads H.264 Annex B bitstreams,
  writes I420 output, verifies PPA output against reference conversion
- `Makefile` — builds with `gcc -O2` against h264bsd sources directly
- `setup.sh` — extracts test streams from AVI files, generates reference output
- `run_tests.sh` — regression test: rebuild, decode, diff against reference

### Test data

- Sintel 400x160, 13322 frames, Constrained Baseline, 15fps
- Tears of Steel 400x160, 11014 frames, Constrained Baseline, 15fps

Both streams exercise I-frames, P-frames, P_Skip, intra16x16, intra4x4,
inter prediction with fractional motion vectors, and scene transitions.

---

## 5. Performance Profiling

### Profiling infrastructure

`h264bsd_profile.h` provides a shared timing framework used across the decoder.
Timers use `esp_timer_get_time()` on ESP-IDF and `clock_gettime(CLOCK_MONOTONIC)`
on PC. Accumulated per 30-frame window and logged by `main.c`.

Instrumented functions:

| Timer | Location | Scope |
|-------|----------|-------|
| `INTER_PREDICTION` | h264bsd_macroblock_layer.c | per-macroblock |
| `INTRA_PREDICTION` | h264bsd_macroblock_layer.c | per-macroblock |
| `PROCESS_RESIDUAL` | h264bsd_macroblock_layer.c | per-macroblock |
| `FILTER_PICTURE` | h264bsd_deblocking.c | per-frame |
| `WRITE_OUTPUT_BLOCKS` | h264bsd_image.c | per inter-macroblock |
| `CONVERT_PPA` | h264bsd_image.c | per-frame |
| `LUMA_INTERP` | h264bsd_reconstruct.c | per h264bsdPredictSamples call |
| `CHROMA_INTERP` | h264bsd_reconstruct.c | per h264bsdPredictSamples call |

Luma interpolation case hit counts (cases 0-15 per `lumaFracPos` table)
are also tracked in `h264bsd_luma_case_counts[16]`.

### Measured ESP32-P4 performance (800x336 @ 15fps, Sintel)

Frame budget at 15fps: **66.7ms**

#### Full frame pipeline

| Step | Time | Notes |
|------|------|-------|
| Decode (h264bsd) | ~100ms | All decoder work including PPA conversion |
| Copy to display | ~13ms | PPA hardware SRM (scale/rotate/mirror) |
| **Total** | **~113ms** | **1.7x over budget** |

#### Decode breakdown (steady state, 30-frame average)

| Component | ms/frame | % of Decode | Scope |
|-----------|----------|-------------|-------|
| **Inter prediction** | **43** | **43%** | per-macroblock |
| — Luma interpolation | 20 | 20% | ~1100 calls/frame |
| — Chroma interpolation | 7 | 7% | ~1100 calls/frame |
| — WriteOutputBlocks | 9 | 9% | per inter-MB |
| — MV prediction + other | 7 | 7% | |
| **Deblocking filter** | **24** | **24%** | per-frame |
| **PPA convert** | **11** | **11%** | per-frame |
| Intra prediction | 4 | 4% | per intra-MB |
| Residual (IQ + IDCT) | 3 | 3% | per-MB |
| Other (CAVLC/NAL/DPB) | 5 | 5% | |

#### Luma interpolation case distribution

Cases correspond to the H.264 quarter-pixel interpolation grid (6-tap FIR
filter combinations). `lumaFracPos[xFrac][yFrac]` maps fractional MV
components to cases 0-15.

| Cases | Function | Calls/30fr | % | Description |
|-------|----------|-----------|---|-------------|
| 0 | FillBlock (memcpy) | 10554 | 32% | Full-pixel, no filtering |
| 1, 3 | InterpolateVerQuarter | 4037 | 12% | Vertical quarter-pixel |
| 2 | InterpolateVerHalf | 2189 | 7% | Vertical half-pixel |
| 4, 12 | InterpolateHorQuarter | 3774 | 11% | Horizontal quarter-pixel |
| 8 | InterpolateHorHalf | 2197 | 7% | Horizontal half-pixel |
| 5, 7, 13, 15 | InterpolateHorVerQuarter | 5721 | 17% | Diagonal quarter-pixel |
| 10 | InterpolateMidHalf | 1505 | 5% | Center half-pixel |
| 6, 14 | InterpolateMidHorQuarter | 1723 | 5% | Center + horizontal quarter |
| 9, 11 | InterpolateMidVerQuarter | 1703 | 5% | Center + vertical quarter |

Distribution is fairly uniform across all fractional positions. No single
case dominates beyond case 0 (fullpel copy, already a memcpy). The 68% of
calls requiring interpolation are spread across 9 function families.

#### 400x160 performance (for comparison)

At 400x160, decode averages ~25ms/frame (well within 66ms budget):
- Inter: 10ms, Deblock: 6ms, PPA: 2ms, WrOut: 3ms, Other: 4ms

---

## 6. SIMD Optimization Plan

Target: reduce 800x336 decode from ~100ms to ≤50ms (leaving ~13ms for PPA
hardware and headroom for complex scenes).

Functions are listed in recommended implementation order, prioritized by
**ms saved per implementation effort**.

### Tier 1: High impact, single-function targets (save ~25ms)

#### 6.1 h264bsdFilterPicture — Deblocking filter (h264bsd_deblocking.c)

**Measured: 24ms/frame (24% of decode). Priority: #1.**

The deblocking filter runs once per frame after all macroblocks are decoded.
It applies adaptive edge filtering at 4x4 block boundaries to reduce
blocking artifacts. The core work is in `FilterVerLumaEdge`,
`FilterHorLumaEdge`, `FilterVerChromaEdge`, `FilterHorChromaEdge`.

```
Operation per edge: load 4 pixels on each side of boundary (p0-p3, q0-q3),
compute boundary strength thresholds, conditionally apply 4-tap or 5-tap
filter with clipping, write back modified pixels.
```

**SIMD approach:**
- Each edge processes 4 pixels vertically (luma) or 2 pixels (chroma)
- Multiple edges per macroblock can be batched
- Compare/clamp/delta operations map well to `esp.vadd.s16`,
  `esp.vsub.s16`, `esp.vmin.s16`, `esp.vmax.s16`
- Vertical edges require transpose (pixels are in adjacent rows, not
  contiguous), limiting SIMD efficiency for vertical filtering
- Horizontal edges are contiguous in memory, good SIMD fit

**Expected speedup:** 2x (12ms saved). Vertical edge transpose overhead
limits gains vs. pure horizontal operations.

#### 6.2 h264bsdConvertImageToPpa — I420 to PPA conversion (h264bsd_image.c)

**Measured: 11ms/frame (11% of decode). Priority: #2.**

Pure byte shuffling, no arithmetic. Runs once per frame.

```
Input:  Y plane — u8, stride = picWidth, 16-byte aligned
        U plane — u8, stride = picWidth/2, 16-byte aligned
        V plane — u8, stride = picWidth/2, 16-byte aligned
Output: PPA buffer — u8, stride = picWidth * 3/2, 64-byte aligned
```

**Operation per 8 pixels:** interleave 8 Y bytes + 4 chroma bytes into 12-byte
PPA pattern: `[C0][Y0][Y1][C2] [Y2][Y3][C4][Y4] [Y5][C6][Y6][Y7]`

**SIMD approach:**
- Process 16 pixels per iteration (24 bytes output)
- Load 16 Y bytes with `esp.vld.128.ip`
- Load 8 chroma bytes with `esp.ld.128.usar.ip`
- Interleave with `esp.vzip.8` / `esp.vunzip.8` patterns
- Store 24 output bytes (16 + 8, or creative packing)

**Expected speedup:** 3-4x (8ms saved). Bulk byte operations, high throughput.

#### 6.3 h264bsdWriteOutputBlocks — Residual addition (h264bsd_image.c)

**Measured: 9ms/frame (9% of decode). Priority: #3.**

Called for every inter-predicted macroblock (non-P_Skip). Adds i16 residuals
to u8 predictions with clipping.

```
Input:  data[256+128] — u8 prediction, stride 16 (luma) / 8 (chroma)
        lumaRows[16][16] — i16 residuals, 16-byte aligned, contiguous rows
        chromaRows[2][8][8] — i16 residuals, 16-byte aligned, contiguous rows
Output: image luma/cb/cr — u8, stride = picWidth
```

**Operation per pixel:** `output = clip(prediction_u8 + residual_i16, 0, 255)`

**SIMD approach:**
- Load 8 u8 predictions → widen to i16 via `esp.vzip.8` with zero register
- Load 8 i16 residuals with `esp.vld.128.ip` (16-byte aligned)
- `esp.vadd.s16` prediction + residual
- Saturate and narrow i16 → u8
- 2 iterations per luma row (16 pixels), 16 rows + 2×8 chroma rows

**Expected speedup:** 2-3x (5ms saved). Data is already row-contiguous and
aligned for SIMD access.

### Tier 2: Medium impact, multiple functions (save ~15ms)

#### 6.4 Luma interpolation functions (h264bsd_reconstruct.c)

**Measured: 20ms/frame total (20% of decode). Spread across 9 function
families, ~1100 calls/frame.**

Each function implements a variant of the H.264 6-tap FIR filter for
sub-pixel motion compensation. All share a common pattern:

```
Input:  ref — u8 reference frame, stride = picWidth (unaligned)
Output: mb — u8 prediction block, stride 16
```

**Functions by call frequency (high to low):**

1. `h264bsdFillBlock` (case 0, 32%) — memcpy, already fast
2. `h264bsdInterpolateHorVerQuarter` (cases 5,7,13,15, 17%) — 6-tap H + 6-tap V + average
3. `h264bsdInterpolateVerQuarter` (cases 1,3, 12%) — 6-tap V + average
4. `h264bsdInterpolateHorQuarter` (cases 4,12, 11%) — 6-tap H + average
5. `h264bsdInterpolateVerHalf` (case 2, 7%) — 6-tap V [SIMD already implemented]
6. `h264bsdInterpolateHorHalf` (case 8, 7%) — 6-tap H
7. `h264bsdInterpolateMidHalf` (case 10, 5%) — 6-tap H + 6-tap V
8. `h264bsdInterpolateMidHorQuarter` (cases 6,14, 5%) — 6-tap H + 6-tap V + avg H
9. `h264bsdInterpolateMidVerQuarter` (cases 9,11, 5%) — 6-tap H + 6-tap V + avg V

**SIMD approach (shared across all):**
- Unaligned reference loads with `esp.ld.128.usar.ip`
- u8→i16 widening with `esp.vzip.8` and zero register
- 6-tap FIR: `(a - 5b + 20c + 20d - 5e + f + 16) >> 5` using
  `esp.vadd.s16`, `esp.vsub.s16`, `esp.vmul.s16` (or shift+add)
- Clamp to [0, 255] and narrow i16→u8

**Implementation strategy:** Start with `h264bsdInterpolateHorHalf` (case 8,
pure horizontal 6-tap, simplest SIMD pattern), then extend to
`h264bsdInterpolateHorQuarter` (adds averaging step). The vertical variants
require column-wise access patterns that are less SIMD-friendly.

**Note:** `h264bsdInterpolateVerHalf` (case 2) had a previous SIMD
implementation that was removed after the h264 internals rewrite. It will
need to be re-implemented against the new code structure.

**Expected speedup:** 1.5-2x across all functions (10ms saved). Limited by
unaligned reference loads and variable block sizes (4x4 through 16x16).

#### 6.5 Chroma interpolation (h264bsd_reconstruct.c)

**Measured: 7ms/frame (7% of decode). ~1100 calls/frame.**

Bilinear interpolation on 8×8 or smaller chroma blocks. Three functions:
`h264bsdInterpolateChromaHor`, `h264bsdInterpolateChromaVer`,
`h264bsdInterpolateChromaHorVer`.

**Expected speedup:** 1.5-2x (3-4ms saved). Small block sizes limit SIMD
utilization.

### Tier 3: Low impact (not recommended)

#### 6.6 h264bsdReorderResidualToRows (h264bsd_image.c)

Not measured separately. Small working set (384 i16 values per macroblock).
Scatter pattern doesn't map well to SIMD. Keep scalar.

#### 6.7 h264bsdAddResidual (h264bsd_intra_prediction.c)

Part of intra prediction (4ms/frame total). Operates on 4x4 blocks (4 u8
per row = 32 bits, poor register utilization). Keep scalar.

#### 6.8 ProcessResidual / IDCT (h264bsd_macroblock_layer.c)

3ms/frame. Inverse quantization + 4x4 IDCT. Small working set per block.
The i32→i16 narrowing added by the rewrite is part of this. Keep scalar.

### Existing SIMD implementations

| Function | Location | Speedup | Notes |
|----------|----------|---------|-------|
| `simd_memcpy` | simd_utils/simd_memcpy.h | 4.3-4.5x | 8-register unrolled, 128 bytes/iter |

Note: The previous SIMD version of `h264bsdInterpolateVerHalf` was removed
after the h264 internals rewrite. It will be re-implemented against the new
code structure.

### Projected impact summary

| Optimization | Current | Target | Saved |
|-------------|---------|--------|-------|
| Deblocking (2x) | 24ms | 12ms | 12ms |
| PPA convert (3x) | 11ms | 3ms | 8ms |
| WriteOutputBlocks (2.5x) | 9ms | 4ms | 5ms |
| Luma interpolation (1.5x) | 20ms | 13ms | 7ms |
| Chroma interpolation (1.5x) | 7ms | 4ms | 3ms |
| **Total saved** | | | **35ms** |
| **Projected decode** | **100ms** | **~65ms** | |

This would bring 800x336 decode to ~65ms, just under the 66ms frame budget.
Tier 1 alone (deblocking + PPA + WriteOutputBlocks) saves ~25ms for ~75ms
decode, which would eliminate most frame skipping.

---

## 7. Performance Notes

### PC benchmark (x86, gcc -O2, Sintel 13322 frames)

| Version | Time | Per-frame | Change |
|---------|------|-----------|--------|
| Original | 5.39s | 0.40ms | baseline |
| Modified | 6.95s | 0.52ms | +29% |

The overhead comes from i32→i16 narrowing, block→row reorder, and full-frame
PPA conversion. On ESP32-P4, the PPA conversion replaces the existing
`i420_to_ppa_yuv420` (11.3ms), so the net effect is a reduction in total
frame time.

### Measured ESP32-P4 frame timing

| Step | 400x160 | 800x336 |
|------|---------|---------|
| Decode | ~25ms | ~100ms |
| PPA hardware | ~5ms | ~13ms |
| **Total** | **~30ms** | **~113ms** |
| **Max FPS** | **~34fps** | **~9fps** |
| 15fps budget | OK (30 < 66ms) | Over (113 > 66ms) |
