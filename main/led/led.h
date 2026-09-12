#ifndef _LED_H_
#define _LED_H_

class Led {
public:
    virtual ~Led() = default;
    // Set the led state based on the device state
    virtual void OnStateChanged() = 0;
    /* 音乐 FFT 柱高（与频谱同一拍）。默认空实现，不占内存。 */
    virtual void OnMusicSpectrum(const int* bar_h, int bar_count, int bar_max_h) {
        (void)bar_h;
        (void)bar_count;
        (void)bar_max_h;
    }
};


class NoLed : public Led {
public:
    virtual void OnStateChanged() override {}
};

#endif // _LED_H_
