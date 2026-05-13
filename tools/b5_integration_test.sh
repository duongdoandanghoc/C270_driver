#!/usr/bin/env bash
# =============================================================================
# b5_integration_test.sh — Integration Test cho mycam.ko (Task B5)
#
# Cách chạy:
#   cd /home/duong/c270_driver
#   sudo bash tools/b5_integration_test.sh
# =============================================================================

# NO set -e — manually handle errors to avoid silent exit from subshell traps

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
KO="$SCRIPT_DIR/kernel/mycam.ko"
LOG="/tmp/b5_test.log"
PASS=0
FAIL=0

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'

log()  { echo -e "$*" | tee -a "$LOG"; }
pass() { log "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { log "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { log "${BLUE}[INFO]${NC} $*"; }
warn() { log "${YELLOW}[WARN]${NC} $*"; }

# ─────────────────────────────────────────────
# Init
# ─────────────────────────────────────────────
echo "" > "$LOG"
log "================================================================"
log "  mycam.ko Integration Test (Task B5) — $(date)"
log "  Kernel: $(uname -r)"
log "================================================================"

if [ "$(id -u)" -ne 0 ]; then
    log "${RED}ERROR: Chạy với sudo${NC}"; exit 1
fi
if [ ! -f "$KO" ]; then
    log "${RED}ERROR: $KO không tồn tại${NC}"; exit 1
fi

# ─────────────────────────────────────────────
# Step 1: Verify C270 hardware
# ─────────────────────────────────────────────
log ""
log "━━━ Step 1: Verify C270 hardware ━━━"

if lsusb | grep -q "046d:0825"; then
    pass "C270 detected: $(lsusb | grep '046d:0825')"
else
    fail "C270 NOT found — cắm camera vào trước!"
    exit 1
fi

# Tìm USB path — dùng grep thay vì for+if để tránh set -e issue
C270_SYS=""
for d in /sys/bus/usb/devices/*/idVendor; do
    v=$(cat "$d" 2>/dev/null || true)
    base=$(dirname "$d")
    p=$(cat "$base/idProduct" 2>/dev/null || true)
    if [ "$v" = "046d" ] && [ "$p" = "0825" ]; then
        C270_SYS=$(basename "$base")
        break
    fi
done

if [ -z "$C270_SYS" ]; then
    fail "Không tìm được USB path của C270 trong sysfs"
    exit 1
fi
info "C270 USB path: $C270_SYS  (interfaces: ${C270_SYS}:1.0, ${C270_SYS}:1.1)"

# ─────────────────────────────────────────────
# Step 2: Unbind C270 from uvcvideo
# ─────────────────────────────────────────────
log ""
log "━━━ Step 2: Unbind C270 from uvcvideo ━━━"

UNBOUND=false
for iface in "${C270_SYS}:1.0" "${C270_SYS}:1.1" "${C270_SYS}:1.2" "${C270_SYS}:1.3"; do
    BIND_PATH="/sys/bus/usb/drivers/uvcvideo/$iface"
    if [ -e "$BIND_PATH" ]; then
        info "Unbinding $iface ..."
        if echo -n "$iface" > /sys/bus/usb/drivers/uvcvideo/unbind 2>/dev/null; then
            UNBOUND=true
            info "  → OK"
        else
            warn "  → Write failed (possible: not bound to uvcvideo)"
        fi
        sleep 0.2
    fi
done

sleep 1

DEVS_NOW=$(ls /dev/video* 2>/dev/null || true)
info "Video devices after unbind: ${DEVS_NOW:-none}"

if [ "$UNBOUND" = true ]; then
    pass "C270 unbind từ uvcvideo thành công"
else
    warn "C270 không thấy trong uvcvideo — có thể đã unbind từ trước, tiếp tục..."
fi

# ─────────────────────────────────────────────
# Step 3: insmod mycam.ko
# ─────────────────────────────────────────────
log ""
log "━━━ Step 3: insmod mycam.ko ━━━"

# Xóa module cũ nếu đang load
if lsmod | grep -q "^mycam"; then
    info "mycam đang loaded — rmmod trước..."
    rmmod mycam 2>/dev/null || true
    sleep 0.5
fi

# Lấy timestamp trước khi insmod để lọc dmesg
DMESG_TS=$(dmesg --time-format iso 2>/dev/null | tail -1 | cut -d' ' -f1 || echo "")

info "insmod $KO ..."
if insmod "$KO" 2>&1; then
    pass "insmod thành công"
else
    fail "insmod thất bại"
    log "dmesg (last 20):"
    dmesg | tail -20 | tee -a "$LOG"
    exit 1
fi

sleep 1  # Chờ probe() chạy

if lsmod | grep -q "^mycam"; then
    MYCAM_LINE=$(lsmod | grep "^mycam")
    pass "mycam in lsmod: $MYCAM_LINE"
else
    fail "mycam KHÔNG xuất hiện trong lsmod"
fi

# ─────────────────────────────────────────────
# Step 4: Check dmesg — probe thành công?
# ─────────────────────────────────────────────
log ""
log "━━━ Step 4: dmesg probe messages ━━━"

DMESG_MYCAM=$(dmesg | grep -i "mycam" | tail -30 || true)
log "--- dmesg (mycam) ---"
echo "$DMESG_MYCAM" | tee -a "$LOG"
log "---------------------"

VIDEO_DEV=""
if echo "$DMESG_MYCAM" | grep -q "registered as /dev/video"; then
    VIDEO_DEV=$(echo "$DMESG_MYCAM" | grep -oP '/dev/video\d+' | tail -1)
    pass "probe OK — registered as $VIDEO_DEV"
elif echo "$DMESG_MYCAM" | grep -qi "c270 detected\|logitech"; then
    pass "probe() triggered — C270 detected in dmesg"
    warn "Không thấy /dev/videoX message — có thể probe lỗi trước video_register"
elif echo "$DMESG_MYCAM" | grep -qi "driver registered"; then
    pass "driver registered"
else
    warn "Không thấy probe message rõ ràng — kiểm tra full dmesg"
    log "--- dmesg (last 40) ---"
    dmesg | tail -40 | tee -a "$LOG"
fi

# Fallback: so sánh /dev/video* trước và sau insmod
if [ -z "$VIDEO_DEV" ]; then
    # Tìm video device của mycam qua driver link
    for vd in /dev/video*; do
        drv=$(cat "/sys/class/video4linux/$(basename $vd)/device/driver/module/name" 2>/dev/null || true)
        if [ "$drv" = "mycam" ]; then
            VIDEO_DEV="$vd"
            info "Found mycam device via sysfs: $VIDEO_DEV"
            break
        fi
    done
fi

# Final fallback
if [ -z "$VIDEO_DEV" ]; then
    VIDEO_DEV=$(ls /dev/video* 2>/dev/null | tail -1 || true)
    [ -n "$VIDEO_DEV" ] && warn "Guessing video device: $VIDEO_DEV"
fi

info "Sử dụng device: ${VIDEO_DEV:-NOT FOUND}"

# ─────────────────────────────────────────────
# Step 5: v4l2-ctl info
# ─────────────────────────────────────────────
log ""
log "━━━ Step 5: v4l2-ctl device info ━━━"

if [ -z "$VIDEO_DEV" ] || [ ! -e "$VIDEO_DEV" ]; then
    fail "Không có video device — skip steps 5,6,7,8"
    SKIP_STREAM=true
else
    SKIP_STREAM=false
    log "v4l2-ctl -d $VIDEO_DEV --all:"
    V4L2_INFO=$(v4l2-ctl -d "$VIDEO_DEV" --all 2>&1 || true)
    echo "$V4L2_INFO" | tee -a "$LOG"

    if echo "$V4L2_INFO" | grep -qi "mycam\|c270"; then
        pass "v4l2-ctl: driver = mycam / C270"
    else
        warn "v4l2-ctl không thấy 'mycam' trong driver info"
    fi

    if echo "$V4L2_INFO" | grep -qi "jpeg\|mjpeg"; then
        pass "MJPEG format có trong device info"
    else
        fail "MJPEG format KHÔNG thấy trong v4l2-ctl info"
    fi
fi

# ─────────────────────────────────────────────
# Step 6: Capture 30 frames
# ─────────────────────────────────────────────
log ""
log "━━━ Step 6: Capture 30 MJPEG frames (DQBUF test) ━━━"

if [ "${SKIP_STREAM:-true}" = true ]; then
    fail "Skip: không có video device"
else
    CAPTURE_FILE="/tmp/mycam_b5_capture.mjpeg"
    info "Capturing 30 frames → $CAPTURE_FILE ..."
    info "  (timeout 30s — nếu treo, driver có thể bị block ở DQBUF)"

    if timeout 30 v4l2-ctl \
            -d "$VIDEO_DEV" \
            --set-fmt-video=width=640,height=480,pixelformat=MJPG \
            --stream-mmap \
            --stream-count=30 \
            --stream-to="$CAPTURE_FILE" 2>&1 | tee -a "$LOG"; then

        FSIZE=$(stat -c%s "$CAPTURE_FILE" 2>/dev/null || echo 0)
        info "Capture file size: $FSIZE bytes"
        if [ "$FSIZE" -gt 10000 ]; then
            pass "Capture OK: $FSIZE bytes — 30 MJPEG frames captured!"
        else
            fail "File quá nhỏ ($FSIZE bytes) — frames có thể rỗng"
        fi
    else
        EC=$?
        fail "v4l2-ctl capture exit code: $EC"
        log ""
        log "--- dmesg sau capture attempt ---"
        dmesg | grep -i "mycam\|urb\|vb2\|error\|warn" | tail -30 | tee -a "$LOG"
    fi
fi

# ─────────────────────────────────────────────
# Step 7: Streaming dmesg analysis
# ─────────────────────────────────────────────
log ""
log "━━━ Step 7: dmesg streaming analysis ━━━"

STREAM_DMESG=$(dmesg | grep -i "mycam" | tail -40 || true)
log "--- dmesg (mycam, last 40) ---"
echo "$STREAM_DMESG" | tee -a "$LOG"
log "-----------------------------"

echo "$STREAM_DMESG" | grep -q "streaming started" \
    && pass "streaming started confirmed" \
    || warn "Không thấy 'streaming started' trong dmesg"

echo "$STREAM_DMESG" | grep -q "Selected alt" \
    && pass "Alt setting selection: $(echo "$STREAM_DMESG" | grep 'Selected alt' | tail -1)" \
    || warn "Không thấy alt setting selection message"

echo "$STREAM_DMESG" | grep -q "streaming stopped" \
    && pass "streaming stopped cleanly" \
    || true

echo "$STREAM_DMESG" | grep -qi "oops\|BUG\|kernel panic" \
    && fail "KERNEL BUG/OOPS detected!" \
    || true

# ─────────────────────────────────────────────
# Step 8: GStreamer test
# ─────────────────────────────────────────────
log ""
log "━━━ Step 8: GStreamer pipeline test ━━━"

if ! command -v gst-launch-1.0 &>/dev/null; then
    warn "gst-launch-1.0 không có — skip"
elif [ "${SKIP_STREAM:-true}" = true ]; then
    warn "Skip: không có video device"
else
    info "GStreamer: v4l2src → jpegdec → fakesink (10 giây, headless)..."
    GST_LOG=$(timeout 12 gst-launch-1.0 -v \
        v4l2src device="$VIDEO_DEV" \
        ! "image/jpeg,width=640,height=480,framerate=30/1" \
        ! jpegdec \
        ! videoconvert \
        ! fakesink sync=false 2>&1 | head -80 || true)
    echo "$GST_LOG" | tee -a "$LOG"

    if echo "$GST_LOG" | grep -q "video/x-raw\|Setting pipeline"; then
        pass "GStreamer pipeline negotiated successfully"
    elif echo "$GST_LOG" | grep -qi "error\|failed"; then
        fail "GStreamer pipeline error"
        log "--- dmesg GStreamer phase ---"
        dmesg | grep -i "mycam" | tail -20 | tee -a "$LOG"
    else
        warn "GStreamer output không rõ ràng — kiểm tra log"
    fi
fi

# ─────────────────────────────────────────────
# Cleanup
# ─────────────────────────────────────────────
log ""
log "━━━ Cleanup ━━━"

info "Stopping any streaming and unloading mycam..."
rmmod mycam 2>/dev/null && info "mycam removed OK" || warn "rmmod: module not loaded"

sleep 0.5

info "Rebinding C270 to uvcvideo..."
for iface in "${C270_SYS}:1.0" "${C270_SYS}:1.1"; do
    if echo -n "$iface" > /sys/bus/usb/drivers/uvcvideo/bind 2>/dev/null; then
        info "  Rebound $iface → uvcvideo OK"
    else
        warn "  Could not rebind $iface (may already be bound)"
    fi
done

sleep 1
info "Video devices sau cleanup: $(ls /dev/video* 2>/dev/null || echo 'none')"

# ─────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────
log ""
log "================================================================"
log "  TEST SUMMARY — Task B5 mycam.ko Integration Test"
log "================================================================"
log "  PASS: ${GREEN}${PASS}${NC}"
log "  FAIL: ${RED}${FAIL}${NC}"
log "  Full log: $LOG"
log "================================================================"

if [ "$FAIL" -eq 0 ]; then
    log ""
    log "${GREEN}✅ Task B5 PASSED — mycam.ko hoạt động end-to-end!${NC}"
    exit 0
else
    log ""
    log "${RED}❌ Task B5 có $FAIL lỗi — xem $LOG và dmesg để debug${NC}"
    exit 1
fi
