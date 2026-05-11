#!/bin/bash
# TC3: Multi-Client — 3 clients cùng truy cập RTSP
# Requires: c270_app đang chạy
echo "═══ TC3: Multi-Client Access ═══"
echo ""

RTSP_URL="rtsp://127.0.0.1:8554/camera0"
DURATION=5  # seconds mỗi client chạy

# Check RTSP server reachable
echo -n "Test 0 — RTSP server reachable... "
if timeout 2 bash -c "echo > /dev/tcp/127.0.0.1/8554" 2>/dev/null; then
    echo "PASS ✓"
else
    echo "FAIL ✗ (c270_app đang chạy chưa?)"
    echo "  → Chạy: sudo ./build/c270_app"
    exit 1
fi

# Start 3 ffmpeg clients simultaneously
echo ""
echo "Starting 3 concurrent ffplay clients (${DURATION}s each)..."

for i in 1 2 3; do
    timeout ${DURATION} ffplay -loglevel error -an -flags low_delay \
        -fflags nobuffer "${RTSP_URL}" </dev/null >/tmp/tc3_client${i}.txt 2>&1 &
    PIDS[${i}]=$!
    echo "  Client $i: PID ${PIDS[$i]}"
done

sleep $((DURATION + 1))

# Check results
echo ""
echo "── Verifying ──"

PASS=0
for i in 1 2 3; do
    echo -n "Test $i — Client $i connected... "
    # If ffplay errored, the output file will have error messages
    if [ -f /tmp/tc3_client${i}.txt ] && ! grep -qi "error\|refused\|failed" /tmp/tc3_client${i}.txt; then
        echo "PASS ✓"
        PASS=$((PASS + 1))
    else
        echo "FAIL ✗"
        [ -s /tmp/tc3_client${i}.txt ] && echo "    Error: $(cat /tmp/tc3_client${i}.txt)"
    fi
    wait ${PIDS[$i]} 2>/dev/null
done

echo ""
echo -n "Multi-client result: "
if [ "$PASS" -eq 3 ]; then
    echo "$PASS/3 PASS ✓"
else
    echo "$PASS/3 (some clients failed)"
fi

echo ""
echo "═══ TC3 Complete ═══"
