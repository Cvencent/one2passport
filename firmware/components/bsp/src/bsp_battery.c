// components/bsp/src/bsp_battery.c
// 移植自 trae_card/components/platform/platform_esp32/src/battery_cw2017.c
//
// 本实现是 LEO RADIO 与 FoloOS 两版 BSP 的合并结果:
//   - 唤醒序列取 FoloOS 版:CW2017 必须按 0x30 -> 0x00 两步走。
//     只写 0x00 无法保证退出默认 0xF0 睡眠态,SOC 会长期停在 0xFF。
//   - 读重试取 LEO RADIO 版:单次 I2C 抖动不该让电量直接掉成 --%。
//   - 电压兜底曲线移到 bsp_battery_level(),与 FoloOS 的 API 划分保持一致,
//     使 bsp_battery_soc() 保持「芯片真实值或 -1」的纯粹语义。
#include "bsp_battery.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_batt";

#define CW_REG_VERSION   0x00   // 版本号,上电应答即代表芯片在位
#define CW_REG_VCELL_H   0x02   // 14bit 电压,V(uV) = raw * 312.5
#define CW_REG_SOC_H     0x04   // 高字节 = 整数百分比;低字节(0x05)= 1/256 %
#define CW_REG_CONFIG    0x08   // 0xF0=睡眠 / 0x30=复位态 / 0x00=正常

#define CW_CONFIG_RESTART 0x30
#define CW_CONFIG_ACTIVE  0x00

static i2c_master_dev_handle_t s_dev;

static int cw_read(uint8_t reg, uint8_t *buf, size_t n) {
    if (!s_dev) return -1;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100) == ESP_OK ? 0 : -1;
}

static int cw_write(uint8_t reg, uint8_t val) {
    if (!s_dev) return -1;
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100) == ESP_OK ? 0 : -1;
}

static int cw_read_soc_register(void) {
    uint8_t b[2] = { 0 };
    if (cw_read(CW_REG_SOC_H, b, 2) != 0) return -1;
    return ((int)b[0] << 8) | b[1];
}

static int cw_read_config(void) {
    uint8_t config = 0;
    if (cw_read(CW_REG_CONFIG, &config, 1) != 0) return -1;
    return config;
}

static esp_err_t cw_restart_and_verify(void) {
    /* CW2017 数据手册规定上电/重启序列必须是 0x30 -> 0x00。
     * 只写 0x00 无法保证从默认 0xF0 睡眠态退出,会导致 SOC
     * 长期为 0xFF,界面只能显示 --%。 */
    if (cw_write(CW_REG_CONFIG, CW_CONFIG_RESTART) != 0) {
        ESP_LOGW(TAG, "CW2017 写入重启态失败");
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    if (cw_write(CW_REG_CONFIG, CW_CONFIG_ACTIVE) != 0) {
        ESP_LOGW(TAG, "CW2017 写入正常态失败");
        return ESP_FAIL;
    }
    /* SOC 的首次估算耗时由芯片内部算法决定,不能在开机路径阻塞等待。
     * 这里只确认唤醒写入生效,后续由 UI 定时读取。 */
    vTaskDelay(pdMS_TO_TICKS(100));
    int config = cw_read_config();
    if (config != CW_CONFIG_ACTIVE) {
        ESP_LOGW(TAG, "CW2017 唤醒回读异常: CONFIG=0x%02X", config);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "CW2017 已进入正常模式,SOC 将异步更新");
    return ESP_OK;
}

esp_err_t bsp_battery_init(void) {
    if (s_dev) {
        /* 已初始化过。但 USB 复位或热插拔可能让芯片回到睡眠态,故复查 CONFIG。 */
        if (cw_read_config() == CW_CONFIG_ACTIVE) return ESP_OK;
        return cw_restart_and_verify();
    }

    esp_err_t e = bsp_i2c_init();
    if (e != ESP_OK) return e;

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BSP_I2C_CW2017_ADDR,
        .scl_speed_hz    = 100000,
    };
    e = i2c_master_bus_add_device(bsp_i2c_bus(), &dc, &s_dev);
    if (e != ESP_OK) { ESP_LOGE(TAG, "添加 I2C 设备失败: %s", esp_err_to_name(e)); return e; }

    uint8_t ver = 0;
    int version_read = -1;
    for (int attempt = 0; attempt < 3 && version_read != 0; ++attempt) {
        version_read = cw_read(CW_REG_VERSION, &ver, 1);
        if (version_read != 0) {
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
    if (version_read != 0) {
        ESP_LOGW(TAG, "CW2017 未应答 —— 用 bsp_i2c_scan() 确认 0x%02X 是否在线;"
                      "无电量计的板子可忽略本项", BSP_I2C_CW2017_ADDR);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    int config = cw_read_config();
    if (config < 0) {
        ESP_LOGW(TAG, "CW2017 CONFIG 读取失败");
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "检测到 CW2017 VERSION=0x%02X CONFIG=0x%02X", ver, config);

    /* SOC 寄存器上电默认值就是 0,不能用 SOC=0 判断芯片已就绪。
     * 必须先检查 CONFIG;默认 0xF0 表示仍在睡眠/复位态。 */
    if (config != CW_CONFIG_ACTIVE) return cw_restart_and_verify();

    return ESP_OK;
}

int bsp_battery_soc(void) {
    /* 单次 I2C 抖动不该让电量直接掉成 --%。三次重试覆盖 CW2017(0x63) 与
     * ES8311(0x18) 共用同一条 I2C 总线时偶发的读失败。 */
    for (int attempt = 0; attempt < 3; ++attempt) {
        int raw = cw_read_soc_register();
        if (raw >= 0) {
            int soc = raw >> 8;               // 高字节即整数百分比
            if (soc <= 100) return soc;
            return -1;                        // 0xFFFF 等未标定值:明确报不可用
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return -1;
}

int bsp_battery_soc_raw(void) {
    return cw_read_soc_register();
}

int bsp_battery_level(void) {
    int soc = bsp_battery_soc();
    if (soc >= 0) return soc;

    /* CW2017 算法未给出有效 SOC 时的显示降级。该曲线是常见 4.2V
     * 单节 Li-ion/Li-Po 的静态电压近似值;充电或大负载下会有误差,
     * 因此只作为界面可用性兜底,不替代电芯专用 profile。 */
    static const struct {
        int mv;
        int soc;
    } curve[] = {
        {4200, 100}, {4150, 95}, {4110, 90}, {4080, 80},
        {4020, 70},  {3980, 60}, {3950, 50}, {3910, 40},
        {3870, 30},  {3800, 20}, {3730, 10}, {3500, 5},
        {3200, 0},
    };
    int mv = bsp_battery_mv();
    if (mv < 0) return -1;
    size_t count = sizeof(curve) / sizeof(curve[0]);
    if (mv >= curve[0].mv) return curve[0].soc;
    if (mv <= curve[count - 1].mv) return curve[count - 1].soc;
    for (size_t i = 0; i + 1 < count; ++i) {
        if (mv >= curve[i + 1].mv) {
            int mv_span = curve[i].mv - curve[i + 1].mv;
            int soc_span = curve[i].soc - curve[i + 1].soc;
            return curve[i + 1].soc +
                   (mv - curve[i + 1].mv) * soc_span / mv_span;
        }
    }
    return -1;
}

int bsp_battery_mv(void) {
    uint8_t b[2] = { 0 };
    if (cw_read(CW_REG_VCELL_H, b, 2) != 0) return -1;
    uint32_t raw = ((uint32_t)b[0] << 8 | b[1]) & 0x3FFF;   // 14bit
    return (int)((raw * 3125) / 10000);                     // raw * 312.5uV -> mV
}

int bsp_battery_config(void) {
    return cw_read_config();
}
