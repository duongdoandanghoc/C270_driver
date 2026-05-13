# C270 Driver — Ghi Chú Phiên Làm Việc

**Cập nhật lần cuối:** 2026-05-13 11:30 AM
**Trạng thái:** Kernel module mycam.ko hoạt động, legacy libusb đã xóa

---

## 1. Trạng Thái Hiện Tại

### ✅ Đã hoàn thành

| Tính năng | Trạng thái | Ghi chú |
|---|---|---|
| **Kernel module mycam.ko** | ✅ 11/11 tests | Custom USB/V4L2/VB2/URB driver |
| Secure Boot signing | ✅ | MOK key enrolled, auto-sign in Makefile |
| /dev/videoX registration | ✅ | V4L2 device, MJPEG 640x480 |
| 30fps MJPEG streaming | ✅ | 0 frames dropped |
| V4L2 userspace pipeline | ✅ | poll + DQBUF capture loop |
| RTSP streaming (H264/H265) | ✅ | GStreamer pipeline |
| Local display (SDL2 + overlay) | ✅ | libjpeg MJPEG decode |
| CLI arguments | ✅ | getopt_long |
| RTSP authentication | ✅ | Basic auth |
| Legacy libusb cleanup | ✅ | 8 files deleted |

### ⬜ Chưa làm

| Tính năng | Ưu tiên |
|---|---|
| V4L2 Controls (exposure/brightness/contrast) | 🔥 Task B6 — tiếp theo |
| Benchmark với mycam.ko | Sau B6 |
| Multi-camera support | Low |

### Cách chạy nhanh

```bash
# Build kernel module
cd kernel && make   # auto-sign for Secure Boot

# Build userspace app
cd build && cmake .. && make -j$(nproc)

# Test kernel module (automated)
sudo bash tools/b5_integration_test.sh

# Chạy thủ công
echo -n "1-10:1.0" | sudo tee /sys/bus/usb/drivers/uvcvideo/unbind
sudo insmod kernel/mycam.ko
sudo ./build/c270_app

# RTSP client
ffplay rtsp://127.0.0.1:8554/camera0
```

---

## 2. Cấu Hình Đã Verify

### UVC Probe (kernel module mycam.ko)

| Format | FormatIndex | FrameIndex | Resolution | Verified |
|---|---|---|---|---|
| **MJPEG** | **1** | **1** | **640x480** | ✅ mycam.ko |

### Alt Setting

Kernel module tự chọn alt setting nhỏ nhất đủ bandwidth (chuẩn UVC spec):
- dwMaxPayloadTransferSize từ camera negotiate
- Chọn alt có effective_pkt_size >= cần thiết

### Commits (branch: phase-c-v4l2-migration)

```
d2603eb  refactor(phase-a): remove legacy libusb code, add build gitignore
cddcb98  fix(phase-b): mycam.ko integration test passing — 3 USB driver bugs fixed
1fe74c4  feat(phase-b): add mycam.ko kernel module scaffold
67d1dab  fix(phase-c): resolve conflicting types warnings
095930f  feat(phase-c): CMake uses V4L2 pipeline, remove libusb
ef12571  feat(phase-c): rewrite main.c for V4L2 pipeline
```

---

## 3. Vấn Đề Đã Biết

### Secure Boot — MOK enrollment cần reboot
Lần đầu build trên máy mới cần: `mokutil --import` → reboot → Enroll MOK trong UEFI shim.

### VLC 3.0.x không kết nối được RTSP
Module SAT>IP chặn URL `rtsp://`. Dùng ffplay hoặc GStreamer client.

### GStreamer v4l2src buffer pool activation
`gst-launch-1.0 v4l2src` có thể fail "Buffer pool activation failed" trong một số trường hợp.
Workaround: dùng `v4l2-ctl --stream-mmap` để test trực tiếp, hoặc set `num-buffers` prop.

---

## 4. File Quan Trọng

| File | Vai trò |
|---|---|
| **kernel/mycam_main.c** | Module init, USB probe, UVC Probe/Commit |
| **kernel/mycam_video.c** | V4L2 device, IOCTL ops |
| **kernel/mycam_vb2.c** | VB2 queue ops, start/stop streaming |
| **kernel/mycam_urb.c** | URB pool, UVC header parse, frame accumulator |
| **kernel/mycam.h** | Shared structs, constants |
| `src/main.c` | Userspace entry, V4L2 capture loop |
| `src/c270_v4l2.c` | V4L2 device open/format/mmap |
| `src/c270_stream.c` | GStreamer RTSP server |
| `tools/b5_integration_test.sh` | Automated kernel module test |
| `docs/WORK_LOG_2026-05-13.md` | Nhật ký hôm nay |
| `docs/superpowers/specs/2026-05-13-v4l2-kernel-migration-design.md` | Design spec |

---

## 5. CLI Quick Reference

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
