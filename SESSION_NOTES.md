# C270 Driver — Ghi Chú Phiên Làm Việc

**Cập nhật lần cuối:** 2026-05-06 03:00 AM
**Trạng thái:** MJPEG 640x480 hoạt động, tất cả requirements đã implement

---

## 1. Trạng Thái Hiện Tại

### ✅ Đã hoàn thành

| Tính năng | Trạng thái |
|---|---|
| MJPEG 640x480 @ 30fps | ✅ Hoạt động |
| RTSP streaming (H264/H265) | ✅ Hoạt động |
| Local display (SDL2 + overlay) | ✅ Hoạt động |
| Hotplug auto-reconnect | ✅ Đã enable |
| CLI arguments đầy đủ | ✅ getopt_long |
| Exposure / Brightness / Contrast | ✅ UVC controls |
| Camera list (`--list`) | ✅ Scan USB |
| RTSP authentication | ✅ Basic auth |
| H265 encoding | ✅ x265enc pipeline |
| unbind_c270.sh (--rebind/--status) | ✅ Rewrite |
| Test scripts TC1-TC4 | ✅ |
| README.md đầy đủ requirements | ✅ |

### Cách chạy nhanh

```bash
# Build
cd build && cmake .. && make -j$(nproc)

# Unbind kernel driver
sudo bash unbind_c270.sh

# Chạy driver (default: MJPEG 640x480 H264)
sudo ./build/c270_app

# RTSP client
ffplay rtsp://127.0.0.1:8554/camera0

# Hoặc GStreamer (latency thấp hơn)
gst-launch-1.0 rtspsrc location=rtsp://127.0.0.1:8554/camera0 latency=0 ! decodebin ! autovideosink
```

---

## 2. Cấu Hình Đã Verify

### UVC Probe (đã xác nhận hoạt động)

| Format | FormatIndex | FrameIndex | Resolution | MaxPayloadTransfer |
|---|---|---|---|---|
| YUYV | 1 | 3 | 176x144 | 512 B |
| **MJPEG** | **2** | **1** | **640x480** | **1,280 B** |

### Alt Setting

Driver chọn **alt=10** (comfort mode, 2x headroom):
- Camera cần: 1280 bytes
- Alt=10 cho: 2688 bytes (effective)
- Kết quả: 0 JPEG corruption warnings

### Commits

```
eea03ae  Complete all requirements: exposure, camera list, H265, RTSP auth, README
14752e3  Approach B: CLI args, hotplug, unbind rewrite, test scripts
9f4a15d  Fix RTSP push-buffer -2 spam on client disconnect
a559bdf  Switch from YUYV 176x144 to MJPEG 640x480@30fps
a7927ad  Initial commit for GitNexus analysis
```

---

## 3. Vấn Đề Đã Biết

### VLC 3.0.x không kết nối được RTSP

**Nguyên nhân:** Module SAT>IP chặn URL `rtsp://`.
**Workaround:** Dùng ffplay hoặc GStreamer client. Đây là bug của VLC.

### IP Camera / ONVIF — chưa implement

Ngoài scope USB driver. Nếu cần, thêm module `ip_camera.c` sử dụng GStreamer `rtspsrc`.

### Windows — chưa test

Code dùng libusb + SDL2 + GStreamer (tất cả portable). Cần test build trên Windows.

---

## 4. Nếu Tiếp Tục Phát Triển

### Ưu tiên 1: Benchmark performance
```bash
# Đo CPU
sudo ./build/c270_app --no-display &
top -p $(pgrep c270_app)

# Đo memory leak
valgrind --leak-check=full sudo ./build/c270_app --no-display --no-stream
```

### Ưu tiên 2: Multi-camera
- Chạy nhiều instance với port khác nhau: `c270_app -p 8554 & c270_app -p 8555`
- Hoặc refactor thành camera manager

### Ưu tiên 3: Runtime config change
- Thêm stdin command reader hoặc Unix socket
- Cho phép thay đổi brightness/exposure/fps khi đang stream

### Ưu tiên 4: IP camera module
- Thêm `ip_camera.c` dùng GStreamer `rtspsrc` làm input
- ONVIF discovery với `onvif-ws-client`

---

## 5. File Quan Trọng

| File | Vai trò |
|---|---|
| `src/main.c` | Entry point, CLI parsing, event loop |
| `src/c270_uvc.c` | UVC protocol, camera controls, alt selection |
| `src/c270_capture.c` | ISO capture, MJPEG decode, custom JPEG handler |
| `src/c270_stream.c` | GStreamer RTSP server, H264/H265, auth |
| `src/c270_hotplug.c` | Watchdog thread, auto-reconnect |
| `unbind_c270.sh` | Unbind/rebind kernel driver |
| `README.md` | Tài liệu đầy đủ requirements + verification |
| `docs/WORK_LOG_2026-05-06.md` | Nhật ký công việc hôm nay |

---

## 6. CLI Quick Reference

```
sudo ./build/c270_app [OPTIONS]

  --list                  Liệt kê cameras
  -F mjpeg|yuyv           Format (default: mjpeg)
  -W N -H N               Resolution (default: 640x480)
  -f N                    FPS (default: 30)
  -C h264|h265            Codec (default: h264)
  -P password             RTSP password
  -e N                    Exposure manual (default: auto)
  -b N -c N               Brightness / Contrast
  -p N                    RTSP port (default: 8554)
  --no-display            Headless mode
  --no-stream             Disable RTSP
  --no-hotplug            Disable auto-reconnect
  --help                  Help
```
