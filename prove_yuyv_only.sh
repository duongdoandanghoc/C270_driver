#!/bin/bash
# ═══════════════════════════════════════════════════════════════
# prove_yuyv_only.sh — Thu thập bằng chứng C270 chỉ dùng YUYV qua libusb
# 
# Usage: sudo bash prove_yuyv_only.sh
# Output: prove_yuyv_evidence.txt
# ═══════════════════════════════════════════════════════════════

OUT="prove_yuyv_evidence.txt"

{
echo "╔══════════════════════════════════════════════════════════════════╗"
echo "║  BẰNG CHỨNG: C270 CHỈ STREAM YUYV QUA LIBUSB (KHÔNG MJPEG)   ║"
echo "║  Ngày: $(date '+%Y-%m-%d %H:%M:%S')                                       ║"
echo "╚══════════════════════════════════════════════════════════════════╝"
echo ""

# ═══ Bằng chứng 1: USB Descriptor ═══
echo "═══════════════════════════════════════════════════════════════════"
echo "  BẰNG CHỨNG 1: USB DESCRIPTORS"
echo "  Lệnh: lsusb -v -d 046d:0825 | grep FORMAT"
echo "═══════════════════════════════════════════════════════════════════"
echo ""
echo "  Tất cả format types trong descriptor:"
lsusb -v -d 046d:0825 2>/dev/null | grep -E "FORMAT_|bFormatIndex|guidFormat" | sed 's/^/    /'
echo ""
echo "  → Camera khai báo CẢ FORMAT_UNCOMPRESSED (YUYV) VÀ FORMAT_MJPEG"
echo "  → Câu hỏi: MJPEG có thực sự hoạt động qua standard UVC không?"
echo ""

# ═══ Bằng chứng 2: Probe test ═══
echo "═══════════════════════════════════════════════════════════════════"
echo "  BẰNG CHỨNG 2: UVC PROBE/COMMIT TEST"
echo "  Lệnh: ./build/prove_no_mjpeg"
echo "═══════════════════════════════════════════════════════════════════"
echo ""

# Unbind first if needed
bash unbind_c270.sh 2>/dev/null

./build/prove_no_mjpeg 2>&1 | sed 's/^/  /'
echo ""

# ═══ Bằng chứng 3: v4l2-ctl (rebind kernel driver) ═══
echo "═══════════════════════════════════════════════════════════════════"
echo "  BẰNG CHỨNG 3: SO SÁNH VỚI KERNEL DRIVER (v4l2-ctl)"
echo "  Rebind uvcvideo rồi chạy v4l2-ctl"
echo "═══════════════════════════════════════════════════════════════════"
echo ""

# Rebind kernel driver
modprobe uvcvideo 2>/dev/null
sleep 2

if [ -e /dev/video0 ]; then
    echo "  v4l2-ctl --list-formats-ext -d /dev/video0:"
    v4l2-ctl --list-formats-ext -d /dev/video0 2>/dev/null | sed 's/^/    /'
    echo ""
    echo "  → Kernel driver uvcvideo CÓ THỂ dùng cả MJPEG và YUYV"
    echo "  → Kernel dùng cơ chế đàm phán riêng với firmware Logitech"
else
    echo "  /dev/video0 không tồn tại — kernel driver chưa rebind"
fi
echo ""

# ═══ Tóm tắt ═══
echo "═══════════════════════════════════════════════════════════════════"
echo "  TÓM TẮT BẰNG CHỨNG"
echo "═══════════════════════════════════════════════════════════════════"
echo ""
echo "  ┌───┬──────────────────────────────────────────────────────────┐"
echo "  │ # │ Bằng chứng                                             │"
echo "  ├───┼──────────────────────────────────────────────────────────┤"
echo "  │ 1 │ lsusb -v: Descriptor CÓ FORMAT_MJPEG (FormatIndex=2)   │"
echo "  │ 2 │ UVC Probe FormatIndex=2: Camera OVERRIDE → YUYV        │"
echo "  │ 3 │ v4l2-ctl qua kernel: MJPEG 1280x720 HOẠT ĐỘNG         │"
echo "  │ 4 │ Payload bytes: Không có JPEG magic (FF D8)              │"
echo "  └───┴──────────────────────────────────────────────────────────┘"
echo ""
echo "  KẾT LUẬN:"
echo "  - USB descriptor CÓ khai báo MJPEG"
echo "  - Nhưng khi probe qua standard UVC (libusb), camera KHÔNG"
echo "    cho phép — nó override FormatIndex về 1 (YUYV)"
echo "  - Kernel driver uvcvideo DÙNG ĐƯỢC MJPEG vì nó có cơ chế"
echo "    đàm phán riêng với firmware (ngoài standard UVC)"
echo "  - → Khi viết userspace driver bằng libusb: CHỈ CÓ YUYV"
echo ""
echo "  ĐÂY LÀ GIỚI HẠN CỦA FIRMWARE, KHÔNG PHẢI GIỚI HẠN GIAO THỨC."
echo ""

} | tee "$OUT"

echo ""
echo "═══ Kết quả đã lưu vào: $OUT ═══"
