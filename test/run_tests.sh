#!/bin/bash
#
# Regression test: rebuild decoder, decode test streams, diff against reference.
# Run from project root: ./test/run_tests.sh
# Exit code: 0 = PASS, non-zero = FAIL
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST_DIR="$SCRIPT_DIR"
PASS=0
FAIL=0

echo "=== H264BSD Regression Test ==="

# Check reference files exist
for ref in "$TEST_DIR/sintel_ref.i420" "$TEST_DIR/tears_ref.i420"; do
    if [ ! -f "$ref" ]; then
        echo "FAIL: Reference file missing: $ref"
        echo "Run ./test/setup.sh first."
        exit 1
    fi
done

# Check test streams exist
for stream in "$TEST_DIR/sintel.h264" "$TEST_DIR/tears.h264"; do
    if [ ! -f "$stream" ]; then
        echo "FAIL: Test stream missing: $stream"
        echo "Run ./test/setup.sh first."
        exit 1
    fi
done

# Rebuild decoder
echo "Building..."
if ! make -C "$TEST_DIR" 2>&1 | tail -3; then
    echo "FAIL: Build failed"
    exit 1
fi

# Test each stream
for name in sintel tears; do
    echo ""
    echo "--- Testing $name ---"
    "$TEST_DIR/test_decode" "$TEST_DIR/${name}.h264" "$TEST_DIR/${name}_test.i420"

    if cmp -s "$TEST_DIR/${name}_ref.i420" "$TEST_DIR/${name}_test.i420"; then
        echo "PASS: $name output matches reference"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $name output differs from reference!"
        # Find first difference
        cmp "$TEST_DIR/${name}_ref.i420" "$TEST_DIR/${name}_test.i420" 2>&1 || true
        # Show sizes
        ls -l "$TEST_DIR/${name}_ref.i420" "$TEST_DIR/${name}_test.i420"
        FAIL=$((FAIL + 1))
    fi

    # Clean up test output
    rm -f "$TEST_DIR/${name}_test.i420"
done

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="

if [ $FAIL -ne 0 ]; then
    exit 1
fi
exit 0
