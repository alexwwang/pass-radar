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
         "radar_capture_condition.c"
         "radar_capture_codec.c"
         "radar_capture_session_agg.c"
         "radar_capture_batch.c"
         "radar_identity.c"
         "vendor/ed25519/fe.c"
         "vendor/ed25519/ge.c"
         "vendor/ed25519/sc.c"
         "vendor/ed25519/sha512.c"
         "vendor/ed25519/keypair.c"
         "vendor/ed25519/sign.c"
         "radar_upload.c"
         "radar_ftm.c"
         "radar_ble.c"
         "radar_ui.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash app_update mbedtls bt esp_http_server
)

# ED25519_NO_SEED:种子的唯一来源是 MAC 派生,禁用 vendor 库的 urandom
# 取种路径(嵌入式无 /dev/urandom,留着只会引入未用符号)。
target_compile_definitions(${COMPONENT_LIB} PRIVATE ED25519_NO_SEED)
