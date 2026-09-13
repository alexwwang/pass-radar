#include "radar_kalman.h"

void radar_kalman_init(radar_kalman_t *kf, float init_rssi) {
    kf->q = 0.15f;
    kf->r = 4.0f;
    kf->x = init_rssi;
    kf->p = 1.0f;
    kf->k = 0.0f;
}

float radar_kalman_update(radar_kalman_t *kf, float measurement) {
    // 预测步骤
    kf->p = kf->p + kf->q;

    // 更新卡尔曼增益
    kf->k = kf->p / (kf->p + kf->r);

    // 用测量值修正估计
    kf->x = kf->x + kf->k * (measurement - kf->x);
    kf->p = (1.0f - kf->k) * kf->p;

    return kf->x;
}
