#!/bin/bash
#
# One-time setup: extract H.264 streams from test AVIs, generate reference output.
# Run from project root: ./test/setup.sh
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST_DIR="$SCRIPT_DIR"
TESTDATA_DIR="/home/cavac/src/tanmatsu/h264tests"

echo "=== H264BSD Test Setup ==="

# Extract H.264 Annex B streams from AVI containers
echo "Extracting H.264 streams..."
if [ ! -f "$TEST_DIR/sintel.h264" ]; then
    ffmpeg -y -i "$TESTDATA_DIR/Sintel.2010.720p.avi" \
        -c:v copy -bsf:v h264_mp4toannexb "$TEST_DIR/sintel.h264" 2>/dev/null
    echo "  sintel.h264 extracted"
else
    echo "  sintel.h264 already exists, skipping"
fi

if [ ! -f "$TEST_DIR/tears.h264" ]; then
    ffmpeg -y -i "$TESTDATA_DIR/Tears-of-Steel-4k-1920.avi" \
        -c:v copy -bsf:v h264_mp4toannexb "$TEST_DIR/tears.h264" 2>/dev/null
    echo "  tears.h264 extracted"
else
    echo "  tears.h264 already exists, skipping"
fi

# Build decoder
echo "Building test decoder..."
make -C "$TEST_DIR" clean >/dev/null 2>&1 || true
make -C "$TEST_DIR" 2>&1 | tail -1

# Generate reference output
echo "Generating reference output (this may take a moment)..."
"$TEST_DIR/test_decode" "$TEST_DIR/sintel.h264" "$TEST_DIR/sintel_ref.i420"
"$TEST_DIR/test_decode" "$TEST_DIR/tears.h264" "$TEST_DIR/tears_ref.i420"

echo ""
echo "Reference files:"
ls -lh "$TEST_DIR"/sintel_ref.i420 "$TEST_DIR"/tears_ref.i420
echo ""
echo "=== Setup complete. Run ./test/run_tests.sh to test after code changes. ==="
