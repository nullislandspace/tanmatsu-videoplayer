#!/bin/bash
# Convert videos to Tanmatsu H.264 test format (H.264 baseline + MP3 in AVI)
# Output: <= max_w x max_h, 15fps, H.264 baseline profile, MP3 128k stereo 44.1kHz
# Dimensions are rounded down to multiples of 16 for macroblock alignment
#
# Usage: convert_h264.sh [input_dir] [--force] [--half]
#   --half: output at half resolution (max 400x240) for hardware upscaling
#
# NOTE: H.264 in AVI is non-standard but FFmpeg handles it.
# The esp_h264 software decoder only supports baseline profile.

INPUT_DIR="${1:-orig}"
OUTPUT_DIR="converted_h264"
FORCE=0
HALF=0
FPS=15
CRF=28  # Lower = better quality, bigger file. 23=default, 28=smaller, 32=tiny
MAX_W=800
MAX_H=480

# Parse flags
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        --half) HALF=1 ;;
    esac
done

if [ "$HALF" -eq 1 ]; then
    MAX_W=400
    MAX_H=240
    OUTPUT_DIR="converted_h264_half"
fi

if [ ! -d "$INPUT_DIR" ]; then
    echo "Error: Input directory '$INPUT_DIR' not found"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

# Find all video files
VIDEOFILES=()
while IFS= read -r -d '' f; do
    VIDEOFILES+=("$f")
done < <(find "$INPUT_DIR" -maxdepth 1 -type f \( -iname '*.mkv' -o -iname '*.mov' -o -iname '*.mp4' -o -iname '*.avi' -o -iname '*.webm' \) -print0 | sort -z)

COUNT=${#VIDEOFILES[@]}

if [ "$COUNT" -eq 0 ]; then
    echo "No video files found in $INPUT_DIR"
    exit 0
fi

echo "Converting $COUNT video(s) from $INPUT_DIR/ to $OUTPUT_DIR/"
echo "Format: H.264 baseline + MP3 AVI, ${FPS}fps, max ${MAX_W}x${MAX_H}, CRF=$CRF"
echo ""

DONE=0
for f in "${VIDEOFILES[@]}"; do
    BASENAME=$(basename "$f")
    NAME="${BASENAME%.*}"
    OUTPUT="$OUTPUT_DIR/${NAME}.avi"

    DONE=$((DONE + 1))

    if [ -f "$OUTPUT" ] && [ "$FORCE" -eq 0 ]; then
        echo "[$DONE/$COUNT] SKIP: $BASENAME (already converted, use --force to reconvert)"
        continue
    fi

    echo "[$DONE/$COUNT] Converting: $BASENAME"

    # H.264 baseline profile (required by esp_h264 decoder)
    # - no B-frames, no CABAC, single reference frame
    # - CRF for quality control (lower = better)
    ffmpeg -i "$f" \
        -c:v libx264 -profile:v baseline -level 3.1 \
        -preset medium -crf $CRF -x264opts slices=1 \
        -vf "scale=w='trunc(min($MAX_W,iw*min($MAX_W/iw,$MAX_H/ih))/16)*16':h='trunc(min($MAX_H,ih*min($MAX_W/iw,$MAX_H/ih))/16)*16'" \
        -r $FPS \
        -g $((FPS * 2)) \
        -c:a libmp3lame -b:a 128k -ar 44100 -ac 2 \
        -y "$OUTPUT" \
        2>&1 | tail -1

    if [ -f "$OUTPUT" ]; then
        IN_SIZE=$(du -h "$f" | cut -f1)
        OUT_SIZE=$(du -h "$OUTPUT" | cut -f1)
        DIMS=$(ffprobe -v quiet -select_streams v:0 -show_entries stream=width,height -of csv=p=0 "$OUTPUT")
        echo "  Done: $IN_SIZE -> $OUT_SIZE ($DIMS)"
    else
        echo "  FAILED"
    fi
done

echo ""
echo "Conversion complete. Files in $OUTPUT_DIR/"
