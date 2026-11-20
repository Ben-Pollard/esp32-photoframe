// Pins the weather data block: the exact text the prompt writer receives.
//
// This is the one place in the weather path where a silent edit changes what the
// model sees, and the block is authored in ai-lab/scenarios.json, so it is worth
// pinning. These expectations were produced by the lab's format_data_block() for
// the same raw values (clear_cold and undefined_code_100), so the device and the
// lab agreeing is a checked fact rather than an intention.

#include <gtest/gtest.h>

#include "weather.h"

// "°C" as UTF-8 bytes, spelled out so this file's encoding cannot mangle it.
#define DEG "\xC2\xB0"

static weather_day_t full_day()
{
    weather_day_t d = {};
    return d;
}

TEST(WeatherBlock, MatchesLabForFullData)
{
    // Scenario clear_cold: 2026-11-16, code 1.
    weather_day_t d = full_day();
    snprintf(d.day, sizeof(d.day), "2026-11-16");
    d.weekday = 1;  // Monday, 0 = Sunday
    d.weather_code = 1;
    d.has_code = true;
    snprintf(d.condition, sizeof(d.condition), "%s", "Mainly clear");
    d.has_condition = true;
    d.temp_max_c = 6.4;
    d.temp_min_c = -1.2;
    d.has_temp_max = d.has_temp_min = true;
    d.cloud_cover_pct = 8;
    d.has_cloud = true;
    d.sunshine_s = 28700;
    d.daylight_s = 30900;
    d.has_sunshine = d.has_daylight = true;
    d.precip_mm = 0.0;
    d.precip_hours = 0;
    d.has_precip_hours = true;
    d.wind_max_kmh = 7.3;
    d.gust_max_kmh = 15.8;
    d.has_gust = true;
    d.snow_cm = 0;
    d.has_snow = true;

    char out[640];
    weather_day_summary(&d, out, sizeof(out));

    const char *want =
        "date           Monday 16th November 2026\n"
        "condition      Mainly clear (wmo code 1)\n"
        "temperature    high 6.4 " DEG "C, low -1.2 " DEG "C\n"
        "cloud cover    8 % mean\n"
        "sunshine       8.0 h of 8.6 h daylight (93%)\n"
        "precipitation  0.0 mm over 0 h\n"
        "snowfall       0 cm\n"
        "wind           max 7.3 km/h, gusts to 15.8 km/h";

    EXPECT_STREQ(want, out);
}

TEST(WeatherBlock, UndefinedCodeReportsNoCondition)
{
    // Scenario undefined_code_100: a code Open-Meteo does not define must never
    // be described by guesswork.
    weather_day_t d = full_day();
    snprintf(d.day, sizeof(d.day), "2026-12-20");
    d.weekday = 0;  // Sunday
    d.weather_code = 100;
    d.has_code = true;
    d.temp_max_c = 8.0;
    d.temp_min_c = 3.3;
    d.has_temp_max = d.has_temp_min = true;
    d.cloud_cover_pct = 60;
    d.has_cloud = true;
    d.sunshine_s = 15000;
    d.daylight_s = 28300;
    d.has_sunshine = d.has_daylight = true;
    d.precip_mm = 0.0;
    d.precip_hours = 0;
    d.has_precip_hours = true;
    d.wind_max_kmh = 9.9;
    d.gust_max_kmh = 19.8;
    d.has_gust = true;
    d.snow_cm = 0;
    d.has_snow = true;

    char out[640];
    weather_day_summary(&d, out, sizeof(out));

    const char *want =
        "date           Sunday 20th December 2026\n"
        "condition      not reported -- wmo code 100 is undefined\n"
        "temperature    high 8.0 " DEG "C, low 3.3 " DEG "C\n"
        "cloud cover    60 % mean\n"
        "sunshine       4.2 h of 7.9 h daylight (53%)\n"
        "precipitation  0.0 mm over 0 h\n"
        "snowfall       0 cm\n"
        "wind           max 9.9 km/h, gusts to 19.8 km/h";

    EXPECT_STREQ(want, out);
}

TEST(WeatherBlock, ShapeIsFixedWhenFieldsAreMissing)
{
    // The block must not lose lines when readings are absent. An omitted field
    // used to make a dry day indistinguishable from a missing reading, and it
    // let the firmware decide what the model was allowed to see.
    weather_day_t d = full_day();
    snprintf(d.day, sizeof(d.day), "2026-11-16");
    d.weekday = 1;

    char out[640];
    weather_day_summary(&d, out, sizeof(out));

    int lines = 0;
    for (const char *p = out; *p; p++) {
        lines += (*p == '\n');
    }
    EXPECT_EQ(7, lines) << "expected 8 lines, i.e. 7 newlines:\n" << out;
    EXPECT_NE(std::string::npos, std::string(out).find("date           Monday 16th November 2026"));
    EXPECT_NE(std::string::npos, std::string(out).find("condition      not reported"));
    EXPECT_NE(std::string::npos, std::string(out).find("cloud cover    not reported"));
    EXPECT_NE(std::string::npos, std::string(out).find("sunshine       not reported"));
    EXPECT_NE(std::string::npos, std::string(out).find("snowfall       not reported"));
    EXPECT_NE(std::string::npos, std::string(out).find("wind           not reported"));
}

TEST(WeatherBlock, OrdinalsHandleTheEleventhTwelfthThirteenthTrap)
{
    // A naive suffix rule yields 11st/12th-adjacent nonsense, and this string is
    // on a prompt every day for a year.
    struct {
        int dom;
        const char *suffix;
    } cases[] = {
        {1, "1st"},   {2, "2nd"},   {3, "3rd"},   {4, "4th"},   {11, "11th"},
        {12, "12th"}, {13, "13th"}, {21, "21st"}, {22, "22nd"}, {23, "23rd"},
        {31, "31st"},
    };
    for (auto &c : cases) {
        weather_day_t d = full_day();
        // November 2026: 1st is a Sunday, so weekday/date parse cleanly.
        snprintf(d.day, sizeof(d.day), "2026-11-%02d", c.dom);
        d.weekday = 0;
        char out[640];
        weather_day_summary(&d, out, sizeof(out));
        std::string line(out);
        line = line.substr(0, line.find('\n'));
        EXPECT_NE(std::string::npos, line.find(c.suffix))
            << "day " << c.dom << " produced: " << line;
    }
}
