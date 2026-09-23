// Host tests for the run-level FTM raw session aggregator (FORMAT_VERSION 4,
// P1 calibration data source). Pure C: compiles radar_capture_session_agg.c
// directly with no IDF dependencies.

#include "../main/radar_capture_session_agg.h"

#include <stdio.h>

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        failures++; \
    } \
} while (0)

static void test_reset_and_empty(void) {
    radar_capture_session_agg_t agg;
    radar_capture_session_agg_reset(&agg);
    CHECK(agg.count == 0u && agg.last_id == 0u, "reset clears state");
    CHECK(!radar_capture_session_agg_refresh(&agg), "empty aggregate is invalid");
    CHECK(agg.ftm_raw_sessions == 0u && agg.ftm_raw_bursts == 0u,
          "empty aggregate reports zero sessions/bursts");
    CHECK(agg.ftm_raw_median_m == 0.0f && agg.ftm_raw_iqr_m == 0.0f,
          "empty aggregate zeroes outputs");
}

static void test_single_session(void) {
    radar_capture_session_agg_t agg;
    radar_capture_session_agg_reset(&agg);
    radar_capture_session_agg_add(&agg, 3u, 1.47f, 12u);
    CHECK(agg.count == 1u && agg.last_id == 3u, "first session is recorded");
    CHECK(radar_capture_session_agg_refresh(&agg), "one session is valid");
    CHECK(agg.ftm_raw_sessions == 1u, "session count is one");
    CHECK(agg.ftm_raw_median_m == 1.47f, "single median passes through");
    CHECK(agg.ftm_raw_iqr_m == 0.0f, "single-sample IQR is zero");
    CHECK(agg.ftm_raw_bursts == 12u, "single burst count passes through");
}

static void test_dedup_and_invalid(void) {
    radar_capture_session_agg_t agg;
    radar_capture_session_agg_reset(&agg);
    radar_capture_session_agg_add(&agg, 1u, 1.50f, 10u);
    radar_capture_session_agg_add(&agg, 1u, 9.99f, 99u); // same id: ignored
    CHECK(agg.count == 1u && agg.ftm_raw_sessions == 0u,
          "duplicate id ignored before refresh");
    radar_capture_session_agg_add(&agg, 0u, 2.0f, 5u); // id 0: ignored
    radar_capture_session_agg_add(&agg, 2u, -1.0f, 5u); // non-positive: ignored
    radar_capture_session_agg_add(&agg, 3u, 2000.0f, 5u); // out of range: ignored
    CHECK(agg.count == 1u, "only the valid session is kept");
    radar_capture_session_agg_add(&agg, 2u, 1.60f, 14u);
    CHECK(agg.count == 2u, "new id after ignored add is accepted");
    CHECK(radar_capture_session_agg_refresh(&agg), "two sessions are valid");
    CHECK(agg.ftm_raw_median_m > 1.549f && agg.ftm_raw_median_m < 1.551f,
          "even-count median is the midpoint");
    CHECK(agg.ftm_raw_bursts == 24u, "burst totals accumulate");
}

static void test_median_and_iqr_parity(void) {
    radar_capture_session_agg_t agg;
    radar_capture_session_agg_reset(&agg);
    // Odd count: medians 1.0..5.0 -> median 3.0, q1=idx1, q3=idx3 -> IQR 2.0.
    for (unsigned i = 0; i < 5u; ++i) {
        radar_capture_session_agg_add(&agg, i + 1u, (float)(i + 1), 8u);
    }
    CHECK(radar_capture_session_agg_refresh(&agg), "odd set is valid");
    CHECK(agg.ftm_raw_median_m == 3.0f, "odd median is the center");
    CHECK(agg.ftm_raw_iqr_m == 2.0f, "odd IQR matches q3-q1");

    radar_capture_session_agg_reset(&agg);
    // Even count: medians 1.0..4.0 -> median 2.5, q1=idx1, q3=idx3 -> IQR 2.0.
    for (unsigned i = 0; i < 4u; ++i) {
        radar_capture_session_agg_add(&agg, i + 1u, (float)(i + 1), 8u);
    }
    CHECK(radar_capture_session_agg_refresh(&agg), "even set is valid");
    CHECK(agg.ftm_raw_median_m == 2.5f, "even median is the midpoint");
    CHECK(agg.ftm_raw_iqr_m == 2.0f, "even IQR matches q3-q1");
}

static void test_unsorted_input_and_weighting(void) {
    radar_capture_session_agg_t agg;
    radar_capture_session_agg_reset(&agg);
    radar_capture_session_agg_add(&agg, 1u, 2.0f, 20u);
    radar_capture_session_agg_add(&agg, 2u, 1.0f, 1u);
    radar_capture_session_agg_add(&agg, 3u, 3.0f, 5u);
    CHECK(radar_capture_session_agg_refresh(&agg), "unsorted set is valid");
    CHECK(agg.ftm_raw_median_m == 2.0f, "median sorts input");
    CHECK(agg.ftm_raw_iqr_m == 2.0f, "IQR sorts input");
    CHECK(agg.ftm_raw_bursts == 26u, "burst weighting sums all sessions");
}

static void test_capacity_overflow(void) {
    radar_capture_session_agg_t agg;
    radar_capture_session_agg_reset(&agg);
    for (unsigned i = 1; i <= RADAR_CAPTURE_AGG_MAX_SESSIONS + 5u; ++i) {
        radar_capture_session_agg_add(&agg, i, 1.0f + (float)i * 0.01f, 2u);
    }
    CHECK(agg.count == RADAR_CAPTURE_AGG_MAX_SESSIONS,
          "aggregate saturates at capacity");
    CHECK(radar_capture_session_agg_refresh(&agg), "full aggregate is valid");
    CHECK(agg.ftm_raw_sessions == RADAR_CAPTURE_AGG_MAX_SESSIONS,
          "session count capped at capacity");
    CHECK(agg.ftm_raw_bursts == RADAR_CAPTURE_AGG_MAX_SESSIONS * 2u,
          "burst total only counts stored sessions");
}

static void test_null_safety(void) {
    radar_capture_session_agg_reset(NULL);
    radar_capture_session_agg_add(NULL, 1u, 1.0f, 1u);
    CHECK(!radar_capture_session_agg_refresh(NULL), "NULL refresh is invalid");
}

int main(void) {
    test_reset_and_empty();
    test_single_session();
    test_dedup_and_invalid();
    test_median_and_iqr_parity();
    test_unsorted_input_and_weighting();
    test_capacity_overflow();
    test_null_safety();
    if (failures) {
        fprintf(stderr, "FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS: capture session aggregate verified\n");
    return 0;
}
