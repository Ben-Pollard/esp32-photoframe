#ifndef WEATHER_H
#define WEATHER_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// One calendar day of forecast, fetched from Open-Meteo.
//
// The day is index 0 of the server's daily arrays, resolved in the timezone that
// Open-Meteo derives from our coordinates (timezone=auto). Because the server
// picks the day, the result is correct even if our own clock has not synced yet
// -- SNTP only has to be right about the *time of day* we run, not the date.
//
// This is deliberately a bag of numbers plus one descriptive string: its only
// consumer is the prompt writer (ai_prompt.c), which wants plain language rather
// than codes it would have to interpret again.
typedef struct {
    double latitude;   // echoed back from the config the fetch used
    double longitude;
    char day[16];   // "2026-11-06", as the server labelled it
    int weekday;    // 0 = Sunday .. 6 = Saturday, computed from `day`
    int weather_code;    // raw WMO code, kept so the block can cite it
    bool has_code;
    char condition[48];  // the code expanded verbatim, or absent if undefined
    double temp_max_c;
    double temp_min_c;
    int cloud_cover_pct;   // daily mean, 0..100
    double sunshine_s;     // seconds -- converted to hours when rendered
    double daylight_s;     // seconds
    double precip_mm;
    int precip_hours;      // how long precipitation fell, in hours
    double wind_max_kmh;
    double gust_max_kmh;
    double snow_cm;
    bool has_condition;   // the fields every caller needs
    bool has_temp_max;
    bool has_temp_min;    // tracked separately: the daily arrays carry nulls
    bool has_cloud;
    bool has_sunshine;
    bool has_daylight;
    bool has_precip_hours;
    bool has_gust;
    bool has_snow;
} weather_day_t;

/**
 * @brief Render the day's temperature as a phrase that only claims what the
 *        forecast actually reported ("1.2 to 8.4", "up to 8.4", "unknown").
 */
void weather_temp_text(const weather_day_t *d, char *out, size_t cap);

/**
 * @brief Fetch today's forecast for the configured coordinates.
 *
 * @param out  Filled on ESP_OK; left untouched otherwise.
 * @return ESP_OK, ESP_ERR_INVALID_STATE if no location is configured,
 *         ESP_ERR_HTTP_* / ESP_FAIL on network or parse failure.
 */
esp_err_t weather_get_today(weather_day_t *out);

/**
 * @brief Render one day as the weather data block for the prompt writer.
 *
 * Emits exactly the same text as the lab's format_data_block(): fixed labels,
 * units that need no arithmetic (hours rather than seconds), and explicit zeros
 * so a dry day cannot be mistaken for a missing reading. The format is pinned by
 * host_tests/test_weather_block.cpp against the lab's output.
 */
void weather_day_summary(const weather_day_t *d, char *out, size_t cap);

/**
 * @brief Render the on-panel label for one day, e.g. "20th Nov | Clear sky".
 *
 * Deliberately dates the picture it is drawn on rather than "today": if a
 * generation fails and the frame still shows yesterday's image, the label keeps
 * saying yesterday, so the staleness is visible instead of hidden.
 */
void weather_label_text(const weather_day_t *d, char *out, size_t cap);

/**
 * @brief Expand a WMO weather interpretation code (Open-Meteo's weather_code).
 * @return The description exactly as Open-Meteo publishes it, or NULL if the
 *         code is not one Open-Meteo defines. Callers must not substitute a
 *         guess for NULL -- absent is honest, invented is not.
 */
const char *weather_wmo_description(int code);

#endif
