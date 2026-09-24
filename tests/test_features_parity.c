/* Feature-replay parity harness (package A3).
 *
 * Computes radar_obstruct/radar_fusion features for fixed synthetic vectors
 * and prints them as "name value" lines. tests/test_features_parity.py runs
 * this binary and compares every value against tools/model_fit/features.py;
 * max allowed deviation is 1e-4. Vector generators must stay bit-identical
 * with the Python side (integer/half-step values only, no transcendentals).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "radar_fusion.h"
#include "radar_obstruct.h"

#define N 48
#define NOW_MS 7051u  /* 150*(N-1) + 1 */

static void fill_const(float *x) {
    for (unsigned i = 0; i < N; ++i) x[i] = -60.0f;
}

static void fill_ramp(float *x) {
    for (unsigned i = 0; i < N; ++i) x[i] = -50.0f - 0.5f * (float)i;
}

static void fill_mod(float *x) {
    for (unsigned i = 0; i < N; ++i) {
        int v = (int)((i * 7u) % 11u) - 5;
        x[i] = -65.0f + (float)v + (float)(i % 3u) * 0.5f - 0.5f;
    }
}

static void fill_lcg(float *x) {
    uint32_t s = 12345u;
    for (unsigned i = 0; i < N; ++i) {
        s = s * 1103515245u + 12345u;
        x[i] = -90.0f + (float)((s >> 16) % 51u);
    }
}

static void fill_ts(uint32_t *ts) {
    for (unsigned i = 0; i < N; ++i) ts[i] = 150u * i;
}

static void dump_vector(const char *name, const float *x, const uint32_t *ts) {
    float p1 = 0.0f, p10 = 0.0f;
    radar_obstruct_p2p(x, ts, N, NOW_MS, &p1, &p10);
    printf("kfactor.%s %.6f\n", name, (double)radar_obstruct_kfactor_db(x, N));
    printf("uquant80.%s %.6f\n", name,
           (double)radar_obstruct_upper_quantile(x, N, 0.8f));
    printf("uquant50.%s %.6f\n", name,
           (double)radar_obstruct_upper_quantile(x, N, 0.5f));
    printf("p2p1.%s %.6f\n", name, (double)p1);
    printf("p2p10.%s %.6f\n", name, (double)p10);
    printf("meanr3.%s %.6f\n", name,
           (double)radar_obstruct_mean_recent(x, ts, N, NOW_MS, 3000u));
    printf("meanr10.%s %.6f\n", name,
           (double)radar_obstruct_mean_recent(x, ts, N, NOW_MS, 10000u));
}

static void dump_scalars(const float *ramp) {
    static const float raw[] = {3.0f,  3.1f,  2.9f,  3.3f,   0.0f,  -1.0f,
                                3.05f, 1500.0f, 3.02f, 2.95f, 3.2f,  3.15f};
    static const float calib[] = {1.02f, 0.98f, 1.05f, 1.0f, 0.9f};
    static const float hist[] = {2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 4.5f};
    float spread = 0.0f;
    float median = radar_fusion_ftm_filter(raw, 12, &spread);

    printf("excess.0 %.6f\n",
           (double)radar_obstruct_excess_loss_db(3.0f, 2.6f, 2.2f));
    printf("excess.1 %.6f\n",
           (double)radar_obstruct_excess_loss_db(8.0f, 6.5f, 3.5f));
    printf("excess.2 %.6f\n",
           (double)radar_obstruct_excess_loss_db(2.0f, 3.0f, 2.2f));
    printf("excess.3 %.6f\n",
           (double)radar_obstruct_excess_loss_db(1.0f, 0.0f, 2.2f));
    printf("excess.4 %.6f\n",
           (double)radar_obstruct_excess_loss_db(5.0f, 4.9f, 2.2f));
    printf("symdist.0 %.6f\n", (double)radar_fusion_sym_distance(
           -55.0f, -45.0f, RADAR_ENV_OUTDOOR));
    printf("symdist.1 %.6f\n", (double)radar_fusion_sym_distance(
           -55.0f, -45.0f, RADAR_ENV_INDOOR));
    printf("symdist.2 %.6f\n", (double)radar_fusion_sym_distance(
           -55.0f, -45.0f, RADAR_ENV_OBSTRUCTED));
    printf("symdist.3 %.6f\n", (double)radar_fusion_sym_distance(
           -90.0f, -45.0f, RADAR_ENV_OUTDOOR));
    printf("symdist.4 %.6f\n", (double)radar_fusion_sym_distance(
           -127.0f, -45.0f, RADAR_ENV_OUTDOOR));
    printf("symdist.5 %.6f\n", (double)radar_fusion_sym_distance(
           -55.0f, -35.0f, RADAR_ENV_INDOOR));
    printf("symdist.6 %.6f\n", (double)radar_fusion_sym_distance(
           -70.0f, -45.0f, RADAR_ENV_OUTDOOR));
    printf("ftmw.0 %.6f\n", (double)radar_fusion_ftm_weight(3.0f));
    printf("ftmw.1 %.6f\n", (double)radar_fusion_ftm_weight(5.0f));
    printf("ftmw.2 %.6f\n", (double)radar_fusion_ftm_weight(8.0f));
    printf("ftmw.3 %.6f\n", (double)radar_fusion_ftm_weight(25.0f));
    printf("ftmw.4 %.6f\n", (double)radar_fusion_ftm_weight(-1.0f));
    printf("ftmw.5 %.6f\n", (double)radar_fusion_ftm_weight(0.03f));
    printf("ftmw.6 %.6f\n", (double)radar_fusion_ftm_weight(60.0f));
    printf("ftmfilter.median %.6f\n", (double)median);
    printf("ftmfilter.spread %.6f\n", (double)spread);
    printf("ftmfilter.empty %.6f\n",
           (double)radar_fusion_ftm_filter(NULL, 0, NULL));
    printf("ftmoffset.0 %.6f\n",
           (double)radar_fusion_ftm_offset_from_calib(calib, 5));
    printf("ftmoffset.1 %.6f\n",
           (double)radar_fusion_ftm_offset_from_calib(calib, 2));
    printf("ftmoffset.2 %.6f\n",
           (double)radar_fusion_ftm_offset_from_calib(calib, 0));
    printf("applyoff.0 %.6f\n", (double)radar_fusion_ftm_apply_offset(2.5f, 0.3f));
    printf("applyoff.1 %.6f\n", (double)radar_fusion_ftm_apply_offset(0.2f, 0.3f));
    printf("applyoff.2 %.6f\n",
           (double)radar_fusion_ftm_apply_offset(NAN, 0.3f));
    printf("lowquant.0 %.6f\n", (double)radar_fusion_ftm_low_quantile(hist, 6));
    printf("lowquant.1 %.6f\n", (double)radar_fusion_ftm_low_quantile(hist, 0));
    printf("calibref.0 %.6f\n", (double)radar_fusion_calib_ref(-25.0f));
    printf("calibref.1 %.6f\n", (double)radar_fusion_calib_ref(-70.0f));
    printf("calibref.2 %.6f\n", (double)radar_fusion_calib_ref(-45.0f));
    printf("kfactor.short %.6f\n", (double)radar_obstruct_kfactor_db(ramp, 10));
}

int main(void) {
    float const_v[N], ramp[N], mod_v[N], lcg[N];
    uint32_t ts[N];

    fill_ts(ts);
    fill_const(const_v);
    fill_ramp(ramp);
    fill_mod(mod_v);
    fill_lcg(lcg);

    dump_vector("const", const_v, ts);
    dump_vector("ramp", ramp, ts);
    dump_vector("mod", mod_v, ts);
    dump_vector("lcg", lcg, ts);
    dump_scalars(ramp);
    return 0;
}
