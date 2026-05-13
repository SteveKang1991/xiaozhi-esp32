#ifndef __LIGHTAM_CONTROLLER_H__
#define __LIGHTAM_CONTROLLER_H__

#include "mcp_server.h"


class LightAMController {
private:
    bool power_ = true;
    gpio_num_t gpio_num_;

public:
    LightAMController(gpio_num_t gpio_num) : gpio_num_(gpio_num) {
        gpio_config_t config = {
            .pin_bit_mask = (1ULL << gpio_num_),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&config));
        gpio_set_level(gpio_num_, 0);

        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.lamp.get_state", "获取灯光的开关状态。当用户询问灯/灯光/氛围灯/背后灯是否打开，会调用此方法获取开关状态", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            return power_ ? "{\"power\": true}" : "{\"power\": false}";
        });

        mcp_server.AddTool("self.lamp.turn_on", "打开灯/灯光/氛围灯/背后灯", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            power_ = true;
            gpio_set_level(gpio_num_, 0);
            return true;
        });

        mcp_server.AddTool("self.lamp.turn_off", "关闭灯/灯光/氛围灯/背后灯", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            power_ = false;
            gpio_set_level(gpio_num_, 1);
            return true;
        });
    }
};


#endif // __LIGHTAM_CONTROLLER_H__

/*
#include "mcp_server.h"
#include <esp_log.h>

#define TAG "绿灯事件："

class Green_Lamp  {          // 1. 类名改成 Green_Lamp 
private:
    bool power_ = false;
    gpio_num_t gpio_num_;

public:
    explicit Green_Lamp (gpio_num_t gpio_num) : gpio_num_(gpio_num) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << gpio_num_),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        gpio_set_level(gpio_num_, 0);       //初始化为低电平

        //2. 把 MCP 工具名改成绿灯相关 
        
        auto& server = McpServer::GetInstance();
        server.AddTool("绿灯.获取开关状态", "返回绿灯的开/关状态",      // 工具名称   , 工具描述    
                       PropertyList(), [this](const PropertyList&) {

                           ESP_LOGW(TAG, "获取到了绿灯的当前状态，当前状态为%s", power_ ? "开" : "关");     //日志记录
                           return power_ ? "{\"灯光状态：\":绿灯是开着的！}" : "{\"灯光状态：\":绿灯是关着的！}";       //返回状态
                       
                       });
   
        server.AddTool("绿灯.打开", "打开绿灯",     // 工具名称   , 工具描述
                       PropertyList(), [this](const PropertyList&) {
                           power_ = true;
                           gpio_set_level(gpio_num_, 1);    //设置为高电平
                           ESP_LOGW(TAG, "已打开绿灯！");   //日志记录
                           return true;     //返回告诉小智执行成功！
                       });

        server.AddTool("绿灯.关闭", "关闭绿灯",     // 工具名称   , 工具描述
                       PropertyList(), [this](const PropertyList&) {
                           power_ = false;
                           gpio_set_level(gpio_num_, 0);    //设置为低电平
                           ESP_LOGW(TAG, "已关闭绿灯！");    //日志记录
                           return true;     //返回告诉小智执行成功！
                       });
    }
};
 */