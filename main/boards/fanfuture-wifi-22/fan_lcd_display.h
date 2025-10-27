#ifndef FAN_LCD_DISPLAY_H
#define FAN_LCD_DISPLAY_H

#include "lcd_display.h"

// FAN LCD显示器
class FanLcdDisplay : public LcdDisplay {
public:
    FanLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);

    virtual void SetEmotion(const char* emotion) override;

private:
    void SetupUI();
};

#endif // FAN_LCD_DISPLAY_H
