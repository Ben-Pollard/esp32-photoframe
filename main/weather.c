#include "weather.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "config_manager.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "weather";

// Open-Meteo answers this query in ~800 B; the ceiling is headroom for extra
// daily fields rather than a working limit.
#define WEATHER_RESPONSE_MAX 8192

// WMO interpretation codes exactly as Open-Meteo publishes them. The API returns
// only the numeric code -- no description -- so the mapping is ours to carry, and
// this table is a verbatim transcription of "Weather code descriptions as JSON"
// from the Open-Meteo docs. The strings are stored unedited rather than reworded
// for sentence flow: inventing our own phrasing is how drift starts, and a
// verbatim copy can be diffed against the source and shown to be clean.
// Canonical copy: docs/wmo_codes_openmeteo.json
static const struct {
    int code;
    const char *text;
} WMO[] = {
    {0,  "Clear sky"},
    {1,  "Mainly clear"},
    {2,  "Partly cloudy"},
    {3,  "Overcast"},
    {45, "Fog"},
    {48, "Depositing rime fog"},
    {51, "Light drizzle"},
    {53, "Moderate drizzle"},
    {55, "Dense drizzle"},
    {56, "Light freezing drizzle"},
    {57, "Dense freezing drizzle"},
    {61, "Slight rain"},
    {63, "Moderate rain"},
    {65, "Heavy rain"},
    {66, "Light freezing rain"},
    {67, "Heavy freezing rain"},
    {71, "Slight snowfall"},
    {73, "Moderate snowfall"},
    {75, "Heavy snowfall"},
    {77, "Snow grains"},
    {80, "Slight rain showers"},
    {81, "Moderate rain showers"},
    {82, "Violent rain showers"},
    {85, "Slight snow showers"},
    {86, "Heavy snow showers"},
    {95, "Thunderstorm"},
    {96, "Thunderstorm with slight hail"},
    {97, "Heavy thunderstorm"},
    {99, "Thunderstorm with heavy hail"},
};

// Returns NULL when the code is not one Open-Meteo defines. Callers must treat
// that as "no description available" rather than substituting a guess: the
// fallback here used to be "unremarkable weather", which reported a heavy
// thunderstorm as nothing noteworthy.
const char *weather_wmo_description(int code)
{
    for (size_t i = 0; i < sizeof(WMO) / sizeof(WMO[0]); i++) {
        if (WMO[i].code == code) {
            return WMO[i].text;
        }
    }
    return NULL;
}

// Tomohiko Sakamoto's algorithm: 0 = Sunday. Used instead of mktime/timegm so
// the weekday comes from the server's date string and not from our own clock,
// which may not be synced.
static int weekday_of(int y, int m, int d)
{
    static const int offset[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) {
        y -= 1;
    }
    return (y + y / 4 - y / 100 + y / 400 + offset[m - 1] + d) % 7;
}

// "2026-11-06T07:03" -> "07:03" was used for sunrise/sunset, which the agreed
// field set no longer pulls: daylight_duration carries the picturable part.
// Removed rather than left unused.

// Reads a one-element daily array into a double. Missing or null leaves *val and
// returns false so the caller can leave the field out of the summary.
static bool daily_number(const cJSON *daily, const char *field, double *val)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(daily, field);
    if (!cJSON_IsArray(arr)) {
        return false;
    }
    const cJSON *first = cJSON_GetArrayItem(arr, 0);
    if (!cJSON_IsNumber(first)) {
        return false;
    }
    *val = first->valuedouble;
    return true;
}

static esp_err_t fetch_json(const char *url, char **out, size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = WEATHER_TIMEOUT_MS,
        .buffer_size = 2048,
        .user_agent = WEATHER_USER_AGENT,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    char *buf = malloc(WEATHER_RESPONSE_MAX);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (total + 1 < WEATHER_RESPONSE_MAX) {
        int n = esp_http_client_read(client, buf + total, (int)(WEATHER_RESPONSE_MAX - total - 1));
        if (n < 0) {
            ESP_LOGE(TAG, "Failed to read forecast body");
            free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
    }
    buf[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        // The body of an Open-Meteo error is small and worth surfacing verbatim.
        ESP_LOGE(TAG, "Forecast request failed: HTTP %d, %u bytes: %.*s", status, (unsigned)total,
                 (int)(total > 200 ? 200 : total), buf);
        free(buf);
        return ESP_FAIL;
    }

    *out = buf;
    *out_len = total;
    return ESP_OK;
}

// Renders the day's temperature as a phrase that only claims what was actually
// reported. Open-Meteo can send null for either bound, and printing an absent one
// as 0.0 would tell the model it was freezing. Public so the log and the summary
// cannot disagree about the same forecast.
void weather_temp_text(const weather_day_t *d, char *out, size_t cap)
{
    if (d == NULL || out == NULL || cap == 0) {
        return;
    }
    if (d->has_temp_min && d->has_temp_max) {
        snprintf(out, cap, "%.1f to %.1f", d->temp_min_c, d->temp_max_c);
    } else if (d->has_temp_max) {
        snprintf(out, cap, "up to %.1f", d->temp_max_c);
    } else if (d->has_temp_min) {
        snprintf(out, cap, "from %.1f", d->temp_min_c);
    } else {
        snprintf(out, cap, "unknown");
    }
}

esp_err_t weather_get_today(weather_day_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    double lat = config_manager_get_weather_latitude();
    double lon = config_manager_get_weather_longitude();
    // Only non-finite is treated as unusable. Testing for 0 as "unset" would be
    // wrong twice over: the getters already fall back to a compiled default so an
    // unset value never reaches here, and 0 is a legitimate coordinate -- a frame
    // on the equator or the prime meridian would fail every cycle forever.
    if (!isfinite(lat) || !isfinite(lon)) {
        ESP_LOGE(TAG, "Location is not a finite coordinate (weather_latitude/weather_longitude)");
        return ESP_ERR_INVALID_STATE;
    }

    // forecast_days=1 -> index 0 is today in the zone resolved from these
    // coordinates. Nothing here depends on our own calendar.
    //
    // This field list is the agreed one: everything here is feasibly picturable
    // or gives the day its shape. Deliberately absent -- precipitation
    // probability (an uncertainty, not a thing you can draw), relative humidity
    // (invisible at any value), sunrise/sunset (daylight_duration carries the
    // picturable part) and wind direction (a picture may lean any way).
    char url[512];
    snprintf(url, sizeof(url),
             "%s?latitude=%.5f&longitude=%.5f"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
             "cloud_cover_mean,sunshine_duration,daylight_duration,"
             "precipitation_sum,precipitation_hours,"
             "wind_speed_10m_max,wind_gusts_10m_max,snowfall_sum"
             "&forecast_days=1&timezone=auto",
             OPEN_METEO_FORECAST_URL, lat, lon);

    char *body = NULL;
    size_t len = 0;
    esp_err_t err = fetch_json(url, &body, &len);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root) {
        ESP_LOGE(TAG, "Forecast response was not valid JSON");
        return ESP_FAIL;
    }

    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (!cJSON_IsObject(daily)) {
        ESP_LOGE(TAG, "Forecast response had no \"daily\" object");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    weather_day_t d = {0};
    d.latitude = lat;
    d.longitude = lon;

    const cJSON *day = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(daily, "time"), 0);
    if (cJSON_IsString(day)) {
        snprintf(d.day, sizeof(d.day), "%s", cJSON_GetStringValue(day));
        int y = 0, m = 0, dom = 0;
        if (sscanf(d.day, "%4d-%2d-%2d", &y, &m, &dom) == 3 && m >= 1 && m <= 12) {
            d.weekday = weekday_of(y, m, dom);
        }
    }

    // Every field is read straight into its own destination rather than through
    // one shared scratch variable. Open-Meteo sends null for a daily field it has
    // no data for, and daily_number() deliberately leaves its output untouched in
    // that case -- so a shared scratch would let one missing field report its
    // neighbour's number. Concretely: a missing precipitation_sum used to leave
    // the temperature in place, and the summary then told the model it had rained
    // 12.4 mm on a 12.4 degree day. The zero-init above makes each absent field
    // honestly zero instead.
    double code = 0;
    if (daily_number(daily, "weather_code", &code)) {
        d.weather_code = (int)code;
        d.has_code = true;
        const char *what = weather_wmo_description((int)code);
        if (what != NULL) {
            snprintf(d.condition, sizeof(d.condition), "%s", what);
            d.has_condition = true;
        } else {
            // Undefined code: report it and leave the condition absent rather
            // than describing it. The fields we do understand still stand.
            ESP_LOGE(TAG, "weather_code %d is not one Open-Meteo defines -- no condition reported",
                     (int)code);
        }
    }
    if (daily_number(daily, "temperature_2m_max", &d.temp_max_c)) {
        d.has_temp_max = true;
    }
    if (daily_number(daily, "temperature_2m_min", &d.temp_min_c)) {
        d.has_temp_min = true;
    }
    daily_number(daily, "precipitation_sum", &d.precip_mm);

    double hours = 0;
    if (daily_number(daily, "precipitation_hours", &hours)) {
        d.precip_hours = (int)(hours + 0.5);
        d.has_precip_hours = true;
    }
    daily_number(daily, "wind_speed_10m_max", &d.wind_max_kmh);

    if (daily_number(daily, "wind_gusts_10m_max", &d.gust_max_kmh)) {
        d.has_gust = true;
    }
    double cloud = 0;
    if (daily_number(daily, "cloud_cover_mean", &cloud)) {
        d.cloud_cover_pct = (int)(cloud + 0.5);
        d.has_cloud = true;
    }
    if (daily_number(daily, "sunshine_duration", &d.sunshine_s)) {
        d.has_sunshine = true;
    }
    if (daily_number(daily, "daylight_duration", &d.daylight_s)) {
        d.has_daylight = true;
    }
    if (daily_number(daily, "snowfall_sum", &d.snow_cm)) {
        d.has_snow = true;
    }

    cJSON_Delete(root);

    if (!d.has_condition && !d.has_temp_max && !d.has_temp_min) {
        ESP_LOGE(TAG, "Forecast carried neither a weather code nor temperatures");
        return ESP_FAIL;
    }

    *out = d;
    char temps[32];
    weather_temp_text(&d, temps, sizeof(temps));
    ESP_LOGI(TAG, "Forecast for %s: %s, %s C, %.1f mm over %d h, cloud %d%%, sunshine %.1f/%.1f h",
             d.day, d.has_condition ? d.condition : "unknown", temps, d.precip_mm,
             d.precip_hours, d.cloud_cover_pct, d.sunshine_s / 3600.0, d.daylight_s / 3600.0);
    return ESP_OK;
}
