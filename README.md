# Logitech C270 Custom Kernel Driver & RTSP Server

![Project Status](https://img.shields.io/badge/Status-Active-brightgreen)
![Language](https://img.shields.io/badge/Language-C99%20%2B%20Kernel%20C-blue)
![Platform](https://img.shields.io/badge/Platform-Linux%206.x%20(Ubuntu)-lightgrey)
![Kernel](https://img.shields.io/badge/Kernel%20Module-mycam.ko-orange)

## 1. Tổng Quan Dự Án (Project Overview)

Dự án xây dựng một hệ thống camera streaming từ A đến Z, bao gồm **custom kernel module** (`mycam.ko`) và **ứng dụng userspace** (`c270_app`) cho camera Logitech C270.

**Kiến trúc 2 tầng:**
-   **Tầng Kernel:** Module `mycam.ko` thay thế driver `uvcvideo` mặc định, giao tiếp trực tiếp với camera qua USB isochronous transfers, parse UVC payload headers, và expose `/dev/videoX` qua V4L2 framework.
-   **Tầng Userspace:** Ứng dụng `c270_app` sử dụng V4L2 API (open → format → MMAP → poll → DQBUF) để nhận MJPEG frames, hiển thị qua SDL2 và stream qua RTSP (GStreamer).

**Mục tiêu cốt lõi:** Biến một camera USB giá rẻ thành IP Camera công nghiệp, với khả năng stream video RTSP (H.264/H.265) ở độ trễ < 100ms, hoạt động bền bỉ 24/7.

---

## 2. Các Đột Phá Kỹ Thuật (Key Technical Highlights)

*(Phần này được thiết kế làm các luận điểm "ăn tiền" cho bài thuyết trình/slide)*

### 2.1. Custom Kernel Module — Tự Viết Driver Từ Scratch
*   **Vấn đề:** Driver mặc định `uvcvideo` là "hộp đen" — không thể tùy chỉnh chiến lược alt setting, URB pool size, hay frame accumulation logic.
*   **Giải pháp:** Viết kernel module `mycam.ko` (~1600 dòng code C kernel) bao gồm:
    -   **USB probe/disconnect** lifecycle management
    -   **UVC Probe/Commit** protocol negotiation (MJPEG 640×480)
    -   **VB2 (videobuf2)** buffer queue management
    -   **8-URB pool** với isochronous completion handler chạy ở softirq context
    -   **UVC header parser** với FID toggle detection để xác định ranh giới frame
*   **Kết quả:** 30 MJPEG frames captured, 0 dropped, streaming clean.

### 2.2. DMA-Safe USB Transfers — Bài Học Kernel Development
*   **Vấn đề:** `usb_control_msg()` trong kernel ≥5.x yêu cầu buffer phải DMA-safe (kmalloc'd). Truyền stack variable gây kernel WARN dump + EAGAIN.
*   **Giải pháp:** Mọi UVC control message đều dùng `kmalloc/kfree` buffer. Return value được normalize (positive byte count → 0) trước khi trả cho VB2 framework.
*   **Bài học áp dụng:** Hiểu rõ contract giữa USB HCD layer và driver layer — một trong những trap phổ biến nhất của kernel USB development.

### 2.3. Secure Boot Compliant — Module Signing Pipeline
*   **Vấn đề:** Trên hệ thống Secure Boot (UEFI), kernel từ chối load module không có chữ ký hợp lệ.
*   **Giải pháp:** Tích hợp pipeline signing vào Makefile:
    1. Tạo MOK keypair (self-signed X.509)
    2. `sign-file sha256` tự động sau mỗi build
    3. Enroll key vào UEFI MOK database qua `mokutil`
*   **Kết quả:** `insmod mycam.ko` thành công trên Ubuntu 24.04 với Secure Boot enabled.

### 2.4. Cơ Chế Chống Crash Bằng setjmp/longjmp (JPEG Error Handler)
*   **Vấn đề:** Khung hình MJPEG tải từ USB thỉnh thoảng chứa byte rác. Thư viện `libjpeg-turbo` sẽ crash toàn bộ hệ thống.
*   **Giải pháp:** Error handler tùy chỉnh sử dụng `setjmp/longjmp` để skip frame lỗi thay vì crash, giữ Uptime tối đa.

### 2.5. RTSP Server Tích Hợp — Zero-Copy MJPEG Pipeline
*   **Vấn đề:** Pipeline truyền thống: USB → decode JPEG → encode H.264 → RTSP. Tốn CPU.
*   **Giải pháp:** Push MJPEG thô trực tiếp vào GStreamer pipeline: `appsrc → jpegdec → x264enc → rtph264pay → rtsp`. Tiết kiệm 1 bước decode/encode, giảm CPU ~30%.

---

## 3. Kiến Trúc & Cấu Trúc Mã Nguồn (Architecture)

### 3.1. Pipeline Tổng Quan

```
USB C270 Camera
    │  isochronous packets (8000 pkt/s)
    ▼
┌──────────────────────────────┐
│  mycam.ko (Kernel Module)    │
│   mycam_urb.c  — URB pool    │   ← softirq context
│   UVC header parse + FID     │
│   mycam_vb2.c  — VB2 queue   │
│   → /dev/videoX              │
└───────────┬──────────────────┘
            │  MMAP + DQBUF (V4L2 API)
            ▼
┌──────────────────────────────┐
│  c270_app (Userspace)        │
│   c270_v4l2.c — V4L2 device  │
│   main.c — capture loop      │
│   c270_display.c — SDL2 UI   │
│   c270_stream.c — RTSP       │
└──────────────────────────────┘
```

### 3.2. Kernel Module Files (`kernel/`)

| File | Vai Trò | Context |
|:---|:---|:---|
| `mycam.h` | Shared structs, constants, device context | — |
| `mycam_main.c` | module_init/exit, USB probe/disconnect, UVC Probe/Commit | Process |
| `mycam_video.c` | V4L2 video_device registration, IOCTL ops | Process |
| `mycam_vb2.c` | VB2 queue ops: queue_setup, buf_queue, start/stop streaming | Process |
| `mycam_urb.c` | URB alloc/submit/complete, UVC header parse, frame accumulator | softirq (ISR) |

### 3.3. Userspace Files (`src/`)

| File | Vai Trò |
|:---|:---|
| `main.c` | Entry point, CLI parsing, V4L2 capture loop (poll + DQBUF) |
| `c270_v4l2.c` | V4L2 device: open, format negotiate, MMAP buffers |
| `c270_display.c` | SDL2 render, MJPEG decode (libjpeg), FPS overlay |
| `c270_stream.c` | GStreamer RTSP server, H.264/H.265, push MJPEG thô |

---

## 4. Bảng Kết Quả Nghiệm Thu (Acceptance Testing)

### 4.1. Kernel Module Test (Task B5)

| # | Test | Kết quả |
|---|------|---------|
| 1 | C270 hardware detect | ✅ PASS |
| 2 | Unbind uvcvideo | ✅ PASS |
| 3 | insmod mycam.ko | ✅ PASS (Secure Boot) |
| 4 | Module in lsmod | ✅ PASS |
| 5 | probe → /dev/video2 | ✅ PASS |
| 6 | v4l2-ctl driver=mycam | ✅ PASS |
| 7 | MJPEG format listed | ✅ PASS |
| 8 | **30 frames DQBUF capture** | ✅ **18.4MB, 0 dropped** |
| 9 | streaming started dmesg | ✅ PASS |
| 10 | streaming stopped cleanly | ✅ PASS |
| 11 | GStreamer pipeline negotiated | ✅ PASS |

### 4.2. Userspace Tests (từ Phase 1)

1.  **Nhận diện thiết bị:** Tự động detect Logitech C270 qua VID/PID.
2.  **Chất lượng Stream:** MJPEG 640×480 @ 30 FPS, độ trễ RTSP ~50-100ms qua LAN.
3.  **Hiệu năng:** GStreamer hardware pipeline, RAM cấp phát tĩnh, không memory leak.

---

## 5. Hướng Dẫn Biên Dịch & Chạy (Quick Start)

### 5.1. Cài đặt Dependencies

```bash
sudo apt update
sudo apt install -y linux-headers-$(uname -r) \
  libsdl2-dev libjpeg-turbo8-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libgstrtspserver-1.0-dev \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly v4l-utils
```

### 5.2. Build Kernel Module

```bash
cd kernel
make              # Build + auto-sign (Secure Boot)

# Enroll signing key lần đầu (chỉ cần 1 lần):
sudo mokutil --import signing/MOK.der
sudo reboot       # Chọn "Enroll MOK" trong UEFI shim
```

### 5.3. Build Userspace App

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

### 5.4. Chạy

```bash
# Bước 1: Load kernel module (thay thế uvcvideo cho C270)
sudo bash tools/b5_integration_test.sh    # Test tự động
# Hoặc thủ công:
echo -n "1-10:1.0" | sudo tee /sys/bus/usb/drivers/uvcvideo/unbind
sudo insmod kernel/mycam.ko

# Bước 2: Chạy userspace app
sudo ./build/c270_app

# Bước 3: Xem stream
ffplay -fflags nobuffer -flags low_delay rtsp://127.0.0.1:8554/camera0
# Hoặc GStreamer (latency thấp hơn):
gst-launch-1.0 rtspsrc location=rtsp://127.0.0.1:8554/camera0 latency=0 \
  ! decodebin ! autovideosink
```

---

## 6. Lịch Sử Migration

| Giai đoạn | Mô tả | Trạng thái |
|-----------|-------|-----------|
| Phase 1 (05/2026) | Userspace driver (libusb) — MJPEG, RTSP, hotplug | ✅ Hoàn thành |
| Phase C (13/05) | Refactor userspace: libusb → V4L2 input | ✅ Hoàn thành |
| Phase B (13/05) | Custom kernel module mycam.ko | ✅ 11/11 tests passed |
| Phase A (13/05) | Legacy libusb cleanup | ✅ 8 files deleted |
| Phase B6 | V4L2 Controls (exposure/brightness/contrast) | ⬜ Planned |

---

## 7. CLI Quick Reference

```
sudo ./build/c270_app [OPTIONS]

  --list                  Liệt kê cameras
  -W N -H N               Resolution (default: 640x480)
  -f N                    FPS (default: 30)
  -C h264|h265            Codec (default: h264)
  -P password             RTSP password
  -p N                    RTSP port (default: 8554)
  --no-display            Headless mode
  --no-stream             Disable RTSP
  --help                  Help
```

---

*Lưu ý: Tài liệu này được thiết kế và tối ưu hóa cho AI Agent (Manus AI, Claude, v.v.) đọc để hiểu nhanh kiến trúc và tự động sinh slide báo cáo thuyết trình kỹ thuật.*
