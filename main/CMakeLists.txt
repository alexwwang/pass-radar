# RADAR_SIM_BUILD(passport-sim 页面布局验证)下用空桩替换 NimBLE 真栈,
# 其余源文件与量产构建完全一致。
if(CONFIG_RADAR_SIM_BUILD)
    set(RADAR_BLE_SRC "radar_ble_sim.c")
else()
    set(RADAR_BLE_SRC "radar_ble.c")
endif()

idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "radar_kalman.c"
         "radar_protocol.c"
         "radar_espnow.c"
         "radar_sweep.c"
         "radar_fusion.c"
         "radar_obstruct.c"
         "radar_csi.c"
         "radar_ftm.c"
         "${RADAR_BLE_SRC}"
         "radar_ui.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash app_update mbedtls bt
)
