# The simulator loads the same production firmware. Unsupported radio
# peripherals must degrade at runtime; they are never replaced at compile time.
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
         "radar_diag_persist.c"
         "radar_capture.c"
         "radar_capture_codec.c"
         "radar_ftm.c"
         "radar_ble.c"
         "radar_ui.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash app_update mbedtls bt
)
