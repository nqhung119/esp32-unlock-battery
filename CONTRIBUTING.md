# Contributing

Cảm ơn bạn muốn đóng góp. Dự án này chạm vào pin Li-Po HV và BMS có khả năng điều khiển sạc/xả, nên ưu tiên số một là log rõ ràng, thay đổi nhỏ, và không khuyến khích thao tác nguy hiểm.

## Khi mở issue

Vui lòng cung cấp các thông tin có thể chia sẻ an toàn:

- Model pack hoặc board, ví dụ DJI Mavic Pro/WM220.
- Ảnh marking IC nếu có thể che thông tin cá nhân/serial không muốn công khai.
- ESP32 board, chân SDA/SCL, mạch chuyển mức/cách ly và pull-up đang dùng.
- Log UART của `probe`, `snapshot` hoặc `dump`.
- `Device Type`, `Firmware Version`, `OperationStatus`, `PFStatus`, `SafetyStatus`.
- Điện áp từng cell, điện áp pack, nhiệt độ/NTC đo độc lập.
- Bạn đã thay cell, reset PF, hay chỉ đọc chẩn đoán.

Không đăng unseal key riêng, dữ liệu cá nhân hoặc log bạn không muốn mất quyền kiểm soát. Nếu pack đang phồng, nóng, rò rỉ hoặc có mùi lạ, ưu tiên xử lý an toàn phần cứng thay vì debug firmware.

## Khi gửi pull request

- Giữ thay đổi nhỏ và tập trung vào một mục tiêu.
- Không thêm lệnh ghi tự động khi boot.
- Mọi lệnh ghi mới phải yêu cầu `CONFIRM`, log trước/sau và cố gắng `seal` ở nhánh dọn dẹp.
- Không reset PF rộng hơn CUDEP-only nếu chưa có phân tích an toàn rõ ràng.
- Không hard-code profile cell cụ thể làm mặc định chung.
- Ưu tiên code C rõ ràng, ít abstraction, theo style hiện có trong `src/main.c`.
- Cập nhật README hoặc docs nếu thay đổi command UART, wiring, build flags hoặc quy trình an toàn.

## Kiểm tra trước khi gửi

Chạy tối thiểu:

```powershell
cd esp32-dji
pio run
```

Nếu thay đổi SMBus hoặc Data Flash, hãy ghi rõ bạn đã test trên phần cứng thật hay chỉ build được firmware. Log logic analyzer rất hữu ích khi sửa timing, PEC, clock stretching hoặc block transaction.

## Tinh thần dự án

Mục tiêu là giúp cộng đồng giữ lại những chiếc Mavic Pro cũ bằng cách sửa chữa có trách nhiệm. Dự án không nhằm bỏ qua cơ chế an toàn, fake trạng thái pin, hoặc khuyến khích dùng cell không phù hợp.
