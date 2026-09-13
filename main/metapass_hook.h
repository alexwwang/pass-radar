// main/metapass_hook.h —— 子固件适配 meta-pass 启动器的最小 hook(header-only)。
//
// 用法(子固件):
//   1. 启动自检通过后调用 metapass_mark_valid()  → 跨重启常驻,否则下次重启自动回启动器。
//   2. 在按键处理里响应 OK 键的 BSP_BTN_LONG2 → 调 metapass_return_to_launcher() 退回启动器。
//      (注意:LONG2 触发前会先触发一次 LONG,应用内"返回"与"退回启动器"会先后发生;
//       设计应用交互时把 LONG 安排为可安全先发生的动作,如返回上一页。)
//
// 只有返回启动器和常驻两个能力需要适配;不适配的子固件表现为"试运行"(重启即回启动器)。
#pragma once

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

// 自检通过后调用:把当前运行的子固件标记为有效,取消自动回滚。
// 返回 ESP_OK 表示已常驻;其他值表示当前不处于待验证态(如直接从 factory 调试运行),可忽略。
static inline esp_err_t metapass_mark_valid(void)
{
    return esp_ota_mark_app_valid_cancel_rollback();
}

// 把启动分区切回 factory(meta-pass 启动器)并立即重启,不返回。
static inline void metapass_return_to_launcher(void)
{
    const esp_partition_t *factory =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory) {
        esp_ota_set_boot_partition(factory);
    }
    esp_restart();
}
