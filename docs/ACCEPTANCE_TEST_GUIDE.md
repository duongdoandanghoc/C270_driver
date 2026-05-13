# Hướng Dẫn Nghiệm Thu — C270 Custom Kernel Driver

**Dự án:** Logitech C270 Custom V4L2 Kernel Driver & RTSP Server
**Phiên bản:** v4.0 (Phase B6 — V4L2 Controls + Auto-Reconnect)
**Ngày:** 13/05/2026
**Hệ thống test:** Ubuntu 24.04, Kernel 6.17.0-23-generic, Secure Boot ON

---

## Chuẩn Bị Trước Khi Nghiệm Thu

### Yêu cầu phần cứng
- Máy tính chạy Linux (Ubuntu 22.04+ hoặc tương đương)
- Camera Logitech C270 kết nối qua USB
- Có kết nối mạng LAN (cho test RTSP)

### Cài đặt dependencies
```bash
sudo apt update
sudo apt install -y linux-headers-$(uname -r) \
  libsdl2-dev libjpeg-turbo8-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libgstrtspserver-1.0-dev \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly v4l-utils ffmpeg
```

### Build hệ thống

```bash
# 1. Build kernel module
cd kernel && make
# Lần đầu cần enroll key cho Secure Boot:
# sudo mokutil --import signing/MOK.der → reboot → Enroll MOK

# 2. Build userspace app
cd .. && mkdir -p build && cd build && cmake .. && make -j$(nproc)
```

### Load kernel module (thay thế uvcvideo cho C270)

```bash
# Tìm USB path của C270
lsusb | grep C270
# Output: Bus 001 Device 004: ID 046d:0825 Logitech, Inc. Webcam C270

# Unbind C270 khỏi uvcvideo (giữ nguyên webcam tích hợp)
echo -n "1-10:1.0" | sudo tee /sys/bus/usb/drivers/uvcvideo/unbind

# Load custom driver
sudo insmod kernel/mycam.ko

# Verify
v4l2-ctl -d /dev/video2 --all | head -20
# Phải thấy: Driver name: mycam
```

---

## Bảng Nghiệm Thu

### TC1: Nhận Diện Thiết Bị (Device Recognition)

| # | Bước kiểm tra | Lệnh / Thao tác | Kết quả mong đợi | Pass/Fail |
|---|---------------|-----------------|------------------|-----------|
| 1.1 | Detect camera USB | `lsusb \| grep C270` | Hiện: `046d:0825 Logitech, Inc. Webcam C270` | ☐ |
| 1.2 | Load kernel module | `sudo insmod kernel/mycam.ko` | Không báo lỗi, module xuất hiện trong `lsmod` | ☐ |
| 1.3 | dmesg probe log | `dmesg \| grep mycam` | Hiện: `mycam: Logitech C270 detected` | ☐ |
| 1.4 | Device node tạo ra | `ls /dev/video*` | Xuất hiện `/dev/videoX` mới | ☐ |
| 1.5 | Driver name đúng | `v4l2-ctl -d /dev/video2 --all \| grep Driver` | `Driver name: mycam` | ☐ |
| 1.6 | Card type đúng | `v4l2-ctl -d /dev/video2 --all \| grep Card` | `Card type: Logitech C270 (mycam)` | ☐ |

**Cách test:**
```bash
# Cắm camera C270 vào USB
lsusb | grep C270

# Unbind uvcvideo + Load mycam.ko
echo -n "1-10:1.0" | sudo tee /sys/bus/usb/drivers/uvcvideo/unbind
sudo insmod kernel/mycam.ko

# Kiểm tra
dmesg | tail -10
v4l2-ctl -d /dev/video2 --all | head -15
```

---

### TC2: Khởi Tạo & Kết Nối (Initialization)

| # | Bước kiểm tra | Thao tác | Kết quả mong đợi | Pass/Fail |
|---|---------------|---------|------------------|-----------|
| 2.1 | Mở kết nối < 3 giây | Chạy `c270_app`, đo thời gian tới frame đầu tiên | Frame xuất hiện trong < 3 giây | ☐ |
| 2.2 | Rút camera → hiện thông báo | Rút cáp USB khi đang stream | Cửa sổ SDL2 chuyển **nền đỏ** + title: `⚠ DISCONNECTED` | ☐ |
| 2.3 | Cắm lại → tự reconnect | Cắm lại camera | Cửa sổ chuyển **nền cam** (reconnecting) → **nền xanh** (connected) → resume stream | ☐ |
| 2.4 | Rút/cắm 5 lần liên tiếp | Rút + cắm nhanh 5 lần | Không crash, mỗi lần reconnect thành công | ☐ |
| 2.5 | Rút lâu (30 giây) rồi cắm | Rút 30 giây, cắm lại | Reconnect thành công, stream hoạt động bình thường | ☐ |

**Cách test:**
```bash
# Terminal 1: Chạy driver
sudo ./build/c270_app -d /dev/video2

# Quan sát cửa sổ SDL2:
# 1. Cửa sổ hiện video camera → OK
# 2. RÚT cáp USB → cửa sổ chuyển NỀN ĐỎ, title: "⚠ DISCONNECTED"
# 3. CẮM lại → nền CAM "⟳ RECONNECTING" → nền XANH "✓ CONNECTED"
# 4. Video tiếp tục stream → OK
# 5. Lặp lại 5 lần

# Terminal 2: Theo dõi log
dmesg -w | grep mycam
```

> **Lưu ý:** Khi rút camera rồi cắm lại, mycam.ko tự probe lại nhưng device node có thể đổi (ví dụ: /dev/video2 → /dev/video4). App sẽ tự detect device mới qua V4L2 open.

---

### TC3: Chất Lượng Stream (Stream Quality)

| # | Bước kiểm tra | Lệnh / Thao tác | Kết quả mong đợi | Pass/Fail |
|---|---------------|-----------------|------------------|-----------|
| 3.1 | Độ phân giải 640×480 | `v4l2-ctl -d /dev/video2 --get-fmt-video` | `Width/Height: 640/480` | ☐ |
| 3.2 | FPS ổn định ≥ 25fps | Chạy `v4l2-ctl --stream-mmap --stream-count=300` | `fps: ~30.0` hiển thị ổn định | ☐ |
| 3.3 | Không drop frame | Kiểm tra dmesg sau capture | `streaming stopped (frames=N dropped=0)` | ☐ |
| 3.4 | Không artifact/vỡ hình | Quan sát video 1 phút | Hình ảnh sắc nét, không xé hình | ☐ |
| 3.5 | RTSP latency < 100ms | Chạy ffplay, so sánh với thực tế | Độ trễ < 100ms (vẫy tay so sánh) | ☐ |

**Cách test:**
```bash
# Test FPS + drop count (kernel module trực tiếp)
sudo bash tools/b5_integration_test.sh
# Kiểm tra output: frames=30 dropped=0

# Test stream thực tế
sudo ./build/c270_app -d /dev/video2
# Tab khác:
ffplay -fflags nobuffer -flags low_delay rtsp://127.0.0.1:8554/camera0
# Hoặc GStreamer (latency thấp hơn):
gst-launch-1.0 rtspsrc location=rtsp://127.0.0.1:8554/camera0 latency=0 \
  ! decodebin ! autovideosink
```

---

### TC4: Định Dạng & Tương Thích (Format & Compatibility)

| # | Bước kiểm tra | Lệnh | Kết quả mong đợi | Pass/Fail |
|---|---------------|------|------------------|-----------|
| 4.1 | MJPEG format hỗ trợ | `v4l2-ctl -d /dev/video2 --list-formats` | `'MJPG' (Motion-JPEG)` | ☐ |
| 4.2 | H.264 RTSP output | `ffprobe rtsp://127.0.0.1:8554/camera0 2>&1 \| grep codec` | `Video: h264` | ☐ |
| 4.3 | H.265 RTSP output | Chạy với `-C h265`, kiểm tra `ffprobe` | `Video: hevc` | ☐ |
| 4.4 | GStreamer compatible | `gst-launch-1.0 v4l2src device=/dev/video2 ! jpegdec ! fakesink` | Pipeline chạy, không error | ☐ |

**Cách test:**
```bash
# Liệt kê formats
v4l2-ctl -d /dev/video2 --list-formats

# Test H264
sudo ./build/c270_app -d /dev/video2 -C h264 --no-display &
ffprobe rtsp://127.0.0.1:8554/camera0 2>&1 | grep "Video:"

# Test H265
sudo ./build/c270_app -d /dev/video2 -C h265 --no-display &
ffprobe rtsp://127.0.0.1:8554/camera0 2>&1 | grep "Video:"
```

---

### TC5: Hiệu Năng (Performance)

| # | Bước kiểm tra | Lệnh | Kết quả mong đợi | Pass/Fail |
|---|---------------|------|------------------|-----------|
| 5.1 | CPU usage < 30% | `top -p $(pgrep c270_app)` | CPU% < 30% khi stream 640x480 | ☐ |
| 5.2 | RAM ổn định (không leak) | Chạy 5 phút, ghi nhận RSS mỗi phút | RSS không tăng liên tục | ☐ |
| 5.3 | Kernel module memory | `cat /proc/modules \| grep mycam` | Size nhỏ (< 50KB), ổn định | ☐ |

**Cách test:**
```bash
# Chạy driver headless
sudo ./build/c270_app -d /dev/video2 --no-display &

# Đo CPU + RAM mỗi 60 giây trong 5 phút
for i in $(seq 1 5); do
  ps -p $(pgrep c270_app) -o pid,%cpu,%mem,rss --no-headers
  sleep 60
done

# Kết quả ví dụ:
#   PID  %CPU %MEM   RSS
#  1234   4.2  0.1  8432    ← OK, CPU thấp, RAM ổn định
```

---

### TC6: Xử Lý Lỗi (Error Handling)

| # | Bước kiểm tra | Thao tác | Kết quả mong đợi | Pass/Fail |
|---|---------------|---------|------------------|-----------|
| 6.1 | Rút camera → không crash | Rút USB khi đang stream | App không crash, hiện thông báo trên cửa sổ | ☐ |
| 6.2 | Device không tồn tại | Chạy `c270_app -d /dev/video99` | In lỗi "cannot open", không crash | ☐ |
| 6.3 | JPEG frame hỏng | Hoạt động tự nhiên (thỉnh thoảng xảy ra) | Frame lỗi bị skip, không crash (setjmp/longjmp) | ☐ |
| 6.4 | RTSP client disconnect | Ngắt ffplay đột ngột | Không spam log, GStreamer dọn rác tự động | ☐ |
| 6.5 | rmmod khi đang stream | `sudo rmmod mycam` | Module unload sạch, app hiện disconnect status | ☐ |
| 6.6 | Log đầy đủ | `dmesg \| grep mycam` | Log đầy đủ: probe, stream start/stop, disconnect | ☐ |
| 6.7 | Không gây kernel panic | Tất cả test cases trên | Hệ thống ổn định, không OOPS/WARN/BUG | ☐ |

**Cách test:**
```bash
# Test 6.1: Rút camera
sudo ./build/c270_app -d /dev/video2
# → Rút USB → Cửa sổ NỀN ĐỎ "Camera đã bị rút!"
# → App KHÔNG crash → ✅

# Test 6.2: Device không tồn tại
sudo ./build/c270_app -d /dev/video99
# Output: "[V4L2] Cannot open /dev/video99: No such file or directory"
# App thoát sạch → ✅

# Test 6.5: rmmod khi stream
sudo ./build/c270_app -d /dev/video2 &
sleep 3
sudo rmmod mycam
# → App hiện disconnect status, không crash → ✅

# Test 6.6: Log đầy đủ
dmesg | grep mycam
# → Phải thấy: probe, video registered, controls registered,
#    streaming started, streaming stopped, disconnecting
```

---

## Bảng Tổng Hợp Kết Quả

| Tiêu chí | Số test | Pass | Fail | Kết quả |
|----------|---------|------|------|---------|
| TC1: Nhận diện thiết bị | 6 | | | ☐ |
| TC2: Khởi tạo & Kết nối | 5 | | | ☐ |
| TC3: Chất lượng Stream | 5 | | | ☐ |
| TC4: Định dạng & Tương thích | 4 | | | ☐ |
| TC5: Hiệu năng | 3 | | | ☐ |
| TC6: Xử lý lỗi | 7 | | | ☐ |
| **TỔNG** | **30** | | | |

### Điều kiện đạt nghiệm thu:
- **Đạt:** ≥ 27/30 tests pass (90%) VÀ không có test TC6 nào fail
- **Đạt có điều kiện:** 24-26/30 pass, các test fail có workaround
- **Không đạt:** < 24/30 pass HOẶC có kernel crash

---

## Thông Tin Hệ Thống

| Thông tin | Giá trị |
|-----------|---------|
| Kernel | 6.17.0-23-generic |
| Module | mycam.ko (custom V4L2 driver) |
| Camera | Logitech C270 (VID:046d PID:0825) |
| Resolution | MJPEG 640×480 |
| FPS | 30fps |
| RTSP Codec | H.264 / H.265 |
| Secure Boot | Enabled (MOK key enrolled) |
| V4L2 Controls | 8 controls (brightness, contrast, saturation, sharpness, gain, gamma, exposure auto, exposure time) |

---

## Lịch Sử Phiên Bản

| Version | Ngày | Thay đổi |
|---------|------|---------|
| v1.0 | 05/05/2026 | Userspace libusb driver, YUYV → MJPEG |
| v2.0 | 06/05/2026 | RTSP H264/H265, hotplug, CLI args |
| v3.0 | 12/05/2026 | V4L2 userspace pipeline (thay libusb) |
| v4.0 | 13/05/2026 | Custom kernel module mycam.ko + V4L2 Controls + Auto-Reconnect visual |

---

*Tài liệu này là hướng dẫn nghiệm thu chính thức. Người nghiệm thu điền kết quả vào cột Pass/Fail, ký tên xác nhận.*

**Người thực hiện:** _________________________ **Ngày:** ___/___/2026

**Người nghiệm thu:** _________________________ **Ngày:** ___/___/2026
