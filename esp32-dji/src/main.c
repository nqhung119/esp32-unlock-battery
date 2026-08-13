#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha1.h"

/*
 * ESP32 SMBus diagnostic tool for BQ30Z554-R1 / WM220 battery boards.
 *
 * GPIO pins and bus speed must be confirmed against the actual board before
 * connection. All write commands require an explicit UART confirmation.
 */

#ifndef BMS_SDA_GPIO_NUM
#define BMS_SDA_GPIO_NUM         21
#endif

#ifndef BMS_SCL_GPIO_NUM
#define BMS_SCL_GPIO_NUM         22
#endif

#ifndef BMS_I2C_PORT_NUM
#define BMS_I2C_PORT_NUM         0
#endif

#ifndef BMS_ENABLE_INTERNAL_PULLUPS
#define BMS_ENABLE_INTERNAL_PULLUPS 0
#endif

/*
 * BQ30Z554 may stretch SCL while it processes a ManufacturerInput block.
 * ESP-IDF defaults this per-clock timeout to 2 ms on the classic ESP32,
 * which is too short for the 20-byte SHA-1 response transaction.
 */
#ifndef BMS_SCL_WAIT_US
#define BMS_SCL_WAIT_US          12000
#endif

#define BMS_SDA_GPIO             ((gpio_num_t)BMS_SDA_GPIO_NUM)
#define BMS_SCL_GPIO             ((gpio_num_t)BMS_SCL_GPIO_NUM)
#define BMS_ADDRESS              0x0B
#define BMS_CLOCK_HZ             50000
#define BMS_TIMEOUT_MS           500
#define BMS_MFG_SELECT_WAIT_MS   10
#define BMS_UNSEAL_WAIT_MS       250
#define BMS_MONITOR_PERIOD_MS    2000
#define BMS_DF_WRITE_WAIT_MS     100
#define BMS_DF_ROW_SIZE          32

#define SBS_MANUFACTURER_ACCESS  0x00
#define SBS_MANUFACTURER_DATA    0x23
#define SBS_MANUFACTURER_INPUT   0x2F

#define SBS_BATTERY_MODE          0x03
#define SBS_TEMPERATURE          0x08
#define SBS_VOLTAGE              0x09
#define SBS_CURRENT              0x0A
#define SBS_AVERAGE_CURRENT      0x0B
#define SBS_RELATIVE_SOC         0x0D
#define SBS_REMAINING_CAPACITY   0x0F
#define SBS_FULL_CHARGE_CAPACITY 0x10
#define SBS_BATTERY_STATUS       0x16
#define SBS_CYCLE_COUNT          0x17
#define SBS_DESIGN_CAPACITY      0x18
#define SBS_DESIGN_VOLTAGE       0x19

#define MA_DEVICE_TYPE           0x0001
#define MA_FIRMWARE_VERSION      0x0002
#define MA_SAFETY_ALERT          0x0050
#define MA_SAFETY_STATUS         0x0051
#define MA_PF_ALERT              0x0052
#define MA_PF_STATUS             0x0053
#define MA_OPERATION_STATUS      0x0054
#define MA_CHARGING_STATUS       0x0055
#define MA_GAUGING_STATUS        0x0056
#define MA_MANUFACTURING_STATUS  0x0057
#define MA_LIFETIME_DATA_1       0x0060
#define MA_LIFETIME_DATA_2       0x0061
#define MA_LIFETIME_DATA_3       0x0062
#define MA_MANUFACTURER_INFO     0x0070
#define MA_VOLTAGES              0x0071
#define MA_TEMPERATURES          0x0072
#define MA_IT_STATUS_1           0x0073
#define MA_IT_STATUS_2           0x0074
#define MA_PF_DATA_RESET         0x0029
#define MA_SEAL                  0x0030
#define MA_UNSEAL                0x0031
#define MA_FULL_ACCESS           0x0032

#define PF_CUDEP_MASK             BIT(2)
#define PF_HARD_BLOCKED_MASK      (BIT(18) | BIT(19) | BIT(20) | BIT(21) | \
                                  BIT(22) | BIT(23) | BIT(24) | BIT(25) | BIT(26))

/* TI SLUUA79 Table 11-1: physical DF address is subclass ID + offset. */
#define DF_SBS_DATA_SUBCLASS                  489
#define DF_SBS_INITIAL_BATTERY_MODE_OFFSET      6
#define DF_SBS_DESIGN_VOLTAGE_OFFSET             8
#define DF_SBS_DESIGN_CAPACITY_MAH_OFFSET       20
#define DF_SBS_DESIGN_CAPACITY_CWH_OFFSET       22

#define DF_LOW_TEMP_CHARGING_SUBCLASS          116
#define DF_STANDARD_TEMP_CHARGING_SUBCLASS     124
#define DF_HIGH_TEMP_CHARGING_SUBCLASS         132
#define DF_REC_TEMP_CHARGING_SUBCLASS          140
#define DF_CHARGING_VOLTAGE_OFFSET               0

#define DF_COV_SUBCLASS                         267
#define DF_COV_THRESHOLD_LOW_OFFSET               0
#define DF_COV_THRESHOLD_STANDARD_OFFSET          2
#define DF_COV_THRESHOLD_HIGH_OFFSET              4
#define DF_COV_THRESHOLD_REC_OFFSET               6
#define DF_COV_RECOVERY_LOW_OFFSET                 9
#define DF_COV_RECOVERY_STANDARD_OFFSET           11
#define DF_COV_RECOVERY_HIGH_OFFSET               13
#define DF_COV_RECOVERY_REC_OFFSET                15

#define BMS_COV_MARGIN_MV                         50

static const char *TAG = "bms";

/* Default candidate used by O-GS for the BQ30 SHA-1 authentication flow. */
static const uint8_t k_ogs_default_unseal_key[16] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10,
};

static i2c_master_bus_handle_t g_i2c_bus;
static i2c_master_dev_handle_t g_bms;
static SemaphoreHandle_t g_bms_lock;
static bool g_bms_ready;
static bool g_unsealed_session;
static bool g_full_access_session;
static bool g_watch_enabled;

static uint16_t le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void log_error(const char *operation, esp_err_t err)
{
    ESP_LOGW(TAG, "%s failed: %s (0x%x)", operation, esp_err_to_name(err), err);
}

static void log_hex(const char *label, const uint8_t *data, size_t length)
{
    printf("[BMS] %s (%u):", label, (unsigned)length);
    for (size_t i = 0; i < length; ++i) {
        printf(" %02X", data[i]);
    }
    putchar('\n');
}

static void log_ascii(const char *label, const uint8_t *data, size_t length)
{
    printf("[BMS] %s ASCII: ", label);
    for (size_t i = 0; i < length; ++i) {
        const uint8_t value = data[i];
        putchar((value >= 32 && value <= 126) ? (char)value : '.');
    }
    putchar('\n');
}

static esp_err_t bms_i2c_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BMS_I2C_PORT_NUM,
        .sda_io_num = BMS_SDA_GPIO,
        .scl_io_num = BMS_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = BMS_ENABLE_INTERNAL_PULLUPS,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &g_i2c_bus), TAG, "create I2C bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BMS_ADDRESS,
        .scl_speed_hz = BMS_CLOCK_HZ,
        .scl_wait_us = BMS_SCL_WAIT_US,
        .flags.disable_ack_check = false,
    };
    return i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &g_bms);
}

static void bms_log_line_levels(void)
{
    const int sda = gpio_get_level(BMS_SDA_GPIO);
    const int scl = gpio_get_level(BMS_SCL_GPIO);

    ESP_LOGI(TAG, "I2C idle levels: SDA=%d SCL=%d", sda, scl);
    if (sda != 1 || scl != 1) {
        ESP_LOGE(TAG, "I2C bus is not idle-high; check wiring, pull-ups and BMS logic power");
    }
}

static esp_err_t bms_probe(void)
{
    return i2c_master_probe(g_i2c_bus, BMS_ADDRESS, BMS_TIMEOUT_MS);
}

static esp_err_t bms_read_word(uint8_t command, uint16_t *value)
{
    uint8_t rx[2];
    esp_err_t err = i2c_master_transmit_receive(
        g_bms, &command, 1, rx, sizeof(rx), BMS_TIMEOUT_MS);
    if (err == ESP_OK) {
        *value = le16(rx);
    }
    return err;
}

static esp_err_t bms_write_word(uint8_t command, uint16_t value)
{
    const uint8_t tx[] = {command, (uint8_t)value, (uint8_t)(value >> 8)};
    return i2c_master_transmit(g_bms, tx, sizeof(tx), BMS_TIMEOUT_MS);
}

/* Reads a SMBus block whose payload size is known from the BQ30Z554 command. */
static esp_err_t bms_read_block_exact(uint8_t command, uint8_t *payload,
                                      size_t expected_length)
{
    uint8_t rx[33];
    if (expected_length > 32) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = i2c_master_transmit_receive(
        g_bms, &command, 1, rx, expected_length + 1, BMS_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    if (rx[0] != expected_length) {
        ESP_LOGW(TAG, "SMBus block 0x%02X length %u, expected %u", command,
                 rx[0], (unsigned)expected_length);
        return ESP_ERR_INVALID_RESPONSE;
    }

    memcpy(payload, &rx[1], expected_length);
    return ESP_OK;
}

static esp_err_t bms_write_block(uint8_t command, const uint8_t *payload,
                                 size_t length)
{
    uint8_t tx[34];
    if (length > 32) {
        return ESP_ERR_INVALID_SIZE;
    }

    tx[0] = command;
    tx[1] = (uint8_t)length;
    memcpy(&tx[2], payload, length);
    return i2c_master_transmit(g_bms, tx, length + 2, BMS_TIMEOUT_MS);
}

static esp_err_t bms_read_mfg(uint16_t subcommand, uint8_t *payload,
                              size_t expected_length)
{
    ESP_RETURN_ON_ERROR(bms_write_word(SBS_MANUFACTURER_ACCESS, subcommand), TAG,
                        "select MA 0x%04X", subcommand);
    vTaskDelay(pdMS_TO_TICKS(BMS_MFG_SELECT_WAIT_MS));
    return bms_read_block_exact(SBS_MANUFACTURER_DATA, payload, expected_length);
}

static esp_err_t bms_read_mfg_u32(uint16_t subcommand, uint32_t *value)
{
    uint8_t payload[4];
    esp_err_t err = bms_read_mfg(subcommand, payload, sizeof(payload));
    if (err == ESP_OK) {
        *value = le32(payload);
    }
    return err;
}

static bool bms_security_is_unsealed(uint32_t operation_status)
{
    const uint8_t sec0 = (operation_status >> 8) & 1U;
    const uint8_t sec1 = (operation_status >> 9) & 1U;
    return sec1 == 0 && sec0 == 1;
}

static bool bms_security_is_full_access(uint32_t operation_status)
{
    const uint8_t sec0 = (operation_status >> 8) & 1U;
    const uint8_t sec1 = (operation_status >> 9) & 1U;
    return sec1 == 0 && sec0 == 0;
}

static const char *bms_security_state_name(uint32_t operation_status)
{
    const uint8_t sec0 = (operation_status >> 8) & 1U;
    const uint8_t sec1 = (operation_status >> 9) & 1U;

    if (bms_security_is_full_access(operation_status)) {
        return "FULL-ACCESS";
    }
    if (bms_security_is_unsealed(operation_status)) {
        return "UNSEALED";
    }
    return (sec1 == 1 && sec0 == 1) ? "SEALED" : "UNKNOWN";
}

static void bms_log_operation_status(uint32_t value)
{
    const uint8_t sec0 = (value >> 8) & 1U;
    const uint8_t sec1 = (value >> 9) & 1U;
    const char *security = bms_security_state_name(value);

    ESP_LOGI(TAG,
             "OperationStatus=0x%08" PRIX32 " SEC1=%u SEC0=%u (%s), PF=%u, DSG=%u, CHG=%u, XDSG=%u, XCHG=%u",
             value, sec1, sec0, security, (unsigned)((value >> 12) & 1U),
             (unsigned)((value >> 1) & 1U), (unsigned)((value >> 2) & 1U),
             (unsigned)((value >> 13) & 1U), (unsigned)((value >> 14) & 1U));
}

static void bms_log_pf_status(uint32_t value)
{
    static const char *const names[27] = {
        "CUV(cell undervoltage)", "COV(cell overvoltage)", "CUDEP(copper deposition)",
        "reserved3", "OTCE(cell overtemperature)", "reserved5", "OTF(FET overtemperature)",
        "QIM(QMAX imbalance)", "CB(cell balancing)", "IMP(cell impedance)",
        "CD(capacity deterioration)", "VIMR(voltage imbalance rest)",
        "VIMA(voltage imbalance active)", "reserved13", "reserved14", "reserved15",
        "CFETF(charge FET)", "DFET(discharge FET)", "THERM(thermistor)",
        "FUSE", "AFER(AFE register)", "AFEC(AFE communication)",
        "2LVL(second-level fuse)", "PTC", "IFC(instruction flash checksum)",
        "OCECO(open cell connection)", "DFW(data flash write failure)",
    };

    ESP_LOGI(TAG, "PFStatus=0x%08" PRIX32, value);
    if (value == 0) {
        ESP_LOGI(TAG, "PFStatus: no active flags");
        return;
    }

    for (size_t bit = 0; bit < 27; ++bit) {
        if ((value & BIT(bit)) != 0) {
            ESP_LOGW(TAG, "PF bit %u active: %s", (unsigned)bit, names[bit]);
        }
    }
    if ((value & PF_HARD_BLOCKED_MASK) != 0) {
        ESP_LOGE(TAG, "PF reset is blocked: hardware/firmware integrity flag is active");
    } else if (value == PF_CUDEP_MASK) {
        ESP_LOGW(TAG, "PF contains CUDEP only; the commissioning transaction may clear this historical flag");
    } else {
        ESP_LOGE(TAG, "PF reset is blocked: flag set is not eligible for the CUDEP-only commissioning flow");
    }
}

static void bms_log_standard_words(void)
{
    typedef struct {
        const char *name;
        uint8_t command;
    } word_command_t;

    static const word_command_t commands[] = {
        {"BatteryMode", SBS_BATTERY_MODE},
        {"Temperature", SBS_TEMPERATURE},
        {"Voltage", SBS_VOLTAGE},
        {"Current", SBS_CURRENT},
        {"AverageCurrent", SBS_AVERAGE_CURRENT},
        {"RelativeStateOfCharge", SBS_RELATIVE_SOC},
        {"RemainingCapacity", SBS_REMAINING_CAPACITY},
        {"FullChargeCapacity", SBS_FULL_CHARGE_CAPACITY},
        {"BatteryStatus", SBS_BATTERY_STATUS},
        {"CycleCount", SBS_CYCLE_COUNT},
        {"DesignCapacity", SBS_DESIGN_CAPACITY},
        {"DesignVoltage", SBS_DESIGN_VOLTAGE},
    };

    bool capacity_mode = false;

    ESP_LOGI(TAG, "--- Standard SBS words ---");
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        uint16_t value;
        esp_err_t err = bms_read_word(commands[i].command, &value);
        if (err != ESP_OK) {
            log_error(commands[i].name, err);
            continue;
        }

        switch (commands[i].command) {
        case SBS_BATTERY_MODE:
            capacity_mode = (value & BIT(15)) != 0;
            ESP_LOGI(TAG, "%s: raw=0x%04X, CAPM=%u (%s)", commands[i].name,
                     value, (unsigned)capacity_mode,
                     capacity_mode ? "10mWh" : "mAh");
            break;
        case SBS_TEMPERATURE:
            ESP_LOGI(TAG, "%s: raw=0x%04X, %.2f C", commands[i].name, value,
                     ((float)value / 10.0f) - 273.15f);
            break;
        case SBS_VOLTAGE:
        case SBS_DESIGN_VOLTAGE:
            ESP_LOGI(TAG, "%s: raw=0x%04X, %u mV", commands[i].name, value, value);
            break;
        case SBS_CURRENT:
        case SBS_AVERAGE_CURRENT:
            ESP_LOGI(TAG, "%s: raw=0x%04X, %d mA", commands[i].name, value,
                     (int16_t)value);
            break;
        case SBS_RELATIVE_SOC:
            ESP_LOGI(TAG, "%s: raw=0x%04X, %u %%", commands[i].name, value, value);
            break;
        case SBS_DESIGN_CAPACITY:
            ESP_LOGI(TAG, "%s: raw=0x%04X, %u %s", commands[i].name, value, value,
                     capacity_mode ? "x 10mWh" : "mAh");
            break;
        default:
            ESP_LOGI(TAG, "%s: raw=0x%04X, %u", commands[i].name, value, value);
            break;
        }
    }
}

static esp_err_t bms_log_mfg_block(const char *name, uint16_t subcommand,
                                   size_t expected_length, uint8_t *out)
{
    esp_err_t err = bms_read_mfg(subcommand, out, expected_length);
    if (err != ESP_OK) {
        log_error(name, err);
        return err;
    }
    log_hex(name, out, expected_length);
    return ESP_OK;
}

static esp_err_t bms_log_status_group(uint32_t *pf_status, uint32_t *operation_status)
{
    uint8_t payload[32];
    uint32_t value;

    ESP_LOGI(TAG, "--- BQ30 status blocks ---");
    bms_log_mfg_block("DeviceType", MA_DEVICE_TYPE, 2, payload);
    bms_log_mfg_block("FirmwareVersion", MA_FIRMWARE_VERSION, 13, payload);
    bms_log_mfg_block("SafetyAlert", MA_SAFETY_ALERT, 4, payload);
    bms_log_mfg_block("SafetyStatus", MA_SAFETY_STATUS, 4, payload);
    bms_log_mfg_block("PFAlert", MA_PF_ALERT, 4, payload);

    esp_err_t err = bms_read_mfg_u32(MA_PF_STATUS, &value);
    if (err == ESP_OK) {
        uint8_t raw[4] = {(uint8_t)value, (uint8_t)(value >> 8),
                          (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
        log_hex("PFStatus", raw, sizeof(raw));
        bms_log_pf_status(value);
        if (pf_status != NULL) {
            *pf_status = value;
        }
    } else {
        log_error("PFStatus", err);
    }

    err = bms_read_mfg_u32(MA_OPERATION_STATUS, &value);
    if (err == ESP_OK) {
        uint8_t raw[4] = {(uint8_t)value, (uint8_t)(value >> 8),
                          (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
        log_hex("OperationStatus", raw, sizeof(raw));
        bms_log_operation_status(value);
        if (operation_status != NULL) {
            *operation_status = value;
        }
    } else {
        log_error("OperationStatus", err);
        return err;
    }

    bms_log_mfg_block("ChargingStatus", MA_CHARGING_STATUS, 3, payload);
    bms_log_mfg_block("GaugingStatus", MA_GAUGING_STATUS, 2, payload);
    bms_log_mfg_block("ManufacturingStatus", MA_MANUFACTURING_STATUS, 2, payload);
    return ESP_OK;
}

static void bms_log_measurements(bool include_extended)
{
    uint8_t payload[32];

    ESP_LOGI(TAG, "--- BQ30 measurements ---");
    bms_log_standard_words();

    if (bms_read_mfg(MA_VOLTAGES, payload, 12) == ESP_OK) {
        static const char *const names[] = {"Cell0", "Cell1", "Cell2", "Cell3", "BAT", "PACK"};
        log_hex("Voltages", payload, 12);
        for (size_t i = 0; i < 6; ++i) {
            const uint16_t raw = le16(&payload[i * 2]);
            const uint32_t millivolts = (i == 5) ? (uint32_t)raw * 100U : raw;
            ESP_LOGI(TAG, "%s: raw=%u, %" PRIu32 " mV", names[i], raw, millivolts);
        }
    } else {
        ESP_LOGW(TAG, "Voltages block unavailable");
    }

    if (bms_read_mfg(MA_TEMPERATURES, payload, 14) == ESP_OK) {
        static const char *const names[] = {"Internal", "TS1", "TS2", "TS3", "TS4", "Cell", "FET"};
        log_hex("Temperatures", payload, 14);
        for (size_t i = 0; i < 7; ++i) {
            const uint16_t raw = le16(&payload[i * 2]);
            ESP_LOGI(TAG, "%s temperature: raw=%u, %.2f C", names[i], raw,
                     ((float)raw / 10.0f) - 273.15f);
        }
    } else {
        ESP_LOGW(TAG, "Temperatures block unavailable (some BQ firmware reports 10 bytes)");
    }

    if (!include_extended) {
        return;
    }

    if (bms_log_mfg_block("ManufacturerInfo", MA_MANUFACTURER_INFO, 32, payload) == ESP_OK) {
        log_ascii("ManufacturerInfo", payload, 32);
    }
    bms_log_mfg_block("LifetimeData1", MA_LIFETIME_DATA_1, 32, payload);
    bms_log_mfg_block("LifetimeData2", MA_LIFETIME_DATA_2, 27, payload);
    bms_log_mfg_block("LifetimeData3", MA_LIFETIME_DATA_3, 16, payload);
    bms_log_mfg_block("ITStatus1", MA_IT_STATUS_1, 30, payload);
    bms_log_mfg_block("ITStatus2", MA_IT_STATUS_2, 10, payload);
}

static void bms_log_snapshot(void)
{
    uint32_t pf_status = 0;
    uint32_t operation_status = 0;

    ESP_LOGI(TAG, "===== BMS SNAPSHOT START =====");
    if (bms_probe() != ESP_OK) {
        ESP_LOGE(TAG, "No ACK from BMS at 0x%02X", BMS_ADDRESS);
        return;
    }
    bms_log_status_group(&pf_status, &operation_status);
    bms_log_standard_words();
    ESP_LOGI(TAG, "Snapshot summary: PF=0x%08" PRIX32 ", security=%s", pf_status,
             bms_security_state_name(operation_status));
    ESP_LOGI(TAG, "===== BMS SNAPSHOT END =====");
}

static void bms_log_full_dump(void)
{
    uint32_t ignored_pf;
    uint32_t ignored_op;

    ESP_LOGI(TAG, "========== BMS FULL DUMP START ==========");
    ESP_LOGI(TAG,
             "Target=BQ30Z554-R1, address=0x%02X, I2C=%u Hz, SCL wait=%u us, SDA=%d, SCL=%d",
             BMS_ADDRESS, BMS_CLOCK_HZ, BMS_SCL_WAIT_US, BMS_SDA_GPIO, BMS_SCL_GPIO);
    bms_log_line_levels();
    if (bms_probe() != ESP_OK) {
        ESP_LOGE(TAG, "No ACK from BMS at 0x%02X; check wiring and voltage level", BMS_ADDRESS);
        return;
    }
    bms_log_status_group(&ignored_pf, &ignored_op);
    bms_log_measurements(true);
    ESP_LOGI(TAG, "========== BMS FULL DUMP END ==========");
}

/* TI 9.5: B1 = KD || reverse(Mwire), B2 = KD || HMAC1, send reverse(HMAC2). */
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

static esp_err_t bms_unseal(void)
{
    uint8_t challenge[20];
    uint8_t digest[20];
    uint32_t operation_status;

    ESP_LOGW(TAG, "UNSEAL requested; starting SHA-1 challenge/response");
    ESP_RETURN_ON_ERROR(bms_write_word(SBS_MANUFACTURER_ACCESS, MA_UNSEAL), TAG,
                        "start unseal");
    ESP_RETURN_ON_ERROR(bms_read_block_exact(SBS_MANUFACTURER_INPUT, challenge,
                                              sizeof(challenge)), TAG, "read challenge");
    log_hex("Unseal challenge", challenge, sizeof(challenge));

    bms_make_unseal_digest(k_ogs_default_unseal_key, challenge, digest);
    ESP_RETURN_ON_ERROR(bms_write_block(SBS_MANUFACTURER_INPUT, digest, sizeof(digest)), TAG,
                        "write unseal response");
    vTaskDelay(pdMS_TO_TICKS(BMS_UNSEAL_WAIT_MS));

    ESP_RETURN_ON_ERROR(bms_read_mfg_u32(MA_OPERATION_STATUS, &operation_status), TAG,
                        "read operation status after unseal");
    bms_log_operation_status(operation_status);
    if (!bms_security_is_unsealed(operation_status)) {
        return ESP_ERR_INVALID_STATE;
    }

    g_unsealed_session = true;
    g_full_access_session = false;
    ESP_LOGW(TAG, "BMS is UNSEALED. Run 'pf-reset CONFIRM' (CUDEP only) or 'seal CONFIRM'.");
    return ESP_OK;
}

static esp_err_t bms_full_access(void)
{
    uint8_t challenge[20];
    uint8_t digest[20];
    uint32_t operation_status;

    if (!g_unsealed_session) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "FULL ACCESS requested; starting SHA-1 challenge/response");
    ESP_RETURN_ON_ERROR(bms_write_word(SBS_MANUFACTURER_ACCESS, MA_FULL_ACCESS), TAG,
                        "start full access");
    ESP_RETURN_ON_ERROR(bms_read_block_exact(SBS_MANUFACTURER_INPUT, challenge,
                                              sizeof(challenge)), TAG, "read full access challenge");
    log_hex("Full access challenge", challenge, sizeof(challenge));

    bms_make_unseal_digest(k_ogs_default_unseal_key, challenge, digest);
    ESP_RETURN_ON_ERROR(bms_write_block(SBS_MANUFACTURER_INPUT, digest, sizeof(digest)), TAG,
                        "write full access response");
    vTaskDelay(pdMS_TO_TICKS(BMS_UNSEAL_WAIT_MS));

    ESP_RETURN_ON_ERROR(bms_read_mfg_u32(MA_OPERATION_STATUS, &operation_status), TAG,
                        "read operation status after full access");
    bms_log_operation_status(operation_status);
    if (!bms_security_is_full_access(operation_status)) {
        return ESP_ERR_INVALID_STATE;
    }

    g_full_access_session = true;
    ESP_LOGW(TAG, "BMS is in FULL ACCESS. Data Flash writes are now permitted.");
    return ESP_OK;
}

static esp_err_t bms_seal(void)
{
    esp_err_t err = bms_write_word(SBS_MANUFACTURER_ACCESS, MA_SEAL);
    if (err == ESP_OK) {
        g_unsealed_session = false;
        g_full_access_session = false;
        ESP_LOGI(TAG, "Seal command sent");
    }
    return err;
}

typedef struct {
    uint16_t capacity_mah;
    uint16_t nominal_voltage_mv;
    uint16_t low_temp_charge_mv;
    uint16_t standard_temp_charge_mv;
    uint16_t high_temp_charge_mv;
    uint16_t rec_temp_charge_mv;
    uint16_t capacity_cwh;
} bms_profile_t;

typedef struct {
    uint8_t row[5];
    uint8_t original[5][BMS_DF_ROW_SIZE];
    uint8_t updated[5][BMS_DF_ROW_SIZE];
    bool dirty[5];
} bms_df_image_t;

static const uint8_t k_commission_rows[] = {3, 4, 8, 15, 16};

static esp_err_t bms_df_select_row(uint8_t row)
{
    ESP_RETURN_ON_ERROR(bms_write_word(SBS_MANUFACTURER_ACCESS, (uint16_t)(0x0100U | row)),
                        TAG, "select DF row 0x%02X", row);
    vTaskDelay(pdMS_TO_TICKS(BMS_MFG_SELECT_WAIT_MS));
    return ESP_OK;
}

static esp_err_t bms_df_read_row(uint8_t row, uint8_t data[BMS_DF_ROW_SIZE])
{
    ESP_RETURN_ON_ERROR(bms_df_select_row(row), TAG, "select DF row for read");
    return bms_read_block_exact(SBS_MANUFACTURER_INPUT, data, BMS_DF_ROW_SIZE);
}

static esp_err_t bms_df_write_row(uint8_t row, const uint8_t data[BMS_DF_ROW_SIZE])
{
    ESP_RETURN_ON_ERROR(bms_df_select_row(row), TAG, "select DF row for write");
    return bms_write_block(SBS_MANUFACTURER_INPUT, data, BMS_DF_ROW_SIZE);
}

static int bms_df_image_find_row(const bms_df_image_t *image, uint8_t row)
{
    for (size_t i = 0; i < sizeof(image->row) / sizeof(image->row[0]); ++i) {
        if (image->row[i] == row) {
            return (int)i;
        }
    }
    return -1;
}

static uint8_t *bms_df_image_byte(bms_df_image_t *image, uint16_t physical_address)
{
    const uint8_t row = (uint8_t)(physical_address / BMS_DF_ROW_SIZE);
    const uint8_t offset = (uint8_t)(physical_address % BMS_DF_ROW_SIZE);
    const int index = bms_df_image_find_row(image, row);

    return index < 0 ? NULL : &image->updated[index][offset];
}

static const uint8_t *bms_df_image_const_byte(const bms_df_image_t *image,
                                                uint16_t physical_address)
{
    const uint8_t row = (uint8_t)(physical_address / BMS_DF_ROW_SIZE);
    const uint8_t offset = (uint8_t)(physical_address % BMS_DF_ROW_SIZE);
    const int index = bms_df_image_find_row(image, row);

    return index < 0 ? NULL : &image->updated[index][offset];
}

static esp_err_t bms_df_image_read_u16(const bms_df_image_t *image,
                                       uint16_t physical_address, uint16_t *value)
{
    const uint8_t *low = bms_df_image_const_byte(image, physical_address);
    const uint8_t *high = bms_df_image_const_byte(image, (uint16_t)(physical_address + 1U));

    if (low == NULL || high == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *value = (uint16_t)*low | ((uint16_t)*high << 8);
    return ESP_OK;
}

static esp_err_t bms_df_image_write_u16(bms_df_image_t *image,
                                        uint16_t physical_address, uint16_t value)
{
    uint8_t *low = bms_df_image_byte(image, physical_address);
    uint8_t *high = bms_df_image_byte(image, (uint16_t)(physical_address + 1U));

    if (low == NULL || high == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *low = (uint8_t)value;
    *high = (uint8_t)(value >> 8);
    return ESP_OK;
}

static esp_err_t bms_df_image_read(bms_df_image_t *image)
{
    memset(image, 0, sizeof(*image));
    for (size_t i = 0; i < sizeof(k_commission_rows) / sizeof(k_commission_rows[0]); ++i) {
        image->row[i] = k_commission_rows[i];
        ESP_RETURN_ON_ERROR(bms_df_read_row(image->row[i], image->original[i]), TAG,
                            "read DF row 0x%02X", image->row[i]);
        memcpy(image->updated[i], image->original[i], BMS_DF_ROW_SIZE);
    }
    return ESP_OK;
}

static void bms_log_df_image(const char *label, const bms_df_image_t *image)
{
    uint16_t initial_mode;
    uint16_t design_voltage;
    uint16_t capacity_mah;
    uint16_t capacity_cwh;
    uint16_t low_temp_voltage;
    uint16_t standard_temp_voltage;
    uint16_t high_temp_voltage;
    uint16_t rec_temp_voltage;
    char row_label[48];

    for (size_t i = 0; i < sizeof(image->row) / sizeof(image->row[0]); ++i) {
        snprintf(row_label, sizeof(row_label), "%s DF row 0x%02X", label, image->row[i]);
        log_hex(row_label, image->updated[i], BMS_DF_ROW_SIZE);
    }

    if (bms_df_image_read_u16(image, DF_SBS_DATA_SUBCLASS + DF_SBS_INITIAL_BATTERY_MODE_OFFSET,
                              &initial_mode) != ESP_OK ||
        bms_df_image_read_u16(image, DF_SBS_DATA_SUBCLASS + DF_SBS_DESIGN_VOLTAGE_OFFSET,
                              &design_voltage) != ESP_OK ||
        bms_df_image_read_u16(image, DF_SBS_DATA_SUBCLASS + DF_SBS_DESIGN_CAPACITY_MAH_OFFSET,
                              &capacity_mah) != ESP_OK ||
        bms_df_image_read_u16(image, DF_SBS_DATA_SUBCLASS + DF_SBS_DESIGN_CAPACITY_CWH_OFFSET,
                              &capacity_cwh) != ESP_OK ||
        bms_df_image_read_u16(image, DF_LOW_TEMP_CHARGING_SUBCLASS + DF_CHARGING_VOLTAGE_OFFSET,
                              &low_temp_voltage) != ESP_OK ||
        bms_df_image_read_u16(image, DF_STANDARD_TEMP_CHARGING_SUBCLASS + DF_CHARGING_VOLTAGE_OFFSET,
                              &standard_temp_voltage) != ESP_OK ||
        bms_df_image_read_u16(image, DF_HIGH_TEMP_CHARGING_SUBCLASS + DF_CHARGING_VOLTAGE_OFFSET,
                              &high_temp_voltage) != ESP_OK ||
        bms_df_image_read_u16(image, DF_REC_TEMP_CHARGING_SUBCLASS + DF_CHARGING_VOLTAGE_OFFSET,
                              &rec_temp_voltage) != ESP_OK) {
        ESP_LOGE(TAG, "%s DF image does not contain all required fields", label);
        return;
    }

    ESP_LOGI(TAG,
             "%s profile: InitialBatteryMode=0x%04X CAPM=%u, DesignVoltage=%u mV, "
             "DesignCapacity=%u mAh / %u x10mWh",
             label, initial_mode, (unsigned)((initial_mode & BIT(15)) != 0), design_voltage,
             capacity_mah, capacity_cwh);
    ESP_LOGI(TAG, "%s charge voltage per cell: low=%u, standard=%u, high=%u, recommended=%u mV",
             label, low_temp_voltage, standard_temp_voltage, high_temp_voltage, rec_temp_voltage);

    static const uint8_t cov_threshold_offsets[] = {
        DF_COV_THRESHOLD_LOW_OFFSET, DF_COV_THRESHOLD_STANDARD_OFFSET,
        DF_COV_THRESHOLD_HIGH_OFFSET, DF_COV_THRESHOLD_REC_OFFSET,
    };
    static const uint8_t cov_recovery_offsets[] = {
        DF_COV_RECOVERY_LOW_OFFSET, DF_COV_RECOVERY_STANDARD_OFFSET,
        DF_COV_RECOVERY_HIGH_OFFSET, DF_COV_RECOVERY_REC_OFFSET,
    };
    uint16_t cov_thresholds[4];
    uint16_t cov_recoveries[4];
    for (size_t i = 0; i < sizeof(cov_thresholds) / sizeof(cov_thresholds[0]); ++i) {
        if (bms_df_image_read_u16(image, DF_COV_SUBCLASS + cov_threshold_offsets[i],
                                  &cov_thresholds[i]) != ESP_OK ||
            bms_df_image_read_u16(image, DF_COV_SUBCLASS + cov_recovery_offsets[i],
                                  &cov_recoveries[i]) != ESP_OK) {
            ESP_LOGE(TAG, "%s DF image is missing COV fields", label);
            return;
        }
    }
    ESP_LOGI(TAG, "%s COV threshold: low=%u, standard=%u, high=%u, recommended=%u mV",
             label, cov_thresholds[0], cov_thresholds[1], cov_thresholds[2], cov_thresholds[3]);
    ESP_LOGI(TAG, "%s COV recovery: low=%u, standard=%u, high=%u, recommended=%u mV",
             label, cov_recoveries[0], cov_recoveries[1], cov_recoveries[2], cov_recoveries[3]);
}

static esp_err_t bms_log_profile(void)
{
    bms_df_image_t image;
    uint16_t battery_mode;
    esp_err_t err = bms_read_word(SBS_BATTERY_MODE, &battery_mode);

    if (err != ESP_OK) {
        log_error("BatteryMode", err);
        return err;
    }
    ESP_LOGI(TAG, "BatteryMode=0x%04X CAPM=%u (%s)", battery_mode,
             (unsigned)((battery_mode & BIT(15)) != 0),
             (battery_mode & BIT(15)) != 0 ? "10mWh" : "mAh");

    err = bms_df_image_read(&image);
    if (err != ESP_OK) {
        log_error("read commissioning Data Flash", err);
        return err;
    }
    bms_log_df_image("CURRENT", &image);
    return ESP_OK;
}

static bool bms_profile_is_valid(bms_profile_t *profile)
{
    const uint16_t charge_voltages[] = {
        profile->low_temp_charge_mv,
        profile->standard_temp_charge_mv,
        profile->high_temp_charge_mv,
        profile->rec_temp_charge_mv,
    };
    uint32_t capacity_cwh;

    if (profile->capacity_mah < 100U || profile->capacity_mah > 32767U ||
        profile->nominal_voltage_mv < 6000U ||
        profile->nominal_voltage_mv > 18000U) {
        return false;
    }
    for (size_t i = 0; i < sizeof(charge_voltages) / sizeof(charge_voltages[0]); ++i) {
        if (charge_voltages[i] < 2500U || charge_voltages[i] > 4350U) {
            return false;
        }
    }

    capacity_cwh = ((uint32_t)profile->capacity_mah * profile->nominal_voltage_mv + 5000U) / 10000U;
    if (capacity_cwh == 0U || capacity_cwh > 32767U) {
        return false;
    }
    profile->capacity_cwh = (uint16_t)capacity_cwh;
    return true;
}

static esp_err_t bms_df_apply_profile_to_image(bms_df_image_t *image,
                                                const bms_profile_t *profile)
{
    const uint16_t charge_voltages[] = {
        profile->low_temp_charge_mv,
        profile->standard_temp_charge_mv,
        profile->high_temp_charge_mv,
        profile->rec_temp_charge_mv,
    };
    const uint16_t charge_subclasses[] = {
        DF_LOW_TEMP_CHARGING_SUBCLASS,
        DF_STANDARD_TEMP_CHARGING_SUBCLASS,
        DF_HIGH_TEMP_CHARGING_SUBCLASS,
        DF_REC_TEMP_CHARGING_SUBCLASS,
    };
    uint16_t highest_charge_voltage = profile->low_temp_charge_mv;
    static const uint8_t cov_threshold_offsets[] = {
        DF_COV_THRESHOLD_LOW_OFFSET, DF_COV_THRESHOLD_STANDARD_OFFSET,
        DF_COV_THRESHOLD_HIGH_OFFSET, DF_COV_THRESHOLD_REC_OFFSET,
    };
    static const uint8_t cov_recovery_offsets[] = {
        DF_COV_RECOVERY_LOW_OFFSET, DF_COV_RECOVERY_STANDARD_OFFSET,
        DF_COV_RECOVERY_HIGH_OFFSET, DF_COV_RECOVERY_REC_OFFSET,
    };

    for (size_t i = 1; i < sizeof(charge_voltages) / sizeof(charge_voltages[0]); ++i) {
        if (charge_voltages[i] > highest_charge_voltage) {
            highest_charge_voltage = charge_voltages[i];
        }
    }
    const uint16_t cov_threshold = (uint16_t)(highest_charge_voltage + BMS_COV_MARGIN_MV);
    const uint16_t cov_recovery = (uint16_t)(highest_charge_voltage - BMS_COV_MARGIN_MV);

    ESP_RETURN_ON_ERROR(bms_df_image_write_u16(image,
                                                DF_SBS_DATA_SUBCLASS + DF_SBS_DESIGN_VOLTAGE_OFFSET,
                                                profile->nominal_voltage_mv), TAG, "set DesignVoltage");
    ESP_RETURN_ON_ERROR(bms_df_image_write_u16(image,
                                                DF_SBS_DATA_SUBCLASS + DF_SBS_DESIGN_CAPACITY_MAH_OFFSET,
                                                profile->capacity_mah), TAG, "set DesignCapacity mAh");
    ESP_RETURN_ON_ERROR(bms_df_image_write_u16(image,
                                                DF_SBS_DATA_SUBCLASS + DF_SBS_DESIGN_CAPACITY_CWH_OFFSET,
                                                profile->capacity_cwh), TAG, "set DesignCapacity 10mWh");

    for (size_t i = 0; i < sizeof(charge_subclasses) / sizeof(charge_subclasses[0]); ++i) {
        ESP_RETURN_ON_ERROR(bms_df_image_write_u16(image,
                                                    charge_subclasses[i] + DF_CHARGING_VOLTAGE_OFFSET,
                                                    charge_voltages[i]), TAG,
                            "set charge voltage %u", (unsigned)i);
        ESP_RETURN_ON_ERROR(bms_df_image_write_u16(image,
                                                    DF_COV_SUBCLASS + cov_threshold_offsets[i],
                                                    cov_threshold), TAG,
                            "set COV threshold %u", (unsigned)i);
        ESP_RETURN_ON_ERROR(bms_df_image_write_u16(image,
                                                    DF_COV_SUBCLASS + cov_recovery_offsets[i],
                                                    cov_recovery), TAG,
                            "set COV recovery %u", (unsigned)i);
    }

    for (size_t i = 0; i < sizeof(image->row) / sizeof(image->row[0]); ++i) {
        image->dirty[i] = memcmp(image->original[i], image->updated[i], BMS_DF_ROW_SIZE) != 0;
    }
    return ESP_OK;
}

static esp_err_t bms_df_commit_image(const bms_df_image_t *image)
{
    bool touched[sizeof(image->row) / sizeof(image->row[0])] = {false};
    uint8_t verify[BMS_DF_ROW_SIZE];
    esp_err_t err = ESP_OK;

    for (size_t i = 0; i < sizeof(image->row) / sizeof(image->row[0]); ++i) {
        if (!image->dirty[i]) {
            continue;
        }
        ESP_LOGW(TAG, "Writing Data Flash row 0x%02X", image->row[i]);
        touched[i] = true;
        err = bms_df_write_row(image->row[i], image->updated[i]);
        if (err != ESP_OK) {
            log_error("write Data Flash row", err);
            goto rollback;
        }
        vTaskDelay(pdMS_TO_TICKS(BMS_DF_WRITE_WAIT_MS));
        err = bms_df_read_row(image->row[i], verify);
        if (err != ESP_OK) {
            log_error("verify read Data Flash row", err);
            goto rollback;
        }
        if (memcmp(verify, image->updated[i], BMS_DF_ROW_SIZE) != 0) {
            ESP_LOGE(TAG, "Data Flash verification mismatch for row 0x%02X", image->row[i]);
            err = ESP_ERR_INVALID_RESPONSE;
            goto rollback;
        }
        ESP_LOGI(TAG, "Verified Data Flash row 0x%02X", image->row[i]);
    }
    return ESP_OK;

rollback:
    ESP_LOGW(TAG, "Commissioning write failed; attempting rollback of touched Data Flash rows");
    for (size_t i = sizeof(image->row) / sizeof(image->row[0]); i-- > 0;) {
        if (!touched[i]) {
            continue;
        }
        const esp_err_t restore_err = bms_df_write_row(image->row[i], image->original[i]);
        if (restore_err == ESP_OK) {
            ESP_LOGI(TAG, "Rollback command sent for DF row 0x%02X", image->row[i]);
        } else {
            log_error("rollback Data Flash row", restore_err);
        }
    }
    return err;
}

static esp_err_t bms_apply_profile(const bms_profile_t *profile)
{
    bms_df_image_t image;
    esp_err_t err;

    if (!g_full_access_session) {
        return ESP_ERR_INVALID_STATE;
    }
    err = bms_df_image_read(&image);

    if (err != ESP_OK) {
        return err;
    }
    bms_log_df_image("BACKUP", &image);
    ESP_RETURN_ON_ERROR(bms_df_apply_profile_to_image(&image, profile), TAG,
                        "prepare commissioning profile");
    bms_log_df_image("PROPOSED", &image);

    err = bms_df_commit_image(&image);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "Data Flash profile committed; reading live SBS profile");
    return bms_log_profile();
}

static esp_err_t bms_reset_cudep_pf_in_session(void)
{
    uint32_t pf_status = 0;
    esp_err_t err;

    if (!g_unsealed_session) {
        return ESP_ERR_INVALID_STATE;
    }

    err = bms_read_mfg_u32(MA_PF_STATUS, &pf_status);
    if (err != ESP_OK) {
        return err;
    }
    bms_log_pf_status(pf_status);
    if (pf_status == 0) {
        ESP_LOGI(TAG, "PFStatus is already clear");
        return ESP_OK;
    }
    if (pf_status != PF_CUDEP_MASK) {
        ESP_LOGE(TAG, "PF reset refused: only PFStatus=0x%08X (CUDEP only) is allowed", PF_CUDEP_MASK);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "Sending Permanent Fail Data Reset (MA 0x0029)");
    err = bms_write_word(SBS_MANUFACTURER_ACCESS, MA_PF_DATA_RESET);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(BMS_UNSEAL_WAIT_MS));
    ESP_RETURN_ON_ERROR(bms_read_mfg_u32(MA_PF_STATUS, &pf_status), TAG,
                        "read PFStatus after reset");
    bms_log_pf_status(pf_status);
    return pf_status == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void bms_run_pf_reset(void)
{
    esp_err_t err = bms_reset_cudep_pf_in_session();
    if (err != ESP_OK) {
        log_error("CUDEP PF reset", err);
    }
    ESP_LOGI(TAG, "Collecting post-reset snapshot");
    bms_log_snapshot();

    err = bms_seal();
    if (err != ESP_OK) {
        log_error("seal after PF operation", err);
    }
}

static void bms_run_commission(const bms_profile_t *profile)
{
    uint32_t pf_status;
    esp_err_t err;

    if (g_unsealed_session) {
        ESP_LOGE(TAG, "Commissioning requires a sealed starting state; run 'seal CONFIRM' first");
        return;
    }
    if (bms_probe() != ESP_OK) {
        ESP_LOGE(TAG, "Commissioning refused: BMS is not responding");
        return;
    }
    err = bms_read_mfg_u32(MA_PF_STATUS, &pf_status);
    if (err != ESP_OK) {
        log_error("read PFStatus before commissioning", err);
        return;
    }
    if (pf_status != 0 && pf_status != PF_CUDEP_MASK) {
        bms_log_pf_status(pf_status);
        ESP_LOGE(TAG, "Commissioning refused: PF is not clear or CUDEP-only");
        return;
    }

    ESP_LOGW(TAG,
             "COMMISSIONING START: capacity=%u mAh, nominal=%u mV, charge cell voltages=%u/%u/%u/%u mV",
             profile->capacity_mah, profile->nominal_voltage_mv, profile->low_temp_charge_mv,
             profile->standard_temp_charge_mv, profile->high_temp_charge_mv, profile->rec_temp_charge_mv);
    bms_log_full_dump();
    err = bms_unseal();
    if (err != ESP_OK) {
        log_error("unseal for commissioning", err);
        goto reseal;
    }
    err = bms_full_access();
    if (err != ESP_OK) {
        log_error("full access for commissioning", err);
        goto reseal;
    }
    err = bms_apply_profile(profile);
    if (err != ESP_OK) {
        log_error("apply commissioning profile", err);
        goto reseal;
    }
    err = bms_reset_cudep_pf_in_session();
    if (err != ESP_OK) {
        log_error("clear CUDEP after profile update", err);
        goto reseal;
    }
    ESP_LOGI(TAG, "Commissioning profile verified and PF is clear");

reseal:
    {
        const esp_err_t seal_err = bms_seal();
        if (seal_err != ESP_OK) {
            log_error("seal after commissioning", seal_err);
        }
    }
    bms_log_snapshot();
    ESP_LOGI(TAG, "COMMISSIONING END");
}

static void print_help(void)
{
    printf("\nCommands:\n"
           "  help                 Show this command list\n"
           "  probe                Probe only BMS address 0x0B\n"
           "  snapshot             Log core SBS and BQ status\n"
           "  dump                 Log full SBS/BQ status, lifetime and IT blocks\n"
           "  watch on|off         Enable or disable a 2-second core snapshot\n"
           "  profile show         Read and log commissioning Data Flash rows\n"
           "  commission <mAh> <nominal-mV> <low-mV> <std-mV> <high-mV> <rec-mV> CONFIRM\n"
           "                       Apply profile, clear CUDEP-only PF, verify and seal\n"
           "  unseal CONFIRM       Run BQ30 SHA-1 unseal using the configured candidate key\n"
           "  pf-reset CONFIRM     Reset CUDEP-only PF in an unsealed session; always seals afterward\n"
           "  seal CONFIRM         Send Seal command and end the write session\n\n");
}

static bool has_confirm(const char *argument)
{
    return argument != NULL && strcmp(argument, "CONFIRM") == 0;
}

static bool parse_u16(const char *text, uint16_t *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || *text == '\0') {
        return false;
    }
    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT16_MAX) {
        return false;
    }
    *value = (uint16_t)parsed;
    return true;
}

static bool parse_commission_profile(char *const arguments[], size_t argument_count,
                                     bms_profile_t *profile)
{
    uint16_t values[6];

    if (argument_count != 7 || !has_confirm(arguments[6])) {
        return false;
    }
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (!parse_u16(arguments[i], &values[i])) {
            return false;
        }
    }

    *profile = (bms_profile_t){
        .capacity_mah = values[0],
        .nominal_voltage_mv = values[1],
        .low_temp_charge_mv = values[2],
        .standard_temp_charge_mv = values[3],
        .high_temp_charge_mv = values[4],
        .rec_temp_charge_mv = values[5],
    };
    return bms_profile_is_valid(profile);
}

static void handle_command(char *line)
{
    char *saveptr = NULL;
    char *command = strtok_r(line, " \t\r\n", &saveptr);
    char *arguments[8] = {0};
    size_t argument_count = 0;

    while (argument_count < sizeof(arguments) / sizeof(arguments[0])) {
        char *argument = strtok_r(NULL, " \t\r\n", &saveptr);
        if (argument == NULL) {
            break;
        }
        arguments[argument_count++] = argument;
    }
    char *argument = arguments[0];

    if (command == NULL) {
        return;
    }
    if (strcasecmp(command, "help") == 0) {
        print_help();
        return;
    }
    if (strcasecmp(command, "watch") == 0) {
        if (argument != NULL && strcasecmp(argument, "on") == 0) {
            g_watch_enabled = true;
            ESP_LOGI(TAG, "Periodic snapshot enabled");
        } else if (argument != NULL && strcasecmp(argument, "off") == 0) {
            g_watch_enabled = false;
            ESP_LOGI(TAG, "Periodic snapshot disabled");
        } else {
            ESP_LOGW(TAG, "Usage: watch on|off");
        }
        return;
    }
    if (!g_bms_ready) {
        ESP_LOGE(TAG, "I2C is not initialized; BMS commands are unavailable");
        return;
    }
    if (xSemaphoreTake(g_bms_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "BMS is busy");
        return;
    }

    if (strcasecmp(command, "probe") == 0) {
        const esp_err_t err = bms_probe();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "BMS ACK at 0x%02X", BMS_ADDRESS);
        } else {
            log_error("probe", err);
        }
    } else if (strcasecmp(command, "snapshot") == 0) {
        bms_log_snapshot();
    } else if (strcasecmp(command, "dump") == 0) {
        bms_log_full_dump();
    } else if (strcasecmp(command, "profile") == 0) {
        if (argument_count != 1 || strcasecmp(argument, "show") != 0) {
            ESP_LOGW(TAG, "Usage: profile show");
        } else {
            const esp_err_t err = bms_log_profile();
            if (err != ESP_OK) {
                log_error("profile show", err);
            }
        }
    } else if (strcasecmp(command, "commission") == 0) {
        bms_profile_t profile;
        if (!parse_commission_profile(arguments, argument_count, &profile)) {
            ESP_LOGW(TAG,
                     "Usage: commission <mAh> <nominal-mV> <low-mV> <std-mV> <high-mV> <rec-mV> CONFIRM");
            ESP_LOGW(TAG, "mAh=100..32767; nominal=6000..18000; each charge voltage=2500..4350 mV");
        } else {
            bms_run_commission(&profile);
        }
    } else if (strcasecmp(command, "unseal") == 0) {
        if (!has_confirm(argument)) {
            ESP_LOGW(TAG, "Usage: unseal CONFIRM");
        } else {
            esp_err_t err = bms_unseal();
            if (err != ESP_OK) {
                log_error("unseal", err);
                err = bms_seal();
                if (err != ESP_OK) {
                    log_error("seal after failed unseal", err);
                }
            }
        }
    } else if (strcasecmp(command, "pf-reset") == 0) {
        if (!has_confirm(argument)) {
            ESP_LOGW(TAG, "Usage: pf-reset CONFIRM");
        } else {
            bms_run_pf_reset();
        }
    } else if (strcasecmp(command, "seal") == 0) {
        if (!has_confirm(argument)) {
            ESP_LOGW(TAG, "Usage: seal CONFIRM");
        } else {
            esp_err_t err = bms_seal();
            if (err != ESP_OK) {
                log_error("seal", err);
            }
        }
    } else {
        ESP_LOGW(TAG, "Unknown command: %s", command);
        print_help();
    }

    xSemaphoreGive(g_bms_lock);
}

static esp_err_t console_uart_init(void)
{
    if (uart_is_driver_installed(UART_NUM_0)) {
        return ESP_OK;
    }

    /* UART0 pins and baud rate are already configured by the ESP-IDF console. */
    return uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
}

static void console_task(void *argument)
{
    char line[96];
    size_t length = 0;
    (void)argument;

    printf("bms> ");
    fflush(stdout);
    while (true) {
        uint8_t character;
        const int received = uart_read_bytes(UART_NUM_0, &character, 1, portMAX_DELAY);
        if (received != 1) {
            continue;
        }

        if (character == '\r' || character == '\n') {
            if (length == 0) {
                continue;
            }
            line[length] = '\0';
            printf("\r\n");
            handle_command(line);
            length = 0;
            printf("bms> ");
            fflush(stdout);
        } else if ((character == '\b' || character == 0x7F) && length > 0) {
            --length;
            printf("\b \b");
            fflush(stdout);
        } else if (character >= 32 && character <= 126) {
            if (length < sizeof(line) - 1) {
                line[length++] = (char)character;
                putchar(character);
                fflush(stdout);
            }
        }
    }
}

static void monitor_task(void *argument)
{
    (void)argument;
    while (true) {
        if (g_watch_enabled && g_bms_ready &&
            xSemaphoreTake(g_bms_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
            bms_log_snapshot();
            xSemaphoreGive(g_bms_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(BMS_MONITOR_PERIOD_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 BQ30Z554-R1 SMBus diagnostic firmware");
    ESP_LOGI(TAG, "Write operations are manual and require CONFIRM over UART");

    g_bms_lock = xSemaphoreCreateMutex();
    if (g_bms_lock == NULL) {
        ESP_LOGE(TAG, "Cannot allocate BMS mutex");
        return;
    }

    esp_err_t err = bms_i2c_init();
    if (err != ESP_OK) {
        log_error("I2C initialization", err);
    } else {
        g_bms_ready = true;
        if (xSemaphoreTake(g_bms_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            bms_log_full_dump();
            xSemaphoreGive(g_bms_lock);
        }
    }

    print_help();
    err = console_uart_init();
    if (err != ESP_OK) {
        log_error("UART console initialization", err);
        return;
    }
    xTaskCreate(console_task, "bms_console", 4096, NULL, 5, NULL);
    xTaskCreate(monitor_task, "bms_monitor", 4096, NULL, 4, NULL);
}
