# Zōtorōpu — Tanmatsu Video Player

A video player for the Tanmatsu ESP32-P4 badge. Plays AVI files containing
either H.264 (Constrained Baseline) or MJPEG video with MP3 audio, from the
SD card.

## Features

- H.264 Constrained Baseline decoder (software, via h264bsd)
- MJPEG decoder (hardware JPEG decoder on ESP32-P4)
- MP3 audio playback, synchronized to video
- Direct output through the ESP32-P4 PPA (Pixel Processing Accelerator) for
  hardware YUV → RGB conversion and scaling
- Optional SIMD (PIE) optimizations for the H.264 decoder hot paths on
  ESP32-P4, behind `CONFIG_USE_SIMD_OPTIMIZATIONS`

## Hardware targets

- `tanmatsu` (ESP32-P4)
- `konsool` (ESP32-P4)

## A word about performance

The Tanmatsu is an embedded device with very limited CPU and memory compared
to a desktop PC or even a smartphone. Decoding a full 800×480 video stream
in software is not realistic — even with SIMD optimizations, H.264 decode
at that resolution exceeds the real-time budget at 15 fps.

**Recommendation:** use **half-resolution H.264** (up to 400×240) at
**15–20 fps**. The PPA hardware will scale the decoded frames up to fill
the display, so the perceived quality remains good while the decoder stays
well within its real-time budget.

MJPEG can handle larger resolutions because it uses the hardware JPEG
decoder, but MJPEG files are much larger than equivalent H.264 files —
which matters both for SD card space and for read bandwidth.

## Building

The project uses the standard Tanmatsu multi-device Makefile conventions:

```bash
make build DEVICE=tanmatsu     # or DEVICE=konsool
make flash DEVICE=tanmatsu PORT=/dev/ttyACM0
make monitor
```

To enable SIMD optimizations (ESP32-P4 only):

```bash
make menuconfig DEVICE=tanmatsu
# Video Player → Enable SIMD optimizations → yes
rm -rf build sdkconfig_tanmatsu   # config changes require a clean rebuild
make build DEVICE=tanmatsu
```

## Installing to the app repository

```bash
make apprepo                    # copies metadata + icons + application.bin
                                # to ../tanmatsu-app-repository/at.cavac.zoetrope/
```

## Playing videos

Place converted `.avi` files on the SD card under `/sd/movies/` and select
one from the app menu. Navigation keys control playback.

---

## Converting videos

Two conversion scripts are provided in `scripts/`. Both take an input
directory of videos (any format FFmpeg can read: `.mp4`, `.mkv`, `.mov`,
`.avi`, `.webm`) and produce Tanmatsu-compatible `.avi` files with MP3
audio. **FFmpeg must be installed.**

Both scripts accept `--force` to re-convert files that already exist in the
output directory.

### H.264 — `scripts/convert_h264.sh` (recommended)

Produces H.264 Constrained Baseline video + MP3 in an AVI container.
Output: 15 fps, CRF 28, single slice, one reference frame (no B-frames,
no CABAC — required by the software decoder).

```bash
cd scripts/
./convert_h264.sh orig              # full resolution (max 800×480) → converted_h264/
./convert_h264.sh orig --half       # half resolution (max 400×240) → converted_h264_half/
./convert_h264.sh orig --half --force
```

**Use `--half` for smooth playback on Tanmatsu.** The half-resolution
output (max 400×240, 15 fps) is what the H.264 software decoder can
realistically keep up with in real time. The PPA hardware then upscales
the frames to the display size — the quality loss from the downscale is
largely hidden by the upscale, but the decoder stays on budget.

Input directory defaults to `orig/`. All output dimensions are rounded
down to multiples of 16 for macroblock alignment.

### MJPEG — `scripts/convert_mjpeg.sh`

Produces MJPEG video + MP3 in an AVI container. Output: 20 fps, quality 5,
max 800×480 (full resolution is fine because decoding runs in the JPEG
hardware).

```bash
cd scripts/
./convert_mjpeg.sh orig             # → converted/
./convert_mjpeg.sh orig --force
```

MJPEG files are significantly larger than H.264 at similar quality —
expect roughly 5–10× the file size. Use MJPEG if file size is not a
concern and you want full-resolution playback, or if you're hitting H.264
decode limits.

## License

The contents of this repository may be considered in the public domain or
[CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0) licensed at
your disposal.
