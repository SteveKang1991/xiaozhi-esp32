/**
 * @file axp2101.hpp
 * @brief AXP2101 PMIC driver for ESP-IDF (C++ version)
 */

#ifndef __AXP2101_HPP__
#define __AXP2101_HPP__

#include <cstdint>
#include <cstdbool>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* 寄存器定义 */
#define AXP2101_I2C_ADDR        0x34
#define AXP2101_STATUS1         0x00
#define AXP2101_STATUS2         0x01
#define AXP2101_CHIP_ID         0x03
#define AXP2101_COMMON_CONFIG   0x10
#define AXP2101_BATFET_CTRL     0x12
#define AXP2101_VBUS_VOL_LIM    0x15
#define AXP2101_VBUS_CUR_LIM    0x16
#define AXP2101_PWROFF_EN       0x22
#define AXP2101_DC_ONOFF_CTRL   0x80
#define AXP2101_DC_FORCE_PWM    0x81
#define AXP2101_DC_VOL0_CTRL    0x82  // DCDC1
#define AXP2101_DC_VOL1_CTRL    0x83  // DCDC2
#define AXP2101_DC_VOL2_CTRL    0x84  // DCDC3
#define AXP2101_DC_VOL3_CTRL    0x85  // DCDC4
#define AXP2101_DC_VOL4_CTRL    0x86  // DCDC5
#define AXP2101_LDO_ONOFF_CTRL0 0x90
#define AXP2101_LDO_ONOFF_CTRL1 0x91
#define AXP2101_LDO_VOL0_CTRL   0x92  // ALDO1
#define AXP2101_LDO_VOL1_CTRL   0x93  // ALDO2
#define AXP2101_LDO_VOL2_CTRL   0x94  // ALDO3
#define AXP2101_LDO_VOL3_CTRL   0x95  // ALDO4
#define AXP2101_LDO_VOL4_CTRL   0x96  // BLDO1
#define AXP2101_LDO_VOL5_CTRL   0x97  // BLDO2
#define AXP2101_LDO_VOL6_CTRL   0x98  // CPUSLDO
#define AXP2101_LDO_VOL7_CTRL   0x99  // DLDO1
#define AXP2101_LDO_VOL8_CTRL   0x9A  // DLDO2
#define AXP2101_BAT_PERCENT     0xA4
#define AXP2101_ADC_CTRL        0x30
#define AXP2101_ADC_DATA0       0x34  // VBAT H
#define AXP2101_ADC_DATA1       0x35  // VBAT L
#define AXP2101_ADC_DATA4       0x38  // VBUS H
#define AXP2101_ADC_DATA5       0x39  // VBUS L
#define AXP2101_ADC_DATA6       0x3A  // VSYS H
#define AXP2101_ADC_DATA7       0x3B  // VSYS L
#define AXP2101_ADC_DATA8       0x3C  // TEMP H
#define AXP2101_ADC_DATA9       0x3D  // TEMP L
#define AXP2101_IRQ_EN1         0x40
#define AXP2101_IRQ_EN2         0x41
#define AXP2101_IRQ_EN3         0x42
#define AXP2101_IRQ_STAT1       0x48
#define AXP2101_IRQ_STAT2       0x49
#define AXP2101_IRQ_STAT3       0x4A
#define AXP2101_CHG_CTRL        0x18
#define AXP2101_CHG_SET         0x62
#define AXP2101_CHG_VOL_SET     0x64
#define AXP2101_CHGLED_CTRL     0x69

/* 充电电流选项 */
enum class Axp2101ChgCurr : uint8_t {
    CUR_0MA = 0,
    CUR_100MA = 4,
    CUR_125MA,
    CUR_150MA,
    CUR_175MA,
    CUR_200MA,
    CUR_300MA,
    CUR_400MA,
    CUR_500MA,
    CUR_600MA,
    CUR_700MA,
    CUR_800MA,
    CUR_900MA,
    CUR_1000MA,
    CUR_1100MA,
    CUR_1200MA,
    CUR_1300MA,
    CUR_1400MA,
    CUR_1500MA,
    CUR_1600MA,
    CUR_1700MA,
    CUR_1800MA,
    CUR_1900MA,
    CUR_2000MA,
    CUR_2100MA,
    CUR_2200MA,
    CUR_2300MA,
    CUR_2400MA,
    CUR_2500MA,
    CUR_2600MA,
    CUR_2700MA,
    CUR_2800MA,
    CUR_2900MA,
    CUR_3000MA,
};

/* 充电目标电压 */
enum class Axp2101ChgVol : uint8_t {
    VOL_4V0 = 0,
    VOL_4V1,
    VOL_4V2,
    VOL_4V35,
    VOL_4V4,
};

/* LED 模式 */
enum class Axp2101LedMode : uint8_t {
    LED_OFF = 0,
    LED_BLINK_1HZ,
    LED_BLINK_4HZ,
    LED_ON,
    LED_CTRL_CHG,
};

/* 充电状态 */
enum class Axp2101ChgStatus : uint8_t {
    CHG_TRI_STATE = 0,
    CHG_PRE_STATE,
    CHG_CC_STATE,
    CHG_CV_STATE,
    CHG_DONE_STATE,
    CHG_STOP_STATE,
};

/**
 * @class Axp2101
 * @brief AXP2101 Power Management IC C++ Driver
 */
class Axp2101 {
public:
    i2c_master_dev_handle_t dev_handle;  // Public for status checking

    /**
     * @brief 构造函数
     * @param i2c_bus I2C 总线句柄
     */
    Axp2101(i2c_master_bus_handle_t i2c_bus);

    /**
     * @brief 析构函数
     */
    ~Axp2101();

    /**
     * @brief 获取芯片 ID
     * @return 芯片 ID (0x4A 或 0x4B)
     */
    uint8_t GetChipId();

    /* ========== DCDC 控制 ========== */
    esp_err_t SetDcdc1Voltage(uint16_t millivolt);  // 1500-3400mV
    esp_err_t SetDcdc2Voltage(uint16_t millivolt);  // 500-1540mV
    esp_err_t SetDcdc3Voltage(uint16_t millivolt);  // 500-3400mV
    esp_err_t SetDcdc4Voltage(uint16_t millivolt);  // 500-1840mV
    esp_err_t SetDcdc5Voltage(uint16_t millivolt);  // 1200 or 1400-3700mV

    uint16_t GetDcdc1Voltage();
    uint16_t GetDcdc2Voltage();
    uint16_t GetDcdc3Voltage();
    uint16_t GetDcdc4Voltage();
    uint16_t GetDcdc5Voltage();

    esp_err_t EnableDcdc1(bool enable);
    esp_err_t EnableDcdc2(bool enable);
    esp_err_t EnableDcdc3(bool enable);
    esp_err_t EnableDcdc4(bool enable);
    esp_err_t EnableDcdc5(bool enable);

    bool IsDcdc1Enabled();
    bool IsDcdc2Enabled();
    bool IsDcdc3Enabled();
    bool IsDcdc4Enabled();
    bool IsDcdc5Enabled();

    /* ========== ALDO 控制 ========== */
    esp_err_t SetAldo1Voltage(uint16_t millivolt);
    esp_err_t SetAldo2Voltage(uint16_t millivolt);
    esp_err_t SetAldo3Voltage(uint16_t millivolt);
    esp_err_t SetAldo4Voltage(uint16_t millivolt);

    uint16_t GetAldo1Voltage();
    uint16_t GetAldo2Voltage();
    uint16_t GetAldo3Voltage();
    uint16_t GetAldo4Voltage();

    esp_err_t EnableAldo1(bool enable);
    esp_err_t EnableAldo2(bool enable);
    esp_err_t EnableAldo3(bool enable);
    esp_err_t EnableAldo4(bool enable);

    bool IsAldo1Enabled();
    bool IsAldo2Enabled();
    bool IsAldo3Enabled();
    bool IsAldo4Enabled();

    /* ========== BLDO 控制 ========== */
    esp_err_t SetBldo1Voltage(uint16_t millivolt);
    esp_err_t SetBldo2Voltage(uint16_t millivolt);

    uint16_t GetBldo1Voltage();
    uint16_t GetBldo2Voltage();

    esp_err_t EnableBldo1(bool enable);
    esp_err_t EnableBldo2(bool enable);

    bool IsBldo1Enabled();
    bool IsBldo2Enabled();

    /* ========== DLDO 控制 ========== */
    esp_err_t SetDldo1Voltage(uint16_t millivolt);
    esp_err_t SetDldo2Voltage(uint16_t millivolt);

    uint16_t GetDldo1Voltage();
    uint16_t GetDldo2Voltage();

    esp_err_t EnableDldo1(bool enable);
    esp_err_t EnableDldo2(bool enable);

    bool IsDldo1Enabled();
    bool IsDldo2Enabled();

    /* ========== CPUSLDO 控制 ========== */
    esp_err_t SetCpusldoVoltage(uint16_t millivolt);
    uint16_t GetCpusldoVoltage();
    esp_err_t EnableCpusldo(bool enable);
    bool IsCpusldoEnabled();

    /* ========== 电池和充电 ========== */
    bool IsVbusGood();
    bool IsBatteryConnected();
    bool IsCharging();
    Axp2101ChgStatus GetChargeStatus();

    esp_err_t EnableCharging(bool enable);
    esp_err_t SetChargeCurrent(Axp2101ChgCurr current);
    esp_err_t SetChargeTargetVoltage(Axp2101ChgVol voltage);

    uint16_t GetBatteryVoltage();
    uint16_t GetVbusVoltage();
    uint16_t GetSystemVoltage();
    int GetBatteryPercent();
    float GetTemperature();

    /* ========== ADC 控制 ========== */
    esp_err_t EnableBatteryVoltageAdc(bool enable);
    esp_err_t EnableVbusVoltageAdc(bool enable);
    esp_err_t EnableSystemVoltageAdc(bool enable);
    esp_err_t EnableTemperatureAdc(bool enable);

    /* ========== 电源控制 ========== */
    void Shutdown();
    void Reset();
    esp_err_t EnableBatfet(bool enable);

    /* ========== LED 控制 ========== */
    esp_err_t SetLedMode(Axp2101LedMode mode);

    /* ========== 中断控制 ========== */
    esp_err_t EnableIrq(uint32_t irq_mask);
    esp_err_t DisableIrq(uint32_t irq_mask);
    uint32_t GetIrqStatus();
    esp_err_t ClearIrqStatus();

private:
    // 低级寄存器操作
    esp_err_t ReadReg(uint8_t reg, uint8_t *data);
    esp_err_t WriteReg(uint8_t reg, uint8_t data);
    esp_err_t SetRegBit(uint8_t reg, uint8_t bit);
    esp_err_t ClrRegBit(uint8_t reg, uint8_t bit);
    bool GetRegBit(uint8_t reg, uint8_t bit);
    uint16_t ReadAdcH6L8(uint8_t reg_h, uint8_t reg_l);
    uint16_t ReadAdcH5L8(uint8_t reg_h, uint8_t reg_l);
};

#endif /* __AXP2101_HPP__ */

