#ifndef AS5600_H
#define AS5600_H

#include "i2c_device.h"
#include "config.h"

#include <stdint.h>

/**
 * AMS AS5600 磁性角度传感器（I2C）。
 * RAW 角度寄存器 0x0C/0x0D，12 位，0～4095 对应 0°～360°。
 */
class As5600 : public I2cDevice {
public:
    explicit As5600(i2c_master_bus_handle_t i2c_bus)
        : I2cDevice(i2c_bus, AS5600_I2C_ADDR) {}

    /** 读取 12 位原始角度值 [0, 4095] */
    uint16_t ReadRawAngle() {
        uint8_t buf[2];
        ReadRegs(AS5600_REG_RAW_ANGLE, buf, 2);
        uint16_t raw = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
        return raw & 0x0FFF;
    }

    /** 转换为角度（度） */
    static float RawToDegrees(uint16_t raw) {
        return (static_cast<float>(raw) * 360.0f) / 4096.0f;
    }
};

#endif
