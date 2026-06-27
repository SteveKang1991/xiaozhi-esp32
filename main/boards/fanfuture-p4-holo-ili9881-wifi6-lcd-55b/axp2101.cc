/**
 * @file axp2101.cpp
 * @brief AXP2101 PMIC driver for ESP-IDF (C++ implementation)
 */

#include "axp2101.h"
#include "esp_log.h"
#include <cmath>

static const char *TAG = "AXP2101";

/* ========== 低级寄存器操作 ========== */

esp_err_t Axp2101::ReadReg(uint8_t reg, uint8_t *data)
{
    if (!dev_handle) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(dev_handle, &reg, 1, data, 1, 100);
}

esp_err_t Axp2101::WriteReg(uint8_t reg, uint8_t data)
{
    if (!dev_handle) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = {reg, data};
    return i2c_master_transmit(dev_handle, buf, 2, 100);
}

esp_err_t Axp2101::SetRegBit(uint8_t reg, uint8_t bit)
{
    uint8_t val;
    esp_err_t ret = ReadReg(reg, &val);
    if (ret != ESP_OK) return ret;
    val |= (1 << bit);
    return WriteReg(reg, val);
}

esp_err_t Axp2101::ClrRegBit(uint8_t reg, uint8_t bit)
{
    uint8_t val;
    esp_err_t ret = ReadReg(reg, &val);
    if (ret != ESP_OK) return ret;
    val &= ~(1 << bit);
    return WriteReg(reg, val);
}

bool Axp2101::GetRegBit(uint8_t reg, uint8_t bit)
{
    uint8_t val;
    if (ReadReg(reg, &val) != ESP_OK) return false;
    return (val >> bit) & 0x01;
}

uint16_t Axp2101::ReadAdcH6L8(uint8_t reg_h, uint8_t reg_l)
{
    uint8_t h, l;
    if (ReadReg(reg_h, &h) != ESP_OK) return 0;
    if (ReadReg(reg_l, &l) != ESP_OK) return 0;
    return ((uint16_t)(h & 0x3F) << 8) | l;
}

uint16_t Axp2101::ReadAdcH5L8(uint8_t reg_h, uint8_t reg_l)
{
    uint8_t h, l;
    if (ReadReg(reg_h, &h) != ESP_OK) return 0;
    if (ReadReg(reg_l, &l) != ESP_OK) return 0;
    return ((uint16_t)(h & 0x1F) << 8) | l;
}

/* ========== 构造函数和析构函数 ========== */

Axp2101::Axp2101(i2c_master_bus_handle_t i2c_bus) : dev_handle(nullptr)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to add I2C device: %s", esp_err_to_name(ret));
        dev_handle = nullptr;
        return;
    }
    
    ESP_LOGI(TAG, "✓ AXP2101 I2C device added (Address: 0x%02X)", AXP2101_I2C_ADDR);
}

Axp2101::~Axp2101()
{
    if (dev_handle) {
        i2c_master_bus_rm_device(dev_handle);
        dev_handle = nullptr;
    }
}

uint8_t Axp2101::GetChipId()
{
    uint8_t id;
    if (ReadReg(AXP2101_CHIP_ID, &id) != ESP_OK) return 0;
    return id;
}

/* ========== DCDC1 控制 ========== */

esp_err_t Axp2101::SetDcdc1Voltage(uint16_t millivolt)
{
    if (millivolt < 1500 || millivolt > 3400) return ESP_ERR_INVALID_ARG;
    if (millivolt % 100) return ESP_ERR_INVALID_ARG;
    uint8_t val = (millivolt - 1500) / 100;
    return WriteReg(AXP2101_DC_VOL0_CTRL, val);
}

uint16_t Axp2101::GetDcdc1Voltage()
{
    uint8_t val;
    if (ReadReg(AXP2101_DC_VOL0_CTRL, &val) != ESP_OK) return 0;
    return (val & 0x1F) * 100 + 1500;
}

esp_err_t Axp2101::EnableDcdc1(bool enable)
{
    return enable ? SetRegBit(AXP2101_DC_ONOFF_CTRL, 0) 
                  : ClrRegBit(AXP2101_DC_ONOFF_CTRL, 0);
}

bool Axp2101::IsDcdc1Enabled()
{
    return GetRegBit(AXP2101_DC_ONOFF_CTRL, 0);
}

/* ========== DCDC2 控制 ========== */

esp_err_t Axp2101::SetDcdc2Voltage(uint16_t millivolt)
{
    uint8_t val;
    if (ReadReg(AXP2101_DC_VOL1_CTRL, &val) != ESP_OK) return ESP_FAIL;
    val &= 0x80;
    
    if (millivolt >= 500 && millivolt <= 1200) {
        if (millivolt % 10) return ESP_ERR_INVALID_ARG;
        val |= (millivolt - 500) / 10;
    } else if (millivolt >= 1220 && millivolt <= 1540) {
        if (millivolt % 20) return ESP_ERR_INVALID_ARG;
        val |= ((millivolt - 1220) / 20) + 71;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return WriteReg(AXP2101_DC_VOL1_CTRL, val);
}

uint16_t Axp2101::GetDcdc2Voltage()
{
    uint8_t val;
    if (ReadReg(AXP2101_DC_VOL1_CTRL, &val) != ESP_OK) return 0;
    val &= 0x7F;
    if (val < 71) {
        return val * 10 + 500;
    } else {
        return val * 20 - 200;
    }
}

esp_err_t Axp2101::EnableDcdc2(bool enable)
{
    return enable ? SetRegBit(AXP2101_DC_ONOFF_CTRL, 1) 
                  : ClrRegBit(AXP2101_DC_ONOFF_CTRL, 1);
}

bool Axp2101::IsDcdc2Enabled()
{
    return GetRegBit(AXP2101_DC_ONOFF_CTRL, 1);
}

/* ========== DCDC3 控制 ========== */

esp_err_t Axp2101::SetDcdc3Voltage(uint16_t millivolt)
{
    uint8_t val;
    if (ReadReg(AXP2101_DC_VOL2_CTRL, &val) != ESP_OK) return ESP_FAIL;
    val &= 0x80;
    
    if (millivolt >= 500 && millivolt <= 1200) {
        if (millivolt % 10) return ESP_ERR_INVALID_ARG;
        val |= (millivolt - 500) / 10;
    } else if (millivolt >= 1220 && millivolt <= 1540) {
        if (millivolt % 20) return ESP_ERR_INVALID_ARG;
        val |= ((millivolt - 1220) / 20) + 71;
    } else if (millivolt >= 1600 && millivolt <= 3400) {
        if (millivolt % 100) return ESP_ERR_INVALID_ARG;
        val |= ((millivolt - 1600) / 100) + 88;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return WriteReg(AXP2101_DC_VOL2_CTRL, val);
}

uint16_t Axp2101::GetDcdc3Voltage()
{
    uint8_t val;
    if (ReadReg(AXP2101_DC_VOL2_CTRL, &val) != ESP_OK) return 0;
    val &= 0x7F;
    if (val < 71) {
        return val * 10 + 500;
    } else if (val < 88) {
        return val * 20 - 200;
    } else {
        return val * 100 - 7200;
    }
}

esp_err_t Axp2101::EnableDcdc3(bool enable)
{
    return enable ? SetRegBit(AXP2101_DC_ONOFF_CTRL, 2) 
                  : ClrRegBit(AXP2101_DC_ONOFF_CTRL, 2);
}

bool Axp2101::IsDcdc3Enabled()
{
    return GetRegBit(AXP2101_DC_ONOFF_CTRL, 2);
}

/* ========== DCDC4 控制 ========== */

esp_err_t Axp2101::SetDcdc4Voltage(uint16_t millivolt)
{
    uint8_t val;
    if (ReadReg(AXP2101_DC_VOL3_CTRL, &val) != ESP_OK) return ESP_FAIL;
    val &= 0x80;
    
    if (millivolt >= 500 && millivolt <= 1200) {
        if (millivolt % 10) return ESP_ERR_INVALID_ARG;
        val |= (millivolt - 500) / 10;
    } else if (millivolt >= 1220 && millivolt <= 1840) {
        if (millivolt % 20) return ESP_ERR_INVALID_ARG;
        val |= ((millivolt - 1220) / 20) + 71;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return WriteReg(AXP2101_DC_VOL3_CTRL, val);
}

uint16_t Axp2101::GetDcdc4Voltage()
{
    uint8_t val;
    if (ReadReg(AXP2101_DC_VOL3_CTRL, &val) != ESP_OK) return 0;
    val &= 0x7F;
    if (val < 71) {
        return val * 10 + 500;
    } else {
        return val * 20 - 200;
    }
}

esp_err_t Axp2101::EnableDcdc4(bool enable)
{
    return enable ? SetRegBit(AXP2101_DC_ONOFF_CTRL, 3) 
                  : ClrRegBit(AXP2101_DC_ONOFF_CTRL, 3);
}

bool Axp2101::IsDcdc4Enabled()
{
    return GetRegBit(AXP2101_DC_ONOFF_CTRL, 3);
}

/* ========== DCDC5 控制 ========== */

esp_err_t Axp2101::SetDcdc5Voltage(uint16_t millivolt)
{
    uint8_t val;
    if (ReadReg(AXP2101_DC_VOL4_CTRL, &val) != ESP_OK) return ESP_FAIL;
    val &= 0xE0;
    
    if (millivolt == 1200) {
        val |= 0x19;
    } else if (millivolt >= 1400 && millivolt <= 3700) {
        if (millivolt % 100) return ESP_ERR_INVALID_ARG;
        val |= (millivolt - 1400) / 100;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return WriteReg(AXP2101_DC_VOL4_CTRL, val);
}

uint16_t Axp2101::GetDcdc5Voltage()
{
    uint8_t val;
    if (ReadReg(AXP2101_DC_VOL4_CTRL, &val) != ESP_OK) return 0;
    val &= 0x1F;
    if (val == 0x19) return 1200;
    return val * 100 + 1400;
}

esp_err_t Axp2101::EnableDcdc5(bool enable)
{
    return enable ? SetRegBit(AXP2101_DC_ONOFF_CTRL, 4) 
                  : ClrRegBit(AXP2101_DC_ONOFF_CTRL, 4);
}

bool Axp2101::IsDcdc5Enabled()
{
    return GetRegBit(AXP2101_DC_ONOFF_CTRL, 4);
}

/* ========== ALDO1-4 控制 ========== */

esp_err_t Axp2101::SetAldo1Voltage(uint16_t millivolt)
{
    if (millivolt < 500 || millivolt > 3500) return ESP_ERR_INVALID_ARG;
    if (millivolt % 100) return ESP_ERR_INVALID_ARG;
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL0_CTRL, &val) != ESP_OK) return ESP_FAIL;
    val = (val & 0xE0) | ((millivolt - 500) / 100);
    return WriteReg(AXP2101_LDO_VOL0_CTRL, val);
}

uint16_t Axp2101::GetAldo1Voltage()
{
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL0_CTRL, &val) != ESP_OK) return 0;
    return (val & 0x1F) * 100 + 500;
}

esp_err_t Axp2101::EnableAldo1(bool enable)
{
    return enable ? SetRegBit(AXP2101_LDO_ONOFF_CTRL0, 0) 
                  : ClrRegBit(AXP2101_LDO_ONOFF_CTRL0, 0);
}

bool Axp2101::IsAldo1Enabled()
{
    return GetRegBit(AXP2101_LDO_ONOFF_CTRL0, 0);
}

esp_err_t Axp2101::SetAldo2Voltage(uint16_t millivolt)
{
    if (millivolt < 500 || millivolt > 3500) return ESP_ERR_INVALID_ARG;
    if (millivolt % 100) return ESP_ERR_INVALID_ARG;
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL1_CTRL, &val) != ESP_OK) return ESP_FAIL;
    val = (val & 0xE0) | ((millivolt - 500) / 100);
    return WriteReg(AXP2101_LDO_VOL1_CTRL, val);
}

uint16_t Axp2101::GetAldo2Voltage()
{
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL1_CTRL, &val) != ESP_OK) return 0;
    return (val & 0x1F) * 100 + 500;
}

esp_err_t Axp2101::EnableAldo2(bool enable)
{
    return enable ? SetRegBit(AXP2101_LDO_ONOFF_CTRL0, 1) 
                  : ClrRegBit(AXP2101_LDO_ONOFF_CTRL0, 1);
}

bool Axp2101::IsAldo2Enabled()
{
    return GetRegBit(AXP2101_LDO_ONOFF_CTRL0, 1);
}

esp_err_t Axp2101::SetAldo3Voltage(uint16_t millivolt)
{
    if (millivolt < 500 || millivolt > 3500) return ESP_ERR_INVALID_ARG;
    if (millivolt % 100) return ESP_ERR_INVALID_ARG;
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL2_CTRL, &val) != ESP_OK) return ESP_FAIL;
    val = (val & 0xE0) | ((millivolt - 500) / 100);
    return WriteReg(AXP2101_LDO_VOL2_CTRL, val);
}

uint16_t Axp2101::GetAldo3Voltage()
{
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL2_CTRL, &val) != ESP_OK) return 0;
    return (val & 0x1F) * 100 + 500;
}

esp_err_t Axp2101::EnableAldo3(bool enable)
{
    return enable ? SetRegBit(AXP2101_LDO_ONOFF_CTRL0, 2) 
                  : ClrRegBit(AXP2101_LDO_ONOFF_CTRL0, 2);
}

bool Axp2101::IsAldo3Enabled()
{
    return GetRegBit(AXP2101_LDO_ONOFF_CTRL0, 2);
}

esp_err_t Axp2101::SetAldo4Voltage(uint16_t millivolt)
{
    if (millivolt < 500 || millivolt > 3500) return ESP_ERR_INVALID_ARG;
    if (millivolt % 100) return ESP_ERR_INVALID_ARG;
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL3_CTRL, &val) != ESP_OK) return ESP_FAIL;
    val = (val & 0xE0) | ((millivolt - 500) / 100);
    return WriteReg(AXP2101_LDO_VOL3_CTRL, val);
}

uint16_t Axp2101::GetAldo4Voltage()
{
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL3_CTRL, &val) != ESP_OK) return 0;
    return (val & 0x1F) * 100 + 500;
}

esp_err_t Axp2101::EnableAldo4(bool enable)
{
    return enable ? SetRegBit(AXP2101_LDO_ONOFF_CTRL0, 3) 
                  : ClrRegBit(AXP2101_LDO_ONOFF_CTRL0, 3);
}

bool Axp2101::IsAldo4Enabled()
{
    return GetRegBit(AXP2101_LDO_ONOFF_CTRL0, 3);
}

/* ========== BLDO1-2 控制 ========== */

esp_err_t Axp2101::SetBldo1Voltage(uint16_t millivolt)
{
    if (millivolt < 500 || millivolt > 3500) return ESP_ERR_INVALID_ARG;
    if (millivolt % 100) return ESP_ERR_INVALID_ARG;
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL4_CTRL, &val) != ESP_OK) return ESP_FAIL;
    val = (val & 0xE0) | ((millivolt - 500) / 100);
    return WriteReg(AXP2101_LDO_VOL4_CTRL, val);
}

uint16_t Axp2101::GetBldo1Voltage()
{
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL4_CTRL, &val) != ESP_OK) return 0;
    return (val & 0x1F) * 100 + 500;
}

esp_err_t Axp2101::EnableBldo1(bool enable)
{
    return enable ? SetRegBit(AXP2101_LDO_ONOFF_CTRL0, 4) 
                  : ClrRegBit(AXP2101_LDO_ONOFF_CTRL0, 4);
}

bool Axp2101::IsBldo1Enabled()
{
    return GetRegBit(AXP2101_LDO_ONOFF_CTRL0, 4);
}

esp_err_t Axp2101::SetBldo2Voltage(uint16_t millivolt)
{
    if (millivolt < 500 || millivolt > 3500) return ESP_ERR_INVALID_ARG;
    if (millivolt % 100) return ESP_ERR_INVALID_ARG;
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL5_CTRL, &val) != ESP_OK) return ESP_FAIL;
    val = (val & 0xE0) | ((millivolt - 500) / 100);
    return WriteReg(AXP2101_LDO_VOL5_CTRL, val);
}

uint16_t Axp2101::GetBldo2Voltage()
{
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL5_CTRL, &val) != ESP_OK) return 0;
    return (val & 0x1F) * 100 + 500;
}

esp_err_t Axp2101::EnableBldo2(bool enable)
{
    return enable ? SetRegBit(AXP2101_LDO_ONOFF_CTRL0, 5) 
                  : ClrRegBit(AXP2101_LDO_ONOFF_CTRL0, 5);
}

bool Axp2101::IsBldo2Enabled()
{
    return GetRegBit(AXP2101_LDO_ONOFF_CTRL0, 5);
}

/* ========== DLDO1-2 控制 ========== */

esp_err_t Axp2101::SetDldo1Voltage(uint16_t millivolt)
{
    if (millivolt < 500 || millivolt > 3400) return ESP_ERR_INVALID_ARG;
    if (millivolt % 100) return ESP_ERR_INVALID_ARG;
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL7_CTRL, &val) != ESP_OK) return ESP_FAIL;
    val = (val & 0xE0) | ((millivolt - 500) / 100);
    return WriteReg(AXP2101_LDO_VOL7_CTRL, val);
}

uint16_t Axp2101::GetDldo1Voltage()
{
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL7_CTRL, &val) != ESP_OK) return 0;
    return (val & 0x1F) * 100 + 500;
}

esp_err_t Axp2101::EnableDldo1(bool enable)
{
    return enable ? SetRegBit(AXP2101_LDO_ONOFF_CTRL0, 7) 
                  : ClrRegBit(AXP2101_LDO_ONOFF_CTRL0, 7);
}

bool Axp2101::IsDldo1Enabled()
{
    return GetRegBit(AXP2101_LDO_ONOFF_CTRL0, 7);
}

esp_err_t Axp2101::SetDldo2Voltage(uint16_t millivolt)
{
    if (millivolt < 500 || millivolt > 3400) return ESP_ERR_INVALID_ARG;
    if (millivolt % 100) return ESP_ERR_INVALID_ARG;
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL8_CTRL, &val) != ESP_OK) return ESP_FAIL;
    val = (val & 0xE0) | ((millivolt - 500) / 100);
    return WriteReg(AXP2101_LDO_VOL8_CTRL, val);
}

uint16_t Axp2101::GetDldo2Voltage()
{
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL8_CTRL, &val) != ESP_OK) return 0;
    return (val & 0x1F) * 100 + 500;
}

esp_err_t Axp2101::EnableDldo2(bool enable)
{
    return enable ? SetRegBit(AXP2101_LDO_ONOFF_CTRL1, 0) 
                  : ClrRegBit(AXP2101_LDO_ONOFF_CTRL1, 0);
}

bool Axp2101::IsDldo2Enabled()
{
    return GetRegBit(AXP2101_LDO_ONOFF_CTRL1, 0);
}

/* ========== CPUSLDO 控制 ========== */

esp_err_t Axp2101::SetCpusldoVoltage(uint16_t millivolt)
{
    if (millivolt < 500 || millivolt > 1400) return ESP_ERR_INVALID_ARG;
    if (millivolt % 50) return ESP_ERR_INVALID_ARG;
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL6_CTRL, &val) != ESP_OK) return ESP_FAIL;
    val = (val & 0xE0) | ((millivolt - 500) / 50);
    return WriteReg(AXP2101_LDO_VOL6_CTRL, val);
}

uint16_t Axp2101::GetCpusldoVoltage()
{
    uint8_t val;
    if (ReadReg(AXP2101_LDO_VOL6_CTRL, &val) != ESP_OK) return 0;
    return (val & 0x1F) * 50 + 500;
}

esp_err_t Axp2101::EnableCpusldo(bool enable)
{
    return enable ? SetRegBit(AXP2101_LDO_ONOFF_CTRL0, 6) 
                  : ClrRegBit(AXP2101_LDO_ONOFF_CTRL0, 6);
}

bool Axp2101::IsCpusldoEnabled()
{
    return GetRegBit(AXP2101_LDO_ONOFF_CTRL0, 6);
}

/* ========== 电池和充电 ========== */

bool Axp2101::IsVbusGood()
{
    return GetRegBit(AXP2101_STATUS1, 5);
}

bool Axp2101::IsBatteryConnected()
{
    return GetRegBit(AXP2101_STATUS1, 3);
}

bool Axp2101::IsCharging()
{
    uint8_t val;
    if (ReadReg(AXP2101_STATUS2, &val) != ESP_OK) return false;
    return (val >> 5) == 0x01;
}

Axp2101ChgStatus Axp2101::GetChargeStatus()
{
    uint8_t val;
    if (ReadReg(AXP2101_STATUS2, &val) != ESP_OK) return Axp2101ChgStatus::CHG_STOP_STATE;
    return static_cast<Axp2101ChgStatus>(val & 0x07);
}

esp_err_t Axp2101::EnableCharging(bool enable)
{
    return enable ? SetRegBit(AXP2101_CHG_CTRL, 1) 
                  : ClrRegBit(AXP2101_CHG_CTRL, 1);
}

esp_err_t Axp2101::SetChargeCurrent(Axp2101ChgCurr current)
{
    uint8_t val;
    if (ReadReg(AXP2101_CHG_SET, &val) != ESP_OK) return ESP_FAIL;
    val = (val & 0xE0) | (static_cast<uint8_t>(current) & 0x1F);
    return WriteReg(AXP2101_CHG_SET, val);
}

esp_err_t Axp2101::SetChargeTargetVoltage(Axp2101ChgVol voltage)
{
    uint8_t val;
    if (ReadReg(AXP2101_CHG_VOL_SET, &val) != ESP_OK) return ESP_FAIL;
    val = (val & 0xF8) | (static_cast<uint8_t>(voltage) & 0x07);
    return WriteReg(AXP2101_CHG_VOL_SET, val);
}

uint16_t Axp2101::GetBatteryVoltage()
{
    if (!IsBatteryConnected()) return 0;
    return ReadAdcH5L8(AXP2101_ADC_DATA0, AXP2101_ADC_DATA1);
}

uint16_t Axp2101::GetVbusVoltage()
{
    if (!IsVbusGood()) return 0;
    return ReadAdcH6L8(AXP2101_ADC_DATA4, AXP2101_ADC_DATA5);
}

uint16_t Axp2101::GetSystemVoltage()
{
    return ReadAdcH6L8(AXP2101_ADC_DATA6, AXP2101_ADC_DATA7);
}

int Axp2101::GetBatteryPercent()
{
    if (!IsBatteryConnected()) return -1;
    uint8_t val;
    if (ReadReg(AXP2101_BAT_PERCENT, &val) != ESP_OK) return -1;
    return val;
}

float Axp2101::GetTemperature()
{
    uint16_t raw = ReadAdcH6L8(AXP2101_ADC_DATA8, AXP2101_ADC_DATA9);
    if (raw == 0 || raw > 16383) return NAN;
    return 22.0f + (7274.0f - raw) / 20.0f;
}

/* ========== ADC 控制 ========== */

esp_err_t Axp2101::EnableBatteryVoltageAdc(bool enable)
{
    return enable ? SetRegBit(AXP2101_ADC_CTRL, 0) 
                  : ClrRegBit(AXP2101_ADC_CTRL, 0);
}

esp_err_t Axp2101::EnableVbusVoltageAdc(bool enable)
{
    return enable ? SetRegBit(AXP2101_ADC_CTRL, 2) 
                  : ClrRegBit(AXP2101_ADC_CTRL, 2);
}

esp_err_t Axp2101::EnableSystemVoltageAdc(bool enable)
{
    return enable ? SetRegBit(AXP2101_ADC_CTRL, 3) 
                  : ClrRegBit(AXP2101_ADC_CTRL, 3);
}

esp_err_t Axp2101::EnableTemperatureAdc(bool enable)
{
    return enable ? SetRegBit(AXP2101_ADC_CTRL, 4) 
                  : ClrRegBit(AXP2101_ADC_CTRL, 4);
}

/* ========== 电源控制 ========== */

void Axp2101::Shutdown()
{
    SetRegBit(AXP2101_COMMON_CONFIG, 0);
}

void Axp2101::Reset()
{
    SetRegBit(AXP2101_COMMON_CONFIG, 1);
}

esp_err_t Axp2101::EnableBatfet(bool enable)
{
    return enable ? SetRegBit(AXP2101_BATFET_CTRL, 3) 
                  : ClrRegBit(AXP2101_BATFET_CTRL, 3);
}

/* ========== LED 控制 ========== */

esp_err_t Axp2101::SetLedMode(Axp2101LedMode mode)
{
    uint8_t val;
    if (ReadReg(AXP2101_CHGLED_CTRL, &val) != ESP_OK) return ESP_FAIL;
    
    if (mode == Axp2101LedMode::LED_CTRL_CHG) {
        val = (val & 0xF9) | 0x01;
    } else {
        val = (val & 0xC8) | 0x05 | (static_cast<uint8_t>(mode) << 4);
    }
    return WriteReg(AXP2101_CHGLED_CTRL, val);
}

/* ========== 中断控制 ========== */

esp_err_t Axp2101::EnableIrq(uint32_t irq_mask)
{
    esp_err_t ret = ESP_OK;
    if (irq_mask & 0xFF0000) {
        ret = WriteReg(AXP2101_IRQ_EN1, (irq_mask >> 16) & 0xFF);
    }
    if (ret == ESP_OK && (irq_mask & 0x00FF00)) {
        ret = WriteReg(AXP2101_IRQ_EN2, (irq_mask >> 8) & 0xFF);
    }
    if (ret == ESP_OK && (irq_mask & 0x0000FF)) {
        ret = WriteReg(AXP2101_IRQ_EN3, irq_mask & 0xFF);
    }
    return ret;
}

esp_err_t Axp2101::DisableIrq(uint32_t irq_mask)
{
    uint8_t val;
    esp_err_t ret = ESP_OK;
    
    if (irq_mask & 0xFF0000) {
        if (ReadReg(AXP2101_IRQ_EN1, &val) == ESP_OK) {
            ret = WriteReg(AXP2101_IRQ_EN1, val & ~((irq_mask >> 16) & 0xFF));
        }
    }
    if (ret == ESP_OK && (irq_mask & 0x00FF00)) {
        if (ReadReg(AXP2101_IRQ_EN2, &val) == ESP_OK) {
            ret = WriteReg(AXP2101_IRQ_EN2, val & ~((irq_mask >> 8) & 0xFF));
        }
    }
    if (ret == ESP_OK && (irq_mask & 0x0000FF)) {
        if (ReadReg(AXP2101_IRQ_EN3, &val) == ESP_OK) {
            ret = WriteReg(AXP2101_IRQ_EN3, val & ~(irq_mask & 0xFF));
        }
    }
    return ret;
}

uint32_t Axp2101::GetIrqStatus()
{
    uint8_t s1, s2, s3;
    ReadReg(AXP2101_IRQ_STAT1, &s1);
    ReadReg(AXP2101_IRQ_STAT2, &s2);
    ReadReg(AXP2101_IRQ_STAT3, &s3);
    return ((uint32_t)s1 << 16) | ((uint32_t)s2 << 8) | s3;
}

esp_err_t Axp2101::ClearIrqStatus()
{
    esp_err_t ret;
    ret = WriteReg(AXP2101_IRQ_STAT1, 0xFF);
    if (ret != ESP_OK) return ret;
    ret = WriteReg(AXP2101_IRQ_STAT2, 0xFF);
    if (ret != ESP_OK) return ret;
    return WriteReg(AXP2101_IRQ_STAT3, 0xFF);
}

