# Logitech C270 Custom UVC Driver & RTSP Server

![Project Status](https://img.shields.io/badge/Status-Completed-success)
![Language](https://img.shields.io/badge/Language-C99-blue)
![Platform](https://img.shields.io/badge/Platform-Linux%20IPC-lightgrey)

## 1. Tổng Quan Dự Án (Project Overview)

Đây là một trình điều khiển (driver) tùy chỉnh hoạt động ở chế độ người dùng (Userspace) dành cho camera Logitech C270. Dự án giao tiếp trực tiếp với phần cứng USB thông qua `libusb-1.0`, bỏ qua hoàn toàn kernel driver mặc định (`uvcvideo`) của hệ điều hành Linux. 

**Mục tiêu cốt lõi:** Biến một camera USB giá rẻ thành một IP Camera theo chuẩn công nghiệp, có khả năng stream video qua giao thức RTSP (H.264/H.265) với độ trễ siêu thấp (< 100ms), hoạt động bền bỉ 24/7 và có khả năng tự động phục hồi khi mất kết nối.

---

## 2. Các Đột Phá Kỹ Thuật (Key Technical Highlights)

*(Phần này được thiết kế làm các luận điểm "ăn tiền" cho bài thuyết trình/slide)*

Dự án này đã giải quyết thành công 3 bài toán khó nhất của USB Camera Streaming trong môi trường công nghiệp:

### 2.1. Thuật toán "Comfort Mode" - Vượt Giới Hạn Băng Thông USB
*   **Vấn đề:** Chuẩn USB Isochronous không hỗ trợ truyền lại gói tin khi bị lỗi. Theo mặc định, với MJPEG 640x480, camera tính toán gói tin là 1280 bytes (Alt Setting 7). Nếu dùng mức này, luồng video sẽ liên tục bị xé hình (artifact) do thiếu hụt băng thông tức thời.
*   **Giải pháp:** Driver chủ động can thiệp và bắt ép `libusb` cấp phát **Alt Setting = 10 (2688 Bytes)**. Kỹ thuật này tạo ra bộ đệm băng thông dư dả gấp đôi (2x headroom), đảm bảo truyền tải 30 FPS mượt mà tuyệt đối với 0% rớt gói tin (packet loss).

### 2.2. Cơ Chế Chống Crash Bằng setjmp/longjmp (JPEG Error Handler)
*   **Vấn đề:** Khung hình MJPEG tải từ USB thỉnh thoảng sẽ chứa byte rác hoặc thiếu hụt đuôi. Thư viện `libjpeg-turbo` mặc định cực kỳ nhạy cảm: nó sẽ làm crash toàn bộ hệ thống hoặc spam log cảnh báo gây tràn RAM.
*   **Giải pháp:** Xây dựng một Error Handler tùy chỉnh sử dụng cấu trúc `setjmp / longjmp` của C. Khi phát hiện dữ liệu hỏng, luồng sẽ "búng" (longjmp) ra ngoài, chủ động **bỏ qua (skip) khung hình bị lỗi** thay vì crash chương trình, giúp hệ thống duy trì Uptime tối đa.

### 2.3. Máy Trạng Thái (State Machine) & Tự Dọn Rác (Garbage Collection)
*   **Auto-Reconnect:** Luồng Watchdog độc lập liên tục theo dõi cổng USB. Rút camera ra -> Pause hệ thống. Cắm lại -> Tự động khôi phục luồng stream dưới 3 giây.
*   **Chống Memory Leak RTSP:** Khi Client mạng (VLC, ffplay) đột ngột ngắt kết nối, GStreamer thường bị kẹt buffer. Driver lắng nghe tín hiệu `media-unprepared` để lập tức dọn rác (clear appsrc), giữ mức tiêu thụ RAM luôn phẳng lặng cho dù có hàng chục client kết nối/ngắt kết nối liên tục.

---

## 3. Kiến Trúc & Cấu Trúc Mã Nguồn (Architecture)

Hệ thống được thiết kế theo dạng Module hóa (Modular Design). Dưới đây là sơ đồ nhiệm vụ của từng file trong thư mục `src/`:

| Tên File | Vai Trò (Module Role) |
| :--- | :--- |
| `main.c` | **Entry Point & Event Loop:** Xử lý tham số dòng lệnh (CLI), khởi tạo hệ thống và giữ vòng lặp sự kiện chính. |
| `c270_usb.c` | **USB Interface:** Tầng giao tiếp thấp nhất. Gọi `libusb` để tìm thiết bị, mở kết nối, claim interface và cấu hình Alt Setting. |
| `c270_uvc.c` | **UVC Protocol:** Gửi các lệnh Probe/Commit theo chuẩn USB Video Class để ép camera chạy định dạng MJPEG, và điều khiển phần cứng (Độ sáng, Phơi sáng). |
| `c270_capture.c` | **Capture & Decode:** Gom các gói tin USB Isochronous rời rạc ghép thành một khung hình hoàn chỉnh, sau đó giải mã MJPEG sang mảng pixel RGB24. |
| `c270_stream.c` | **RTSP Server:** Nhận ảnh RGB24, dùng pipeline GStreamer để nén sang H.264/H.265 và đóng gói phát luồng mạng (RTSP). |
| `c270_display.c` | **Local UI:** Sử dụng SDL2 để render video trực tiếp tại màn hình máy tính nhúng kèm các thông số Overlay (FPS hiện tại, Timestamp). |
| `c270_hotplug.c`| **Hotplug Watchdog:** Quản lý vòng đời phần cứng thông qua State Machine (Plug/Unplug/Wait), ra lệnh Reinit khi nhận diện lại thiết bị. |

---

## 4. Bảng Kết Quả Nghiệm Thu (Acceptance Testing Criteria)

Hệ thống đã vượt qua toàn bộ các Test Case (TC1 -> TC4) đặt ra ban đầu:

1. **Nhận diện thiết bị:** Unbind kernel driver mượt mà, tự động detect đúng model Logitech C270 qua thông số Vendor ID / Product ID.
2. **Khởi tạo & Phục hồi:** Hoàn tất chu trình từ USB Probe đến dựng xong RTSP Server dưới 3 giây. Phục hồi kết nối thành công khi mô phỏng tháo dỡ cáp.
3. **Chất lượng Stream:** 
   - Nén phần cứng MJPEG trực tiếp từ firmware.
   - Đầu ra RTSP đạt chuẩn nét 640x480 (khắc phục hoàn toàn điểm yếu chỉ chạy được 176x144 của format YUYV).
   - Tốc độ cố định 30 FPS, độ trễ qua mạng LAN (Latency) chỉ từ ~50ms đến 100ms.
4. **Hiệu năng:** Tối ưu hóa CPU nhờ GStreamer Hardware Pipeline. Bộ nhớ RAM được cấp phát tĩnh ở các luồng nóng (hot path), tuyệt đối không rò rỉ (memory leak).

---

## 5. Hướng Dẫn Biên Dịch & Chạy (Quick Start)

### 5.1. Cài đặt thư viện nền tảng (Dependencies)
```bash
sudo apt update
sudo apt install -y libusb-1.0-0-dev libsdl2-dev libjpeg-turbo8-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstrtspserver-1.0-dev \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly
```

### 5.2. Biên dịch mã nguồn (Build)
```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

### 5.3. Chạy thử nghiệm
```bash
# Bước 1: Bắt buộc ngắt quyền điều khiển của Linux kernel mặc định
sudo bash tools/unbind_c270.sh

# Bước 2: Chạy Driver ở quyền Root (để có quyền truy cập cổng USB)
sudo ./build/c270_app

# Bước 3: Kiểm tra luồng stream bằng ffplay (ở tab Terminal khác hoặc máy khác)
ffplay -fflags nobuffer -flags low_delay rtsp://127.0.0.1:8554/camera0
```

---
*Lưu ý: Tài liệu này được thiết kế và tối ưu hóa đặc biệt. Dành cho các AI Agent (như Manus AI) đọc để hiểu nhanh kiến trúc và tự động sinh ra các slide báo cáo thuyết trình kỹ thuật.*
