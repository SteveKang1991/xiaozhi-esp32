#include "bt_uart.h"

#include <esp_log.h>
#include "freertos/task.h"

static const char* TAG = "BtUart";

BtUart::BtUart()
    : port_(BT_UART_NUM), tx_(BT_UART_TX_PIN), rx_(BT_UART_RX_PIN), baud_(BT_UART_BAUD) {}

void BtUart::Start(std::function<void(const char* text)> notify) {
    if (started_) {
        return;
    }
    notify_ = std::move(notify);
    uart_config_t cfg = {};
    cfg.baud_rate = baud_;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install(port_, 256, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(port_, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(port_, tx_, rx_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreate([](void* arg) {
        static_cast<BtUart*>(arg)->RxTask();
    }, "bt_uart_rx", 3072, this, 2, nullptr);
    started_ = true;
    ESP_LOGI(TAG, "蓝牙串口 UART%d 发送GPIO%d 接收GPIO%d %d 8N1", port_, tx_, rx_, baud_);
}

void BtUart::Toggle() {
    if (mode_ == kEmit) {
        Close();
    } else {
        OpenEmit();
    }
}

void BtUart::Send(uint8_t cmd) {
    uint8_t frame[5] = {0x55, 0xAA, 0x01, cmd, 0};
    uint8_t sum = 0;
    for (int i = 0; i < 4; i++) {
        sum = static_cast<uint8_t>(sum + frame[i]);
    }
    frame[4] = sum;
    uart_write_bytes(port_, frame, sizeof(frame));
    ESP_LOGI(TAG, "蓝牙指令 cmd=0x%02X", cmd);
}

void BtUart::Notify(const char* text) {
    if (notify_ && text != nullptr && text[0] != '\0') {
        notify_(text);
    }
}

void BtUart::ForceOff() {
    if (mode_ != kOff || force_off_) {
        return;
    }
    force_off_ = true;
    if (xTaskCreate([](void* arg) {
            auto* self = static_cast<BtUart*>(arg);
            vTaskDelay(pdMS_TO_TICKS(500));
            if (self->mode_ == kOff) {
                self->Send(0x01);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            if (self->mode_ == kOff) {
                self->Send(0x02);
            }
            self->force_off_ = false;
            vTaskDelete(nullptr);
        }, "bt_off", 2048, this, 2, nullptr) != pdPASS) {
        force_off_ = false;
    }
}

void BtUart::OpenEmit() {
    mode_ = kEmit;
    Send(0x01);
    Notify("进入蓝牙发射模式");
}

void BtUart::Close() {
    mode_ = kOff;
    Send(0x02);
    Notify("退出蓝牙模式");
}

const char* BtUart::AckName(uint8_t cmd) {
    switch (cmd) {
        case 0xE0: return "";  // 上电
        case 0xE1: return "播放";
        case 0xE2: return "暂停";
        case 0xE3: return "下一首";
        case 0xE4: return "上一首";
        case 0xE5: return "发射模式已断开";
        case 0xE6: return "发射模式已连接";
        default: return "未知";
    }
}

void BtUart::HandleAck(uint8_t cmd) {
    ESP_LOGI(TAG, "蓝牙ACK 0x%02X %s", cmd, AckName(cmd));
    if (mode_ == kOff && (cmd == 0xE0 || cmd == 0xE6)) {
        ForceOff();
        if (cmd == 0xE6) {
            return;
        }
    }
    /* 上电和自动关发射都会带 0xE5，这时人还没操作，状态栏不弹。 */
    if (cmd == 0xE5 && mode_ != kEmit) {
        return;
    }
    if (cmd == 0xE5 || cmd == 0xE6) {
        Notify(AckName(cmd));
    }
}

void BtUart::RxTask() {
    uint8_t frame[16] = {};
    int got = 0;
    uint8_t need = 0;
    while (true) {
        uint8_t byte = 0;
        if (uart_read_bytes(port_, &byte, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (got == 0) {
            if (byte != 0x55) {
                continue;
            }
            frame[got++] = byte;
            continue;
        }
        if (got == 1) {
            if (byte != 0xAA) {
                got = (byte == 0x55) ? 1 : 0;
                if (got == 1) {
                    frame[0] = 0x55;
                }
                continue;
            }
            frame[got++] = byte;
            continue;
        }
        frame[got++] = byte;
        if (got == 3) {
            need = byte;
            if (need < 1 || need > 11) {
                got = 0;
            }
            continue;
        }
        if (got < 3 + need + 1) {
            continue;
        }
        uint8_t sum = 0;
        for (int i = 0; i < 3 + need; i++) {
            sum = static_cast<uint8_t>(sum + frame[i]);
        }
        if (sum == frame[3 + need]) {
            HandleAck(frame[3]);
        }
        got = 0;
    }
}
