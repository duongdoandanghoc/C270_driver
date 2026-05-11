#!/bin/bash
# TC2: Start Stream — verify video output, no lag
# Requires: camera plugged in, driver built
echo "═══ TC2: Start Stream ═══"
echo ""

TIMEOUT=8  # seconds to run

# Ensure unbind
echo "Preparing: unbind uvcvideo..."
sudo bash unbind_c270.sh --quiet 2>/dev/null

# Start app in background
echo "Starting c270_app (${TIMEOUT}s test)..."
sudo timeout ${TIMEOUT} ./build/c270_app --no-display --no-hotplug 2>&1 | tee /tmp/tc2_output.txt &
APP_PID=$!

sleep 3

# Check output
echo ""
echo "── Verifying ──"

echo -n "Test 1 — USB opened... "
if grep -q "C270 opened" /tmp/tc2_output.txt; then
    echo "PASS ✓"
else echo "FAIL ✗"; fi

echo -n "Test 2 — UVC probe accepted... "
if grep -q "Probe accepted" /tmp/tc2_output.txt; then
    echo "PASS ✓"
else echo "FAIL ✗"; fi

echo -n "Test 3 — FormatIndex = 2 (MJPEG)... "
if grep -q "FormatIndex.*= 2" /tmp/tc2_output.txt; then
    echo "PASS ✓"
else echo "FAIL ✗"; fi

echo -n "Test 4 — Stream started 640x480... "
if grep -q "Stream started.*640x480" /tmp/tc2_output.txt; then
    echo "PASS ✓"
else echo "FAIL ✗"; fi

echo -n "Test 5 — ISO transfers submitted... "
if grep -q "iso transfers submitted" /tmp/tc2_output.txt; then
    echo "PASS ✓"
else echo "FAIL ✗"; fi

echo -n "Test 6 — RTSP server started... "
if grep -q "RTSP server started" /tmp/tc2_output.txt; then
    echo "PASS ✓"
else echo "FAIL ✗"; fi

echo -n "Test 7 — No crash errors... "
if ! grep -qi "segfault\|abort\|core dump" /tmp/tc2_output.txt; then
    echo "PASS ✓"
else echo "FAIL ✗"; fi

# Wait for app to finish
wait $APP_PID 2>/dev/null

echo ""
echo "═══ TC2 Complete ═══"
