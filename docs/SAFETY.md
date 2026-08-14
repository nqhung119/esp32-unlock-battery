# Safety Checklist

Dự án này làm việc với pack Li-Po HV 3S của DJI Mavic Pro. Một thao tác sai có thể làm cell nóng, cháy, hỏng BMS hoặc hỏng drone. Đừng chạy lệnh ghi nếu bạn chưa kiểm tra được nguyên nhân gốc của lỗi pin.

## Không tiếp tục nếu

- Cell phồng, rò rỉ, móp nặng, có mùi lạ hoặc nóng bất thường.
- Điện áp cell lệch lớn, có cell quá xả sâu hoặc không phục hồi ổn định.
- Dây balance, NTC, cầu chì, FET hoặc đường đo cell bị đứt/chập.
- SDA/SCL có mức high cao hơn 3.3 V mà bạn chưa dùng chuyển mức/cách ly.
- Bạn chưa lưu log `dump` trước thao tác ghi.
- Bạn định lắp pack vào drone để thử ngay sau khi reset PF.

## Trước khi nối ESP32

1. Tháo pin khỏi drone, bộ sạc và tải.
2. Làm việc trên bề mặt không cháy, có phương án xử lý pin nóng/cháy.
3. Đo `BAT+`, `BAT-`, từng cell/tap balance và GND logic.
4. Đo mức high của `SMBD/SDA` và `SMBC/SCL` so với GND logic.
5. Xác định có cần mạch chuyển mức hoặc cách ly I2C hai chiều hay không.
6. Không cấp 3.3 V từ ESP32 ngược vào BMS nếu chưa hiểu sơ đồ nguồn logic.

## Trước khi chạy lệnh ghi

1. Chạy `probe` và xác nhận chỉ làm việc với BMS ở `0x0B`.
2. Chạy `dump` và lưu log nguyên bản.
3. Kiểm tra `PFStatus`, `SafetyStatus`, `OperationStatus`, điện áp cell và nhiệt độ.
4. Chỉ tiếp tục nếu PF là `0` hoặc CUDEP-only khi dùng flow hiện tại.
5. Dừng nếu có lỗi fuse, AFE, PTC, instruction flash, open cell sense hoặc Data Flash write fail.
6. Xác nhận cell mới phù hợp chemistry, điện áp sạc và dòng tải của pack.

## Khi commission cell mới

- Nhập capacity và điện áp theo datasheet cell đang dùng, không copy số từ pack khác.
- `nominal-mV` là điện áp danh định của cả pack.
- `low-mV`, `std-mV`, `high-mV`, `rec-mV` là điện áp sạc theo từng cell cho các dải nhiệt.
- Sau khi ghi Data Flash, firmware sẽ verify row đã ghi và cố rollback nếu lỗi, nhưng rollback không thể sửa lỗi phần cứng.
- Commission không thay ChemID, QMax, calibration hoặc lịch sử học gauge. Sau khi sửa cần chu kỳ học/kiểm tra phù hợp.

## Sau khi thao tác

1. Xác nhận firmware đã gửi `seal` và `OperationStatus` trở lại state sealed.
2. Chạy `snapshot` hoặc `dump` lại.
3. Kiểm tra PF không lập lại ngay.
4. Sạc/xả thử bằng tải phù hợp, có giám sát, trong khu vực an toàn.
5. Chỉ lắp vào drone sau khi pack ổn định qua thử nghiệm ngoài drone.

Nếu có nghi ngờ, dừng ở bước đọc chẩn đoán. Với pin Li-Po, một lần dừng sớm thường rẻ hơn rất nhiều so với một lần thử liều.
