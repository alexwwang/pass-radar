#pragma once

#include "radar_capture_batch.h"
#include <stdbool.h>
#include <stdint.h>

bool radar_capture_context_option_available(
    radar_capture_environment_id_t environment,
    radar_capture_link_condition_id_t link,
    radar_capture_context_field_t field, uint8_t value);

bool radar_capture_condition_has_warning(
    radar_capture_environment_id_t environment,
    radar_capture_link_condition_id_t link, uint8_t obstacle,
    uint8_t space);

uint16_t radar_capture_condition_scene_id(
    radar_capture_environment_id_t environment,
    radar_capture_link_condition_id_t link);
radar_capture_propagation_id_t radar_capture_condition_propagation(
    radar_capture_link_condition_id_t link);
radar_capture_card_height_t radar_capture_condition_card_height_from_m(
    float height_m);
