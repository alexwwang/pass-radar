#pragma once
#ifndef PASS_RADAR_MOCK_ESP_ERR_H
#define PASS_RADAR_MOCK_ESP_ERR_H

// Host mock: esp_err_t 与错误码定义集中在 esp_log.h,这里只做转发,
// 保持固件源码 #include "esp_err.h" 在宿主机编译时可用。
#include "esp_log.h"

#endif
