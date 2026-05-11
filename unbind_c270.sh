#!/bin/bash
# ═══════════════════════════════════════════════════════
# Unbind uvcvideo driver từ Logitech C270
# Chạy script này TRƯỚC KHI build/run bất kỳ test nào
#
# Usage: sudo ./unbind_c270.sh [--rebind] [--status] [--quiet]
# ═══════════════════════════════════════════════════════

C270_VID="046d"
C270_PID="0825"
UVC_DRIVER_PATH="/sys/bus/usb/drivers/uvcvideo"

# ── Parse arguments ──
ACTION="unbind"
QUIET=0
for arg in "$@"; do
    case "$arg" in
        --rebind) ACTION="rebind" ;;
        --status) ACTION="status" ;;
        --quiet|-q) QUIET=1 ;;
        --help|-h)
            echo "Usage: sudo $0 [--rebind] [--status] [--quiet]"
            echo ""
            echo "  (default)   Unbind uvcvideo driver từ C270"
            echo "  --rebind    Rebind (reload) uvcvideo driver"
            echo "  --status    Kiểm tra trạng thái C270"
            echo "  --quiet     Ít output"
            exit 0
            ;;
    esac
done

log() { [ "$QUIET" -eq 0 ] && echo "$@"; }

# ── Check camera ──
if ! lsusb 2>/dev/null | grep -q "${C270_VID}:${C270_PID}"; then
    echo "ERROR: C270 không tìm thấy (${C270_VID}:${C270_PID}). Hãy cắm camera."
    exit 1
fi

# Lấy bus:device info
USB_INFO=$(lsusb -d "${C270_VID}:${C270_PID}" | head -1)
log "✓ C270 detected: $USB_INFO"

# ── Status ──
if [ "$ACTION" = "status" ]; then
    echo ""
    echo "── C270 Status ──"
    echo "USB: $USB_INFO"

    # Check kernel driver
    BOUND_IFACES=""
    if [ -d "$UVC_DRIVER_PATH" ]; then
        for entry in "$UVC_DRIVER_PATH"/*; do
            name=$(basename "$entry")
            # Skip non-device entries (module, bind, unbind, etc.)
            [[ "$name" =~ ^[0-9] ]] || continue
            # Check if this interface belongs to C270
            if [ -L "$entry" ]; then
                VENDOR=$(cat "$entry/../idVendor" 2>/dev/null || echo "")
                PRODUCT=$(cat "$entry/../idProduct" 2>/dev/null || echo "")
                if [ "$VENDOR" = "$C270_VID" ] && [ "$PRODUCT" = "$C270_PID" ]; then
                    BOUND_IFACES="$BOUND_IFACES $name"
                fi
            fi
        done
    fi

    if [ -n "$BOUND_IFACES" ]; then
        echo "Driver: uvcvideo (bound:$BOUND_IFACES)"
        echo "→ Cần unbind trước khi dùng custom driver"
    else
        echo "Driver: không bind (sẵn sàng cho custom driver)"
    fi

    # Check /dev/video
    VIDEOS=$(ls /dev/video* 2>/dev/null || echo "(không có)")
    echo "Devices: $VIDEOS"
    exit 0
fi

# ── Rebind ──
if [ "$ACTION" = "rebind" ]; then
    log ""
    log "Rebinding uvcvideo driver..."
    modprobe -r uvcvideo 2>/dev/null
    sleep 0.5
    modprobe uvcvideo
    log "✓ uvcvideo driver reloaded"
    ls /dev/video* 2>/dev/null && log "  Devices: $(ls /dev/video* 2>/dev/null)"
    exit 0
fi

# ── Unbind ──
log ""

# Check nếu uvcvideo driver có loaded không
if [ ! -d "$UVC_DRIVER_PATH" ]; then
    log "✓ uvcvideo driver không loaded — không cần unbind"
    exit 0
fi

# Tìm tất cả interfaces của C270
UNBOUND=0
for entry in "$UVC_DRIVER_PATH"/*; do
    name=$(basename "$entry")
    # Skip non-device entries
    [[ "$name" =~ ^[0-9] ]] || continue

    if [ -L "$entry" ]; then
        VENDOR=$(cat "$entry/../idVendor" 2>/dev/null || echo "")
        PRODUCT=$(cat "$entry/../idProduct" 2>/dev/null || echo "")

        if [ "$VENDOR" = "$C270_VID" ] && [ "$PRODUCT" = "$C270_PID" ]; then
            log "Unbinding: $name"
            echo "$name" > "$UVC_DRIVER_PATH/unbind" 2>/dev/null || true
            UNBOUND=$((UNBOUND + 1))
            # Sau khi unbind interface đầu tiên, các interface khác
            # của cùng device thường auto-release. Đợi rồi check lại.
            sleep 0.2
        fi
    fi
done

if [ "$UNBOUND" -eq 0 ]; then
    log "✓ C270 không bind với uvcvideo — đã sẵn sàng"
else
    log "✓ Unbind $UNBOUND interface(s)"
fi

# Verify
log ""
REMAINING=$(ls /dev/video* 2>/dev/null | wc -w)
if [ "$REMAINING" -eq 0 ]; then
    log "✓ Không còn /dev/videoX — unbind hoàn tất"
else
    log "  /dev/videoX còn lại: $(ls /dev/video* 2>/dev/null)"
    log "  (có thể từ camera khác)"
fi

log ""
log "✓ C270 sẵn sàng cho custom driver"
log "  Chạy: sudo ./build/c270_app"
log "  Rebind: sudo $0 --rebind"
exit 0
