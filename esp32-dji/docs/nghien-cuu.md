# BQ30Z554-R1 trên pin DJI Mavic Pro: chẩn đoán và xoá PF bằng ESP32

## Phạm vi đã chốt

Firmware mục tiêu của dự án là **BQ30Z554-R1** tại địa chỉ SMBus 7-bit `0x0B`, dùng ESP32 DevKit với ESP-IDF 5.5.3. Ngoài các thao tác đọc chẩn đoán, firmware phải hỗ trợ đúng chuỗi thao tác sau:

```text
Ghi log → Unseal bằng SHA-1 challenge/response → Full Access
→ backup Data Flash → ghi/verify profile cell mới → Permanent Fail Data Reset
→ đọc lại trạng thái → Seal lại
```

ESP32 giữ SCL tối đa 12 ms cho mỗi phase của giao dịch. Giá trị này thay thế
default 2 ms của ESP-IDF, vì BQ30Z554 có thể clock-stretch khi nhận block
`ManufacturerInput` 20 byte trong SHA-1 authentication.

ESP32 đời đầu có thanh ghi I²C hardware chỉ đợi SCL-low tối đa khoảng 13 ms.
Vì BQ30 có thể kéo SCL lâu hơn khi nhận block SHA-1/Data Flash, firmware tự
tạm nhả hardware I²C và truyền riêng các block `ManufacturerInput` dài 20/32
byte bằng GPIO open-drain; mỗi cạnh SCL được đợi tối đa 500 ms, rồi hardware
I²C được khôi phục. Các lệnh SBS ngắn vẫn dùng driver ESP-IDF.

PEC là tuỳ chọn của SBS. Firmware có thể thêm/kiểm tra PEC (CRC-8,
polynomial `0x07`) khi build với `BMS_USE_PEC=1`; nhưng board DJI này được
quan sát giao tiếp ổn định với host PEC tắt, nên cấu hình mặc định là `0`.

Firmware retry probe kèm I2C bus recovery khi SMBus không ổn định. Pull-up nội
bộ ESP32 chỉ là chế độ chẩn đoán tùy chọn; pull-up ngoài phù hợp vẫn cần thiết
để giao tiếp bền vững.

Chuỗi này bám theo quy trình trong bài [Make a custom battery for DJI Mavic Pro](https://ludovic.cool/make-a-custom-battery-for-dji-mavic-pro/) nhưng thay Raspberry Pi bằng ESP32. Mã lệnh, định dạng SHA-1 và thời gian chờ được đối chiếu với [BQ30Z554-R1 Technical Reference Manual của TI](https://www.ti.com/lit/pdf/sluua79) và định nghĩa BQ30Z554 của [O-GS dji-firmware-tools](https://github.com/o-gs/dji-firmware-tools).

## Xác nhận IC

Bài tham khảo nhận diện IC của bo WM220 là BQ30Z55. BQ30Z554-R1 là biến thể BQ30 cùng họ, hỗ trợ 2–4 cell Li-ion/Li-polymer và giao tiếp SBS 1.1/SMBus. [Trang sản phẩm TI](https://www.ti.com/product/BQ30Z554-R1)

Với dự án này, cấu hình BQ30Z554-R1 đã được chốt theo yêu cầu. Tuy vậy, không thể chứng minh IC *trên chính bo pin vật lý* chỉ từ tài liệu hoặc từ giao thức: driver O-GS dùng chung định nghĩa cho BQ30z50, BQ30z55 và BQ30z554. Trước khi cho phép lệnh ghi, phải lưu bằng chứng sau vào log dự án:

1. Ảnh sắc nét marking trên IC và mặt bo, xác nhận đúng BQ30Z554-R1/BQ30Z55 tương thích.
2. Đọc `Device Type` bằng `ManufacturerAccess(0x0001)` rồi đọc block kết quả từ `ManufacturerData (0x23)`.
3. Đọc `Firmware Version` bằng `ManufacturerAccess(0x0002)` rồi đọc block từ `0x23`.
4. Xác nhận BMS ACK ổn định ở `0x0B` và các giá trị điện áp/nhiệt độ hợp lý trước khi chạy `Unseal`.

Nếu marking hoặc hai lệnh nhận dạng không khớp, dừng: không áp dụng các lệnh dưới đây cho IC/bo khác.

## Cảnh báo an toàn và điều kiện tiên quyết

Pin Mavic Pro là pack Li-Po HV 3S. Việc pin hiện khó hoặc không còn mua được không làm giảm rủi ro cháy, quá nhiệt hoặc hỏng drone.

- Không thao tác nếu cell phồng/rò rỉ, dây balance/NTC hỏng, có mùi lạ hoặc nhiệt độ bất thường.
- Tháo pin khỏi drone, bộ sạc và tải. Làm việc trên bề mặt không cháy; không để pack đang thử nghiệm không giám sát.
- Trước khi reset PF, phải xử lý nguyên nhân gốc: cell suy giảm/mất cân bằng, NTC, FET, cầu chì, đường đo cell hoặc bo AFE. TI nêu rõ PF có thể do điện áp, dòng, nhiệt độ, FET, thermistor, cầu chì, AFE hoặc Data Flash.
- Sao lưu toàn bộ log `PFStatus`, `SafetyStatus`, điện áp cell, nhiệt độ, điện áp pack và ảnh hiện trạng **trước** khi reset. Lệnh `0x0029` sẽ xoá dữ liệu PF, làm mất bằng chứng chẩn đoán đó.
- Chỉ transaction `commission` mới được phép sửa Data Flash; nó chỉ cập nhật các trường capacity, nominal voltage, charge voltage theo nhiệt độ và COV. Không ghi số chu kỳ, key bảo mật, calibration, ChemID hoặc điều khiển FET thủ công.
- Sau khi unseal, luôn chạy `Seal` trong `finally`/nhánh dọn dẹp, kể cả khi PF reset thất bại.

Không reset khi `PFStatus` cho thấy lỗi cầu chì, PTC, AFE/AFE communication, Instruction Flash checksum hoặc Data Flash write fail; các lỗi này không được coi là lỗi cell thông thường. Một số điều kiện, như PTC, yêu cầu chu kỳ cấp nguồn hoàn chỉnh; nếu điều kiện lỗi còn tồn tại thì cờ PF sẽ lập lại ngay sau khi reset. [Chi tiết PF của TI](https://www.ti.com/lit/pdf/sluua79)

## Kết nối ESP32 ↔ BMS

ESP32 có hai bộ điều khiển I²C, dùng được làm master SMBus ở mức giao thức. Chọn GPIO 21/22 làm mặc định; chúng có thể đổi được trong firmware.

```text
PC/USB ── ESP32 DevKit ── [cách ly I²C / chuyển mức nếu cần] ── BMS
             │ GPIO21  ───────── SDA / SMBD
             │ GPIO22  ───────── SCL / SMBC
             └ GND     ───────── GND logic (*)

(*) Chỉ nối mass trực tiếp khi đã xác minh mức logic. Nếu không, dùng cách ly I²C hai chiều.
```

| Tín hiệu BMS | ESP32 | Quy tắc |
| --- | --- | --- |
| `SMBD` / `SDA` | GPIO 21 | Qua chuyển mức/cách ly nếu cần. |
| `SMBC` / `SCL` | GPIO 22 | Qua chuyển mức/cách ly nếu cần. |
| `GND` logic | GND | Chỉ khi đã xác nhận chung mass và mức điện áp. |
| `BAT+`, `BAT-`, tap cell, NTC | Không nối GPIO | Không phải bus SMBus. |
| `3V3` ESP32 | Không nối mặc định | Không cấp nguồn ngược vào BMS/pack. |

Đo mức high của SDA/SCL so với GND logic trước khi nối ESP32. Không tự thêm pull-up khi chưa biết bo BMS kéo lên bao nhiêu volt. Pull-up nội của ESP32 phải tắt; dùng pull-up ngoài đúng mức logic hoặc mạch cách ly/chuyển mức. Tài liệu API I²C: [Espressif I²C master](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/i2c.html).

## Cấu hình PlatformIO / ESP-IDF

`platformio.ini` đã dùng `board = esp32dev` và `framework = espidf`. Build hiện tại là ESP-IDF 5.5.3. Sửa `src/CMakeLists.txt` khi bắt đầu triển khai firmware:

```cmake
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(
    SRCS ${app_sources}
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_uart mbedtls
)
```

Không cần thư viện PlatformIO ngoài. Sử dụng driver I²C mới `driver/i2c_master.h`, không dùng API I²C legacy.

## Các command BQ30Z554 cần dùng

Tất cả word truyền SMBus có byte thấp trước. Ví dụ ghi subcommand `0x0029` vào `ManufacturerAccess()` là ba byte I²C: `00 29 00`.

| Mục đích | Command / subcommand | Hành vi |
| --- | --- | --- |
| Manufacturer Access | `0x00` | Ghi một word subcommand theo little-endian. |
| Manufacturer Data | `0x23` | Đọc block kết quả sau nhiều ManufacturerAccess command. |
| Manufacturer Input | `0x2F` | Đọc challenge và ghi SHA-1 digest khi unseal. |
| PF status | `MA 0x0053`, sau đó đọc `0x23` | Chụp lý do PF trước/sau thao tác. |
| Operation status | `MA 0x0054`, sau đó đọc `0x23` | Kiểm tra state bảo mật `SEC1/SEC0`. |
| Unseal | `MA 0x0031` | Bắt đầu challenge/response SHA-1. |
| PF data reset | `MA 0x0029` | Xoá PF data/flag. Đây là lệnh bài tham khảo gọi `PermanentFailDataReset`. |
| Seal | `MA 0x0030` | Khoá lại các command cần bảo vệ. |

`0x0029` chỉ xoá dữ liệu PF; nó không sửa cell, NTC, cầu chì hay FET. TI mô tả chính xác lệnh này là "Permanent Fail Data Reset" dùng để clear PF data. [TI, mục 10.1.22](https://www.ti.com/lit/pdf/sluua79)

## Unseal đúng kiểu BQ30Z554-R1

Không dùng chuỗi hai word `0x0414`/`0x3672` cho BQ30Z554-R1. Cơ chế BQ30Z554-R1 là SHA-1 challenge/response:

1. Gửi `MA 0x0031`: bytes `00 31 00`.
2. Đọc SMBus block ở `ManufacturerInput (0x2F)`: byte đầu là length `0x14`, theo sau là challenge 20 byte. TI định nghĩa byte đầu của message là LSB.
3. Từ unseal key 128-bit `KD`, tạo `HMAC1 = SHA1(KD || reverse(challenge_wire))`.
4. Tạo `HMAC2 = SHA1(KD || HMAC1)`, đảo thứ tự 20 byte của HMAC2 theo thứ tự wire.
5. Ghi SMBus block vào `ManufacturerInput`: `2F 14 <20 byte HMAC2_wire>`.
6. Chờ ít nhất 250 ms, sau đó đọc `OperationStatus` và xác nhận `SEC1/SEC0 = 0b01` (Unsealed).

Đây là đúng trình tự tám bước của TI; response SHA-1 không phải chuỗi command tuỳ ý. [TI, mục 9.5](https://www.ti.com/lit/pdf/sluua79)

Mã O-GS mà bài tham khảo gọi không truyền `--sha1key`, nên dùng default hiện tại `0123456789abcdeffedcba9876543210`. Đây chỉ là **candidate key** đã công khai trong mã O-GS, không phải cam kết rằng mọi pack DJI có cùng key. Nếu xác thực không thành công, không thử key ngẫu nhiên hoặc ghi `Unseal Key (0x0035)`; giữ BMS sealed và dừng. [O-GS sealing defaults](https://github.com/o-gs/dji-firmware-tools/blob/master/comm_sbs_bqctrl.py)

## Khung code ESP-IDF

Đoạn mã dưới đây bao gồm các primitive đọc/ghi cần thiết, tính digest theo đúng thứ tự byte của TI và ba thao tác `unseal` / `clear PF` / `seal`. `BMS_UNSEAL_KEY` phải được đặt ngoài source public (NVS mã hoá hoặc build secret); giá trị candidate phía trên chỉ được dùng sau khi đã xác minh đúng bo.

```c
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha1.h"

#define BMS_SDA_GPIO          GPIO_NUM_21
#define BMS_SCL_GPIO          GPIO_NUM_22
#define BMS_ADDRESS           0x0B
#define BMS_CLOCK_HZ          50000
#define BMS_TIMEOUT_MS        100
#define BMS_MA                0x00
#define BMS_MFR_DATA          0x23
#define BMS_MFR_INPUT         0x2F
#define BMS_UNSEAL            0x0031
#define BMS_PF_DATA_RESET     0x0029
#define BMS_SEAL              0x0030

static const char *TAG = "bms";
static i2c_master_dev_handle_t bms_dev;

static esp_err_t bms_i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BMS_SDA_GPIO,
        .scl_io_num = BMS_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "I2C init");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BMS_ADDRESS,
        .scl_speed_hz = BMS_CLOCK_HZ,
    };
    return i2c_master_bus_add_device(bus, &dev_cfg, &bms_dev);
}

static esp_err_t bms_write_word(uint8_t command, uint16_t word)
{
    const uint8_t tx[] = {command, (uint8_t)word, (uint8_t)(word >> 8)};
    return i2c_master_transmit(bms_dev, tx, sizeof(tx), BMS_TIMEOUT_MS);
}

/* SMBus block read with known payload length, excluding the length byte. */
static esp_err_t bms_read_block_exact(uint8_t command, uint8_t *payload,
                                      size_t expected_length)
{
    uint8_t raw[33]; // SMBus block: one length byte plus maximum 32 bytes
    if (expected_length > 32) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = i2c_master_transmit_receive(
        bms_dev, &command, 1, raw, expected_length + 1, BMS_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    if (raw[0] != expected_length) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    memcpy(payload, &raw[1], expected_length);
    return ESP_OK;
}

static esp_err_t bms_write_block(uint8_t command, const uint8_t *payload,
                                 size_t length)
{
    uint8_t tx[34]; // command + count + payload
    if (length > 32) {
        return ESP_ERR_INVALID_SIZE;
    }
    tx[0] = command;
    tx[1] = (uint8_t)length;
    memcpy(&tx[2], payload, length);
    return i2c_master_transmit(bms_dev, tx, length + 2, BMS_TIMEOUT_MS);
}

/* TI 9.5: B1 = KD || reverse(Mwire), B2 = KD || HMAC1; send reverse(HMAC2). */
static void bms_make_unseal_digest(const uint8_t kd[16],
                                   const uint8_t challenge_wire[20],
                                   uint8_t digest_wire[20])
{
    uint8_t input[36];
    uint8_t hmac1[20];
    uint8_t hmac2[20];

    memcpy(input, kd, 16);
    for (size_t i = 0; i < 20; ++i) {
        input[16 + i] = challenge_wire[19 - i];
    }
    mbedtls_sha1(input, sizeof(input), hmac1);

    memcpy(input, kd, 16);
    memcpy(&input[16], hmac1, sizeof(hmac1));
    mbedtls_sha1(input, sizeof(input), hmac2);

    for (size_t i = 0; i < 20; ++i) {
        digest_wire[i] = hmac2[19 - i];
    }
}

static esp_err_t bms_unseal(const uint8_t kd[16])
{
    uint8_t challenge[20];
    uint8_t digest[20];

    ESP_RETURN_ON_ERROR(bms_write_word(BMS_MA, BMS_UNSEAL), TAG, "start unseal");
    ESP_RETURN_ON_ERROR(bms_read_block_exact(BMS_MFR_INPUT, challenge, sizeof(challenge)),
                        TAG, "read challenge");
    bms_make_unseal_digest(kd, challenge, digest);
    ESP_RETURN_ON_ERROR(bms_write_block(BMS_MFR_INPUT, digest, sizeof(digest)),
                        TAG, "write response");
    vTaskDelay(pdMS_TO_TICKS(250));
    return ESP_OK;
}

static esp_err_t bms_clear_pf(void)
{
    return bms_write_word(BMS_MA, BMS_PF_DATA_RESET);
}

static esp_err_t bms_seal(void)
{
    return bms_write_word(BMS_MA, BMS_SEAL);
}
```

Khung trên giả định BMS không bật PEC, giống đường I²C trong bài tham khảo. Nếu logic analyzer cho thấy PEC, thêm byte PEC và kiểm tra CRC-8 SMBus cho cả block read/write trước khi bất kỳ lệnh ghi nào được cho phép.

## Quy trình thực thi bắt buộc

1. **Đọc trước khi ghi.** Đọc Device Type, Firmware Version, `PFStatus (0x0053)`, `SafetyStatus (0x0051)`, `OperationStatus (0x0054)`, `Voltages (0x0071)` và `Temperatures (0x0072)` qua `MA`/`ManufacturerData`. Lưu raw bytes, giá trị giải mã và timestamp.
2. **Đánh giá nguyên nhân.** Chỉ tiếp tục nếu cell, cảm biến nhiệt, đường balance, FET và cầu chì đã được kiểm tra độc lập; pack không phồng/nóng và không có PF thuộc nhóm cấm nêu trên.
3. **Unseal.** Gọi `bms_unseal(kd)`, rồi đọc lại `OperationStatus`. Chấp nhận khi state là Unsealed hoặc Full Access; nếu không đạt một trong hai state này sau 250 ms, không reset PF, gọi `bms_seal()` nếu state đã đổi và dừng.
4. **Xoá PF.** Gọi đúng một lần `bms_clear_pf()`, tức truyền `00 29 00`. Không lặp vô hạn khi lỗi I²C/NACK.
5. **Xác minh ngay.** Đọc lại `PFStatus`, `SafetyStatus`, `OperationStatus`, `Voltages`, `Temperatures` và log kết quả. Cờ PF có thể xuất hiện lại nếu lỗi thực chưa được khắc phục.
6. **Seal.** Gọi `bms_seal()` (`00 30 00`) ngay cả khi bước 4 hoặc 5 lỗi.
7. **Thử nghiệm năng lượng có giám sát.** Chỉ sau khi log sạch và bo đã sealed, kiểm tra bằng bộ sạc chính hãng/tải phù hợp, trong điều kiện giám sát. Không lắp vào drone để làm phép thử đầu tiên.

## Tiêu chí kết quả

| Kết quả | Diễn giải | Hành động |
| --- | --- | --- |
| Không ACK tại `0x0B` | Sai chân, sai mức logic, BMS chưa thức hoặc IC không đúng giả định | Kiểm tra hardware; không quét bừa toàn bus. |
| Challenge đọc được nhưng Unseal thất bại | Key hoặc IC/firmware không phù hợp | Seal nếu cần, dừng; không thử key ngẫu nhiên. |
| Unseal thành công nhưng PF reset lại ngay | Điều kiện PF còn tồn tại | Dừng, sửa lỗi phần cứng/cell trước. |
| PF sạch, điện áp/nhiệt độ hợp lý, BMS sealed | BMS đã qua kiểm tra giao thức | Chuyển sang thử sạc/xả có giám sát. |

## Sử dụng firmware ESP32

Firmware trong `src/main.c` chạy trên UART 115200 baud, tự tạo một full dump khi khởi động nếu BMS ACK tại `0x0B`. GPIO mặc định là SDA = GPIO21, SCL = GPIO22, tốc độ I²C = 50 kHz và pull-up nội bị tắt.

Nếu boot log báo `I2C idle levels: SDA=0` hoặc `SCL=0`, bus đang bị kéo thấp: dừng thao tác ghi và kiểm tra đúng chân SMBD/SMBC, GND logic, mạch chuyển mức/cách ly, pull-up ngoài và nguồn logic của BMS. Cảnh báo `GPIO ... maybe conflict with others` là reservation phần mềm; chọn hai GPIO chưa được dùng và đặt `BMS_SDA_GPIO_NUM`, `BMS_SCL_GPIO_NUM`, `BMS_I2C_PORT_NUM` trong `platformio.ini` nếu board của bạn dùng GPIO21/22 cho phần cứng khác. Pull-up nội chỉ là phép thử ngắn sau khi xác nhận bus an toàn ở 3,3 V.

| Lệnh UART | Chức năng |
| --- | --- |
| `help` | In danh sách lệnh. |
| `probe` | Kiểm tra ACK duy nhất tại `0x0B`; không quét mù bus. |
| `snapshot` | Log trạng thái cốt lõi SBS/BQ30. |
| `dump` | Log đầy đủ raw block, trạng thái PF, điện áp/nhiệt độ, lifetime và Impedance Track. |
| `watch on` / `watch off` | Bật/tắt snapshot mỗi 2 giây. |
| `profile show` | Đọc `BatteryMode` và năm Data Flash row dùng cho commissioning; chỉ đọc, in raw backup và giá trị giải mã. |
| `commission <mAh> <nominal-mV> <low-mV> <std-mV> <high-mV> <rec-mV> CONFIRM` | Transaction khép kín: unseal, Full Access, backup/ghi/verify profile, xoá CUDEP-only PF, seal và snapshot cuối. |
| `unseal CONFIRM` | Chạy SHA-1 challenge/response với candidate key cấu hình sẵn. |
| `pf-reset CONFIRM` | Chỉ reset `PFStatus = 0x00000004` (CUDEP-only) trong session đã unseal; firmware luôn gửi Seal sau đó. |
| `seal CONFIRM` | Gửi Seal ngay và kết thúc session ghi. |

### Commissioning pack cell mới

`commission` nhận capacity theo mAh, điện áp danh định của *cả pack* theo mV, rồi bốn điện áp sạc theo cell ở các dải nhiệt độ `low / standard / high / recommended`. Mọi giá trị được nhập tường minh thay vì hard-code một loại cell.

Ví dụ cú pháp (các số chỉ là vị trí tham số, không phải profile mặc định):

```text
commission <mAh> <nominal-mV> <low-mV> <std-mV> <high-mV> <rec-mV> CONFIRM
```

Firmware giới hạn `mAh` từ 100 đến 32767, nominal voltage từ 6000 đến 18000 mV và mỗi charge voltage từ 2500 đến 4350 mV. Nó tính tự động `DesignCapacity` ở hai dạng: mAh và `10 mWh` (`mAh × nominal-mV / 10000`, làm tròn gần nhất). Bit `CAPM` trong `BatteryMode()` được giữ nguyên; do đó `DesignCapacity()` sẽ report đúng theo mode của pack.

Theo Table 11-1 của TI và raw DF quan sát trên firmware WM220 này, transaction backup toàn bộ sáu row Data Flash `0x03`, `0x04`, `0x08`, `0x09`, `0x0F`, `0x10`; rồi chỉ sửa các trường sau và đọc lại từng row để verify:

- SBS Data: `DesignVoltage`, `DesignCapacity` mAh và `DesignCapacity` 10 mWh.
- Advanced Charging Algorithm: low, standard, high và recommended temperature charging voltage.
- Protections:COV: cả bốn threshold và recovery. Firmware giữ margin COV hiện có của pack theo từng dải nhiệt độ, fallback về ±50 mV nếu không đọc được margin hợp lệ.

Với firmware WM220 đã đo, SBS Data nằm tại base `502`, COV tại base `275`, và `DesignCapacity` 10 mWh nằm trong row `0x10`; vì vậy firmware luôn đọc/ghi cả row `0x0F` lẫn `0x10`. Giá trị số trong các row Data Flash này dùng big-endian; SMBus word vẫn truyền LSB trước. [TI Appendix B: Reading and Writing to Data Flash](https://www.ti.com/lit/pdf/sluua79)

Ghi Data Flash cần **Full Access** sau Unseal. Theo `OperationStatus()`, `SEC1/SEC0 = 1/0` là Full Access, `0/1` là Unsealed, `1/1` là Sealed và `0/0` là Reserved. Nếu SHA-1 key không mở được Full Access, firmware dừng trước bất kỳ lệnh ghi nào và Seal lại. Sau mỗi row write lỗi hoặc verify mismatch, firmware cố gửi lại backup của các row đã chạm rồi Seal. TI cũng lưu ý rằng DF write failure sẽ latch PF và cấm ghi tiếp. [TI: Data Flash Permanent Fail](https://www.ti.com/lit/pdf/sluua79)

Transaction chỉ tiếp tục khi `PFStatus` bằng `0` hoặc chỉ có `CUDEP` (`0x00000004`). Với CUDEP-only, firmware ghi profile trước, gửi `MA 0x0029`, yêu cầu `PFStatus=0` khi đọc lại, rồi Seal. Các cờ fuse, AFE, PTC, instruction flash, Open VCx và Data Flash failure vẫn bị từ chối.

Việc commission không thay ChemID, table Impedance Track hay QMax đã học từ cell cũ. Sau khi `PF=0` và BMS sealed, cần thực hiện chu kỳ học gauge phù hợp với chemistry mới để `FullChargeCapacity` và % pin hội tụ. [TI: điều kiện QMax update](https://www.ti.com/lit/pdf/sluua79)

Các lệnh ghi không tự chạy khi khởi động. Cả giá trị raw lẫn giải mã PF/OperationStatus đều được in UART để lưu bằng chứng trước và sau thao tác.

## Tài liệu đối chiếu

- [Bài hướng dẫn gốc của Ludovic](https://ludovic.cool/make-a-custom-battery-for-dji-mavic-pro/)
- [TI BQ30Z554-R1 Technical Reference Manual](https://www.ti.com/lit/pdf/sluua79)
- [TI BQ30Z554-R1 product page](https://www.ti.com/product/BQ30Z554-R1)
- [O-GS dji-firmware-tools: BQ30Z554 definitions](https://github.com/o-gs/dji-firmware-tools/blob/master/comm_sbs_chips/BQ30z554.py)
