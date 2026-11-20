#ifndef BT_UART_H_
#define BT_UART_H_

#include <driver/gpio.h>
#include <driver/uart.h>
#include <functional>

// 外挂蓝牙板 UART：115200 8N1。P4 发送 GPIO23，接收 GPIO22。三块板相同。
#define BT_UART_NUM             UART_NUM_2
#define BT_UART_TX_PIN          GPIO_NUM_23
#define BT_UART_RX_PIN          GPIO_NUM_22
#define BT_UART_BAUD            115200

class BtUart {
public:
    BtUart();
    void Start(std::function<void(const char* text)> notify);
    void Toggle();

private:
    enum Mode { kOff = 0, kEmit };

    uart_port_t port_;
    gpio_num_t tx_;
    gpio_num_t rx_;
    int baud_;
    Mode mode_ = kOff;
    bool started_ = false;
    bool force_off_ = false;
    std::function<void(const char* text)> notify_;

    void Send(uint8_t cmd);
    void Notify(const char* text);
    void ForceOff();
    void OpenEmit();
    void Close();
    void RxTask();
    void HandleAck(uint8_t cmd);
    static const char* AckName(uint8_t cmd);
};

#endif
