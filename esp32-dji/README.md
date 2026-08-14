# ESP32 DJI BMS Firmware

Đây là project PlatformIO/ESP-IDF chứa firmware UART console cho ESP32.

Tài liệu bắt đầu ở [../README.md](../README.md). Phần phân tích kỹ thuật sâu hơn về BQ30Z554-R1, SMBus, SHA-1 unseal, Data Flash và commissioning nằm ở [docs/nghien-cuu.md](docs/nghien-cuu.md).

Lệnh build nhanh:

```powershell
pio run
pio run -t upload
pio device monitor -b 115200
```

Nếu chỉ mới kiểm tra phần cứng, bắt đầu bằng `probe`, `snapshot` và `dump`. Không chạy lệnh có `CONFIRM` trước khi đã đọc checklist an toàn.
