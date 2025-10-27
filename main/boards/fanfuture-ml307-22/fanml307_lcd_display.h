#ifndef FAN_ML307_LCD_DISPLAY_H
#define FAN_ML307_LCD_DISPLAY_H

#include "lcd_display.h"

// FANML307 LCD显示器
class FanML307LcdDisplay : public LcdDisplay {
public:
    FanML307LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);

    virtual void SetEmotion(const char* emotion) override;

private:
    void SetupUI();
};

#endif // FAN_ML307_LCD_DISPLAY_H
