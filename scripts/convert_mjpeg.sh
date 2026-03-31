#!/bin/bash
# Convert videos to Tanmatsu-optimized AVI format (MJPEG + MP3)
# Output: <= 800x480, 20fps, MJPEG q5, MP3 128k stereo 44.1kHz
# Dimensions are rounded down to multiples of 16 for JPEG hardware decoder

INPUT_DIR="${1:-orig}"
OUTPUT_DIR="converted"
FORCE=0

# Parse flags
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
    esac
done

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
echo "Format: MJPEG+MP3 AVI, 20fps, max 800x480, 16px aligned"
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

    # The scale filter:
    # 1. Scale down to fit within 800x480 while preserving aspect ratio
    # 2. Round dimensions down to nearest multiple of 16 (JPEG HW decoder requirement)
    ffmpeg -i "$f" \
        -c:v mjpeg -q:v 5 \
        -vf "scale=w='trunc(min(800\,iw*min(800/iw\,480/ih))/16)*16':h='trunc(min(480\,ih*min(800/iw\,480/ih))/16)*16'" \
        -r 20 \
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
