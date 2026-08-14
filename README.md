# ESP32 DJI Mavic Pro Battery BMS Unlock

Firmware ESP32/ESP-IDF để đọc chẩn đoán, unseal và commission lại BMS pin DJI Mavic Pro dùng họ TI BQ30Z554/BQ30Z55 trên board WM220.

DJI Mavic Pro đã ra đời từ lâu, pin chính hãng mới gần như không còn dễ mua. Dự án này được viết để giúp người còn giữ máy có thể nghiên cứu, sửa chữa hoặc dựng lại pack cell một cách có kiểm soát, thay vì bỏ cả drone chỉ vì BMS bị khóa sau khi thay cell.

> Đây không phải công cụ bỏ qua an toàn pin. Hãy chỉ dùng khi bạn hiểu rủi ro Li-Po HV, đã kiểm tra cell/phần cứng độc lập và có thể giám sát pack trong lúc thử nghiệm.

## Tính năng

- ESP32 DevKit làm SMBus/I2C master, địa chỉ BMS mặc định `0x0B`.
- UART console 115200 baud với các lệnh `probe`, `snapshot`, `dump`, `watch`, `profile show`, `unseal`, `pf-reset`, `commission`, `seal`.
- Unseal bằng SHA-1 challenge/response cho BQ30Z554-R1, dùng candidate key công khai từ O-GS.
- Commission profile cell mới: cập nhật capacity, nominal voltage, charge voltage theo dải nhiệt, COV threshold/recovery rồi verify Data Flash.
- Chỉ cho reset Permanent Fail khi `PFStatus` là `0` hoặc CUDEP-only (`0x00000004`).
- Luôn cố gắng seal lại sau thao tác ghi.
- Có fallback software open-drain I2C cho các block `ManufacturerInput` dài khi BQ30 clock-stretch lâu hơn giới hạn hardware I2C của ESP32.

## Cảnh báo nhanh

Pin Mavic Pro là pack Li-Po HV 3S. Cell hỏng, mất cân bằng, phồng, rò rỉ, NTC lỗi, FET lỗi hoặc cầu chì lỗi có thể gây cháy, làm hỏng drone hoặc gây chấn thương.

Trước khi chạy bất kỳ lệnh ghi nào:

- Tháo pack khỏi drone, sạc và tải.
- Đo cell, dây balance, NTC, BAT+/BAT-, mass logic và mức high của SDA/SCL.
- Không nối trực tiếp GPIO ESP32 vào bus có mức cao hơn 3.3 V.
- Lưu log `dump` trước khi reset PF, vì lệnh reset sẽ xóa bằng chứng chẩn đoán.
- Không lắp pack vào drone để làm phép thử đầu tiên sau khi sửa.

Đọc checklist đầy đủ tại [docs/SAFETY.md](docs/SAFETY.md).

## Phần cứng cần có

- ESP32 DevKit hoặc board ESP32 tương thích PlatformIO `esp32dev`.
- Cáp USB dữ liệu.
- Đồng hồ đo điện, nguồn/bộ sạc/tải phù hợp và khu vực thử nghiệm chống cháy.
- Mạch chuyển mức hoặc cách ly I2C hai chiều nếu bus BMS không an toàn ở 3.3 V.
- Logic analyzer được khuyến nghị mạnh khi debug SMBus.

## Đấu nối mặc định

Firmware mặc định dùng SDA `GPIO21`, SCL `GPIO22`, I2C port `0`, tốc độ `50 kHz`, tắt pull-up nội.

| BMS | ESP32 | Ghi chú |
| --- | --- | --- |
| `SMBD` / `SDA` | `GPIO21` | Qua chuyển mức/cách ly nếu cần. |
| `SMBC` / `SCL` | `GPIO22` | Qua chuyển mức/cách ly nếu cần. |
| `GND` logic | `GND` | Chỉ nối khi đã xác minh mức logic và mass tham chiếu. |
| `BAT+`, `BAT-`, tap cell, NTC | Không nối GPIO | Đây không phải đường SMBus. |
| `3V3` ESP32 | Không nối mặc định | Không cấp nguồn ngược vào BMS/pack. |

Nếu muốn đổi chân, sửa `build_flags` trong [esp32-dji/platformio.ini](esp32-dji/platformio.ini):

```ini
build_flags =
  -D BMS_SDA_GPIO_NUM=16
  -D BMS_SCL_GPIO_NUM=17
  -D BMS_I2C_PORT_NUM=1
  -D BMS_SCL_WAIT_US=12000
  -D BMS_CLOCK_HZ=50000
```

Chỉ bật `BMS_ENABLE_INTERNAL_PULLUPS=1` cho chẩn đoán ngắn sau khi chắc chắn bus không vượt 3.3 V.

## Build và upload

Cần cài PlatformIO Core hoặc extension PlatformIO trong VS Code.

```powershell
cd esp32-dji
pio run
pio run -t upload
pio device monitor -b 115200
```

Firmware dùng framework `espidf`, không cần thư viện PlatformIO ngoài. Source chính nằm ở [esp32-dji/src/main.c](esp32-dji/src/main.c).

## Lệnh UART

Sau khi mở monitor, gõ `help` để xem danh sách lệnh. Các lệnh có khả năng ghi đều bắt buộc có chữ `CONFIRM`.

| Lệnh | Chức năng |
| --- | --- |
| `probe` | Kiểm tra BMS ACK tại `0x0B`. |
| `snapshot` | Log nhanh SBS/BQ30 status, điện áp, dòng, SOC. |
| `dump` | Log đầy đủ hơn: PF, Safety, Operation, Charging, Gauging, Lifetime, voltages, temperatures. |
| `watch on` / `watch off` | Bật/tắt snapshot mỗi 2 giây. |
| `profile show` | Đọc các Data Flash row liên quan đến commissioning, chỉ đọc. |
| `unseal CONFIRM` | Chạy SHA-1 unseal bằng candidate key trong firmware. |
| `pf-reset CONFIRM` | Reset CUDEP-only PF trong session đã unseal, sau đó seal. |
| `commission <mAh> <nominal-mV> <low-mV> <std-mV> <high-mV> <rec-mV> CONFIRM` | Transaction đầy đủ: unseal, Full Access, backup/ghi/verify profile, clear CUDEP-only PF, seal. |
| `seal CONFIRM` | Gửi lệnh Seal và kết thúc session ghi. |

Ví dụ cú pháp commissioning:

```text
commission <mAh> <nominal-mV> <low-mV> <std-mV> <high-mV> <rec-mV> CONFIRM
```

Không có profile cell mặc định trong README. Hãy nhập thông số theo cell bạn đang dùng và chỉ sau khi đã xác minh bằng datasheet cell.

## Quy trình khuyến nghị

1. Kết nối bus qua chuyển mức/cách ly phù hợp.
2. Mở monitor, chạy `probe`.
3. Chạy `dump` và lưu toàn bộ log trước khi ghi.
4. Kiểm tra cell, NTC, FET, cầu chì và nguyên nhân PF ngoài firmware.
5. Chạy `profile show` để backup các row liên quan.
6. Nếu thay cell mới, chạy `commission ... CONFIRM`.
7. Nếu chỉ cần clear CUDEP-only trong session đã unseal, chạy `pf-reset CONFIRM`.
8. Chạy `snapshot`/`dump` lại, xác nhận BMS đã `SEALED`, PF sạch và giá trị điện áp/nhiệt độ hợp lý.
9. Thử sạc/xả bằng tải phù hợp trong điều kiện giám sát trước khi gắn vào drone.

## Giới hạn đã biết

- Mục tiêu hiện tại là BQ30Z554-R1/BQ30Z55 tương thích trên pack DJI Mavic Pro/WM220.
- Candidate unseal key công khai không đảm bảo đúng với mọi firmware/board.
- `BMS_USE_PEC` mặc định tắt vì board quan sát được giao tiếp ổn định không cần host PEC.
- Firmware không thay ChemID, calibration, key bảo mật, cycle count hoặc bảng Impedance Track/QMax.
- Dự án không thay thế kiểm tra điện, kiểm tra cell và quy trình an toàn pin.

Chi tiết nghiên cứu sâu nằm ở [esp32-dji/docs/nghien-cuu.md](esp32-dji/docs/nghien-cuu.md).

## Cấu trúc repo

```text
.
├── README.md
├── CONTRIBUTING.md
├── docs/
│   └── SAFETY.md
└── esp32-dji/
    ├── platformio.ini
    ├── src/main.c
    └── docs/nghien-cuu.md
```

## Đóng góp

Log thực tế từ các pack khác nhau rất có giá trị, nhất là ảnh marking IC, `Device Type`, `Firmware Version`, PF flags, wiring và kết quả logic analyzer. Vui lòng đọc [CONTRIBUTING.md](CONTRIBUTING.md) trước khi gửi issue hoặc pull request.

## License

Repo hiện chưa khai báo license. Trước khi public rộng rãi, chủ repo nên chọn license rõ ràng, ví dụ MIT/Apache-2.0/GPL hoặc một license khác phù hợp với mục tiêu chia sẻ.
