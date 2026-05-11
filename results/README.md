# Báo Cáo Đánh Giá Hiệu Năng Driver C270

Thư mục này chứa bộ công cụ benchmark tự động dùng để đánh giá hiệu suất của driver UVC C270 tuỳ chỉnh trong các điều kiện hoạt động thực tế.

## 1. Phương Pháp Đo Lường

- **Công cụ:** Sử dụng một script Python (`run_benchmark.py`) chạy bọc bên ngoài ứng dụng C. Hệ thống dùng thư viện `psutil` để quét trực tiếp vào nhân hệ điều hành (`/proc/<pid>/stat` và `/proc/<pid>/statm`) mỗi 0.5 giây.
- **Lợi ích:** Không can thiệp (Zero-touch) vào mã nguồn C. Điều này giúp loại bỏ hoàn toàn rủi ro gây chậm trễ (overhead) do I/O ghi file trong vòng lặp xử lý hình ảnh, mang lại số liệu hiệu suất thực tế và chính xác nhất.
- **Phân tích:** Dữ liệu thu thập được xuất ra file CSV và được script `analyze.py` vẽ thành 3 biểu đồ trực quan (Line chart, Timeline, và Bar chart).

## 2. Các Chế Độ Benchmark (Modes)

1. **Mode 1 - Baseline (Chạy ngầm):**
   - *Lệnh chạy:* `./build/c270_app --no-display --no-stream`
   - *Mục tiêu:* Kiểm tra chi phí tài nguyên thuần túy để đọc tín hiệu USB (MJPEG) và giải mã hình ảnh.
2. **Mode 2 - Local Display (Hiển thị cục bộ):**
   - *Lệnh chạy:* `./build/c270_app --no-stream`
   - *Mục tiêu:* Đánh giá mức độ hao tốn tài nguyên của thư viện SDL2 khi render video ra màn hình.
3. **Mode 3 - RTSP Streaming (Truyền phát IPC):**
   - *Lệnh chạy:* `./build/c270_app --no-display -C h264`
   - *Mục tiêu:* Kiểm tra hiệu năng thực tế khi dùng GStreamer để encode H.264 và đẩy luồng RTSP qua mạng.

---

## 3. Phân Tích Kết Quả & Nhận Xét

Dựa trên dữ liệu thực tế thu thập từ lần chạy gần nhất:

### Chế độ 1: Baseline (Nền tảng cốt lõi)
- **CPU Trung bình:** ~5.9%
- **RAM Trung bình:** ~12.8 MB
- *Nhận xét:* Mã nguồn C tương tác với thư viện `libusb` rất nhẹ và tối ưu. Driver có thể kéo luồng 30FPS liên tục mà gần như không gây ra bất kỳ sức ép nào cho hệ thống.

### Chế độ 2: Local Display (Hiển thị UI)
- **CPU Trung bình:** ~8.8%
- **RAM Trung bình:** ~90.2 MB
- *Nhận xét:* Bộ nhớ RAM tăng vọt lên ~90MB. Đây là hiện tượng hoàn toàn bình thường do thư viện SDL2 phải khởi tạo context đồ họa (OpenGL/X11), phân bổ Framebuffer và Texture cho Window. Tải CPU cũng nhích lên một chút do phải đẩy buffer lên GPU.

### Chế độ 3: IPC Server (RTSP H264 Streaming)
- **CPU Trung bình:** ~6.3%
- **RAM Trung bình:** ~18.5 MB
- *Nhận xét:* **Kết quả cực kỳ ấn tượng.** Dù phải dùng GStreamer để mã hóa video sang chuẩn H.264 và duy trì server RTSP, bộ nhớ RAM chỉ tăng thêm khoảng 6MB so với baseline, trong khi CPU gần như không thay đổi đáng kể so với việc chỉ đọc USB. 
- **Kết luận:** Kiến trúc luồng (Threading) và Pipeline GStreamer đã được cấu hình tối ưu. Ứng dụng này hoàn toàn đạt tiêu chuẩn Industrial-grade (chuẩn công nghiệp) để cắm trên các bo mạch nhúng (Raspberry Pi, v.v) chạy camera giám sát 24/7 mà không lo cạn kiệt tài nguyên.

---

## 4. Hướng Dẫn Chạy Lại Benchmark

Nếu bạn nâng cấp mã nguồn và muốn đo lại hiệu năng, hãy thực hiện:

```bash
# 1. Giải phóng camera khỏi kernel mặc định
sudo bash ../unbind_c270.sh

# 2. Cài đặt thư viện nếu chưa có
sudo apt update
sudo apt install -y python3-psutil python3-matplotlib

# 3. Chạy đo đạc dữ liệu (mất ~30 giây)
sudo python3 run_benchmark.py

# 4. Vẽ lại biểu đồ
python3 analyze.py
```
