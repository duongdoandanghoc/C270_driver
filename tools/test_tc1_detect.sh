#!/bin/bash
# TC1: Detect USB Camera
# Expect: C270 xuất hiện trong lsusb output
echo "═══ TC1: Detect USB Camera ═══"
echo ""

VID="046d"
PID="0825"

# Test 1: lsusb detect
echo -n "Test 1 — lsusb detect C270... "
if lsusb -d "${VID}:${PID}" > /dev/null 2>&1; then
    INFO=$(lsusb -d "${VID}:${PID}" | head -1)
    echo "PASS ✓"
    echo "  → $INFO"
else
    echo "FAIL ✗ (camera không cắm?)"
    exit 1
fi

# Test 2: USB descriptor readable
echo -n "Test 2 — USB descriptor readable... "
DESC=$(lsusb -v -d "${VID}:${PID}" 2>/dev/null | grep -c "FORMAT_" || echo 0)
if [ "$DESC" -ge 2 ]; then
    echo "PASS ✓ ($DESC formats found)"
else
    echo "FAIL ✗ (descriptor not readable, need sudo?)"
fi

# Test 3: MJPEG format present
echo -n "Test 3 — FORMAT_MJPEG in descriptor... "
if lsusb -v -d "${VID}:${PID}" 2>/dev/null | grep -q "FORMAT_MJPEG"; then
    echo "PASS ✓"
else
    echo "FAIL ✗"
fi

# Test 4: unbind script works
echo -n "Test 4 — unbind_c270.sh runs... "
if sudo bash unbind_c270.sh --quiet 2>/dev/null; then
    echo "PASS ✓"
else
    echo "FAIL ✗"
fi

# Test 5: libusb can open after unbind
echo -n "Test 5 — libusb open device... "
if sudo ./build/test_step1_usb 2>&1 | grep -q "C270 opened"; then
    echo "PASS ✓"
else
    echo "FAIL ✗"
fi

echo ""
echo "═══ TC1 Complete ═══"
