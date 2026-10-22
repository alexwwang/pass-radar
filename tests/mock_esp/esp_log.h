#pragma once
#ifndef PASS_RADAR_MOCK_ESP_IDF_H
#define PASS_RADAR_MOCK_ESP_IDF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_INVALID_SIZE 0x10C
#define ESP_ERR_INVALID_CRC 0x109

// Keep the tag argument referenced so source-level variables like
// `static const char *TAG` remain "used" under -Wall -Werror.
#define ESP_LOGI(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGW(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGE(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGD(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGV(tag, ...) do { (void)(tag); } while (0)

static inline const char *esp_err_to_name(esp_err_t err) {
    (void)err;
    return "MOCK_ERR";
}

#endif
