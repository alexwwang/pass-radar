#include "radar_capture_condition.h"
#include <math.h>

radar_capture_propagation_id_t radar_capture_condition_propagation(
    radar_capture_link_condition_id_t link) {
    switch (link) {
    case RADAR_CAPTURE_LINK_STATIC_ATTENUATION:
    case RADAR_CAPTURE_LINK_DYNAMIC_ATTENUATION:
        return RADAR_CAPTURE_PROPAGATION_NLOS;
    case RADAR_CAPTURE_LINK_MULTIPATH:
        return RADAR_CAPTURE_PROPAGATION_MULTIPATH;
    default:
        return RADAR_CAPTURE_PROPAGATION_LOS;
    }
}

radar_capture_card_height_t radar_capture_condition_card_height_from_m(
    float height_m) {
    if (!isfinite(height_m) || height_m < 0.0f) {
        return RADAR_CAPTURE_CARD_HEIGHT_UNKNOWN;
    }
    if (height_m < RADAR_CAPTURE_CARD_HEIGHT_LOW_MAX_M) {
        return RADAR_CAPTURE_CARD_HEIGHT_LOW;
    }
    if (height_m < RADAR_CAPTURE_CARD_HEIGHT_CHEST_MAX_M) {
        return RADAR_CAPTURE_CARD_HEIGHT_CHEST;
    }
    return RADAR_CAPTURE_CARD_HEIGHT_OVERHEAD;
}

uint16_t radar_capture_condition_scene_id(
    radar_capture_environment_id_t environment,
    radar_capture_link_condition_id_t link) {
    if (environment > RADAR_CAPTURE_ENV_INDOOR ||
        link > RADAR_CAPTURE_LINK_MULTIPATH) return 0xffffu;
    return (uint16_t)(((uint16_t)environment << 8) | ((uint16_t)link << 4));
}

bool radar_capture_context_option_available(
    radar_capture_environment_id_t environment,
    radar_capture_link_condition_id_t link,
    radar_capture_context_field_t field, uint8_t value) {
    if (environment > RADAR_CAPTURE_ENV_INDOOR ||
        link > RADAR_CAPTURE_LINK_MULTIPATH) return false;

    switch (field) {
    case RADAR_CAPTURE_CONTEXT_SPACE:
        if (value > RADAR_CAPTURE_ROOM_GEOMETRY_OUTDOOR_OPEN) return false;
        return environment == RADAR_CAPTURE_ENV_INDOOR
            ? value != RADAR_CAPTURE_ROOM_GEOMETRY_OUTDOOR_OPEN
            : value == RADAR_CAPTURE_ROOM_GEOMETRY_OUTDOOR_OPEN;
    case RADAR_CAPTURE_CONTEXT_ROOM_SIZE:
        return value <= RADAR_CAPTURE_ROOM_SIZE_LARGE;
    case RADAR_CAPTURE_CONTEXT_OBSTACLE:
        if (value > RADAR_CAPTURE_OBSTRUCTION_METAL) return false;
        return !(environment == RADAR_CAPTURE_ENV_INDOOR &&
                 value == RADAR_CAPTURE_OBSTRUCTION_FOLIAGE);
    case RADAR_CAPTURE_CONTEXT_MATERIAL:
        return value <= RADAR_CAPTURE_MATERIAL_METAL_GLASS;
    case RADAR_CAPTURE_CONTEXT_FURNITURE:
        if (value > RADAR_CAPTURE_FURNITURE_HIGH) return false;
        return !(environment == RADAR_CAPTURE_ENV_OUTDOOR &&
                 value != RADAR_CAPTURE_FURNITURE_LOW);
    case RADAR_CAPTURE_CONTEXT_BOUNDARY:
        return value <= RADAR_CAPTURE_BOUNDARY_PARTITION;
    case RADAR_CAPTURE_CONTEXT_CARD_HEIGHT:
        return value <= RADAR_CAPTURE_CARD_HEIGHT_UNKNOWN;
    case RADAR_CAPTURE_CONTEXT_CARD_POSE:
        return value <= RADAR_CAPTURE_CARD_POSE_MIXED;
    case RADAR_CAPTURE_CONTEXT_BEARING:
        return value <= RADAR_CAPTURE_BEARING_NW;  // UNSET not settable
    default:
        return false;
    }
}

bool radar_capture_context_option_available_for_space(
    radar_capture_environment_id_t environment,
    radar_capture_link_condition_id_t link,
    radar_capture_context_field_t field, uint8_t value,
    radar_capture_room_geometry_t space) {
    if (field == RADAR_CAPTURE_CONTEXT_ROOM_SIZE &&
        space == RADAR_CAPTURE_ROOM_GEOMETRY_OUTDOOR_OPEN) {
        return environment == RADAR_CAPTURE_ENV_OUTDOOR &&
               value == RADAR_CAPTURE_ROOM_SIZE_NOT_APPLICABLE;
    }
    if (!radar_capture_context_option_available(environment, link, field, value)) {
        return false;
    }
    if (field == RADAR_CAPTURE_CONTEXT_ROOM_SIZE) {
        return value <= RADAR_CAPTURE_ROOM_SIZE_LARGE;
    }
    if (space == RADAR_CAPTURE_ROOM_GEOMETRY_OUTDOOR_OPEN &&
        field == RADAR_CAPTURE_CONTEXT_FURNITURE) {
        return value == RADAR_CAPTURE_FURNITURE_LOW;
    }
    return true;
}

bool radar_capture_condition_has_warning(
    radar_capture_environment_id_t environment,
    radar_capture_link_condition_id_t link, uint8_t obstacle,
    uint8_t space) {
    if (environment > RADAR_CAPTURE_ENV_INDOOR ||
        link > RADAR_CAPTURE_LINK_MULTIPATH ||
        obstacle > RADAR_CAPTURE_OBSTRUCTION_METAL ||
        space > RADAR_CAPTURE_ROOM_GEOMETRY_OUTDOOR_OPEN) return false;
    return (link == RADAR_CAPTURE_LINK_LOS &&
            (obstacle == RADAR_CAPTURE_OBSTRUCTION_METAL ||
             obstacle == RADAR_CAPTURE_OBSTRUCTION_WALL)) ||
           (link == RADAR_CAPTURE_LINK_MULTIPATH &&
            obstacle == RADAR_CAPTURE_OBSTRUCTION_NONE) ||
           (link == RADAR_CAPTURE_LINK_STATIC_ATTENUATION &&
            obstacle == RADAR_CAPTURE_OBSTRUCTION_HUMAN_STATIC) ||
           (environment == RADAR_CAPTURE_ENV_OUTDOOR &&
            obstacle == RADAR_CAPTURE_OBSTRUCTION_METAL);
}
