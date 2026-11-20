#include "config_manager.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board_hal.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "storage.h"

static const char *TAG = "config_manager";

// General
static char device_name[DEVICE_NAME_MAX_LEN] = {0};
static char tz_string[TIMEZONE_MAX_LEN] = {0};
static display_orientation_t display_orientation = DISPLAY_ORIENTATION_LANDSCAPE;
static int display_rotation_deg = BOARD_HAL_DISPLAY_ROTATION_DEG;
static char wifi_ssid[WIFI_SSID_MAX_LEN] = {0};
static char wifi_password[WIFI_PASS_MAX_LEN] = {0};

// Advanced network settings (collapsed section in the UI): custom NTP server,
// static IP instead of DHCP, and DNS override
static char ntp_server[NTP_SERVER_MAX_LEN] = {0};
static ip_mode_t ip_mode = IP_MODE_DHCP;
static char static_ip[IP_ADDR_STR_MAX_LEN] = {0};
static char static_netmask[IP_ADDR_STR_MAX_LEN] = {0};
static char static_gateway[IP_ADDR_STR_MAX_LEN] = {0};
static char dns_server[IP_ADDR_STR_MAX_LEN] = {0};

// Generation settings: the weather service's location, the endpoint/model pair,
// and the owner's standing creative instructions, which the UI edits by hand.
static double weather_latitude = DEFAULT_WEATHER_LATITUDE;
static double weather_longitude = DEFAULT_WEATHER_LONGITUDE;
static char ai_url[AI_URL_MAX_LEN] = {0};
static char ai_image_model[AI_MODEL_MAX_LEN] = {0};
static int ai_model_fails = 0;
static char ai_direction[AI_DIRECTION_MAX_LEN] = {0};

// Auto Rotate
static bool auto_rotate_enabled = false;
static char cron_rules_store[MAX_CRON_RULES][CRON_RULE_MAX_LEN] = {{0}};
static int cron_rule_count = 0;

static rotation_mode_t rotation_mode =
    ROTATION_MODE_STORAGE;  // Default, will be validated during init

// Auto Rotate - SDCARD
static sd_rotation_mode_t sd_rotation_mode = SD_ROTATION_RANDOM;
static int32_t last_index = -1;

// Auto Rotate - URL
static char image_url[IMAGE_URL_MAX_LEN] = {0};
// The pinned CA certificate is read by the rotation task while the HTTP task
// can replace it, so pointer, length and generation move together under a
// lock; nothing outside this file ever sees the buffer itself.
static SemaphoreHandle_t ca_cert_lock;
static uint8_t *ca_cert_der = NULL;  // Heap-allocated DER certificate
static size_t ca_cert_der_len = 0;
static uint32_t ca_cert_generation = 0;  // Bumped by every change, see config_manager.h
static char access_token[ACCESS_TOKEN_MAX_LEN] = {0};
static char http_password[HTTP_PASSWORD_MAX_LEN] = {0};
static char http_header_key[HTTP_HEADER_KEY_MAX_LEN] = {0};
static char http_header_value[HTTP_HEADER_VALUE_MAX_LEN] = {0};
static bool save_downloaded_images = false;
static char image_etag[HTTP_ETAG_MAX_LEN] = {0};

// Home Assistant
static char ha_url[HA_URL_MAX_LEN] = {0};

// AI API Keys
static char ai_api_key[AI_API_KEY_MAX_LEN] = {0};
static char ai_writer_url[AI_WRITER_URL_MAX_LEN] = {0};
static char ai_writer_model[AI_WRITER_MODEL_MAX_LEN] = {0};
static char ai_place[AI_PLACE_MAX_LEN] = {0};
static char ai_template[AI_TEMPLATE_MAX_LEN] = {0};
static size_t ai_template_len = 0;  // 0 = no saved template, use the default
static char ai_model_order[AI_MODEL_ORDER_MAX_LEN] = {0};
static int ai_fail_streak = 0;

// Power
static bool deep_sleep_enabled = true;     // Enabled by default
static bool low_battery_indicator = true;  // Badge on the picture when the battery is low
static int low_battery_threshold = LOW_BATTERY_THRESHOLD_DEFAULT;

// Debugging
static bool debug_log_enabled = false;

// Config sync
static int64_t config_last_updated = 0;

// ----------------------------------------------------------------------------
// Cron schedule helpers
// ----------------------------------------------------------------------------

// Fill cron_rules_store from a '\n'-separated joined string (skips empty and
// over-long entries, caps at MAX_CRON_RULES).
static void cron_load_from_joined(const char *joined)
{
    cron_rule_count = 0;
    if (!joined) {
        return;
    }
    const char *p = joined;
    while (*p && cron_rule_count < MAX_CRON_RULES) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t) (nl - p) : strlen(p);
        if (len > 0 && len < CRON_RULE_MAX_LEN) {
            memcpy(cron_rules_store[cron_rule_count], p, len);
            cron_rules_store[cron_rule_count][len] = '\0';
            cron_rule_count++;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
}

// Persist the current cron_rules_store to NVS as a '\n'-joined string.
static void cron_persist(void)
{
    char joined[MAX_CRON_RULES * CRON_RULE_MAX_LEN];
    joined[0] = '\0';
    size_t off = 0;
    for (int i = 0; i < cron_rule_count; i++) {
        int n = snprintf(joined + off, sizeof(joined) - off, "%s%s", i ? "\n" : "",
                         cron_rules_store[i]);
        if (n < 0 || (size_t) n >= sizeof(joined) - off) {
            break;
        }
        off += n;
    }

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        if (cron_rule_count > 0) {
            nvs_set_str(nvs_handle, NVS_ROTATE_CRON_KEY, joined);
        } else {
            nvs_erase_key(nvs_handle, NVS_ROTATE_CRON_KEY);
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

// Convert a legacy rotation interval (seconds) into a single cron expression.
// Mirrors the documented best-effort mapping; falls back to hourly.
static void cron_from_legacy_interval(int seconds, char *out, size_t out_len)
{
    if (seconds >= 3600 && seconds % 3600 == 0) {
        int hours = seconds / 3600;
        if (hours <= 1) {
            snprintf(out, out_len, "0 * *");
        } else if (hours >= 24) {
            snprintf(out, out_len, "0 0 *");
        } else {
            snprintf(out, out_len, "0 */%d *", hours);
        }
    } else if (seconds >= 60 && 3600 % seconds == 0) {
        snprintf(out, out_len, "*/%d * *", seconds / 60);
    } else {
        // Not cleanly expressible as cron (e.g. 90 min) — approximate to hourly.
        snprintf(out, out_len, "0 * *");
    }
}

esp_err_t config_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing config manager");
    ca_cert_lock = xSemaphoreCreateMutex();
    if (!ca_cert_lock) {
        return ESP_ERR_NO_MEM;
    }

    // Rotation-schedule load is resolved after the read-only NVS handle closes
    // (migration / default seeding may need a read-write handle).
    char cron_buf[MAX_CRON_RULES * CRON_RULE_MAX_LEN] = {0};
    int32_t legacy_interval = 0;
    bool migrate_legacy_interval = false;
    bool seed_default_cron = true;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        // General
        size_t device_name_len = DEVICE_NAME_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_DEVICE_NAME_KEY, device_name, &device_name_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded device name from NVS: %s", device_name);
        } else {
            strncpy(device_name, DEFAULT_DEVICE_NAME, DEVICE_NAME_MAX_LEN - 1);
            device_name[DEVICE_NAME_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No device name in NVS, using default: %s", device_name);
        }

        size_t tz_len = TIMEZONE_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_TIMEZONE_KEY, tz_string, &tz_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded timezone from NVS: %s", tz_string);
        } else {
            strncpy(tz_string, DEFAULT_TIMEZONE, TIMEZONE_MAX_LEN - 1);
            tz_string[TIMEZONE_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No timezone in NVS, using default: %s", tz_string);
        }

        size_t http_password_len = HTTP_PASSWORD_MAX_LEN;
        esp_err_t pw_err =
            nvs_get_str(nvs_handle, NVS_HTTP_PASSWORD_KEY, http_password, &http_password_len);
        if (pw_err != ESP_OK) {
            // Not found is the normal "auth off" default. Any other error
            // also leaves auth off rather than failing closed: the routes that
            // could repair it, factory reset included, sit behind the same
            // gate, so a lockout here would need a reflash to undo.
            if (pw_err != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGE(TAG, "Failed to read HTTP API password (%s); auth is OFF",
                         esp_err_to_name(pw_err));
            }
            http_password[0] = '\0';
        }

        size_t ntp_server_len = NTP_SERVER_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_NTP_SERVER_KEY, ntp_server, &ntp_server_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded NTP server from NVS: %s", ntp_server);
        } else {
            strncpy(ntp_server, DEFAULT_NTP_SERVER, NTP_SERVER_MAX_LEN - 1);
            ntp_server[NTP_SERVER_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No NTP server in NVS, using default: %s", ntp_server);
        }

        // Advanced network settings
        uint8_t stored_ip_mode = IP_MODE_DHCP;
        if (nvs_get_u8(nvs_handle, NVS_IP_MODE_KEY, &stored_ip_mode) == ESP_OK) {
            ip_mode = (ip_mode_t) stored_ip_mode;
        }
        size_t addr_len = sizeof(static_ip);
        nvs_get_str(nvs_handle, NVS_STATIC_IP_KEY, static_ip, &addr_len);
        addr_len = sizeof(static_netmask);
        nvs_get_str(nvs_handle, NVS_STATIC_NETMASK_KEY, static_netmask, &addr_len);
        addr_len = sizeof(static_gateway);
        nvs_get_str(nvs_handle, NVS_STATIC_GATEWAY_KEY, static_gateway, &addr_len);
        addr_len = sizeof(dns_server);
        nvs_get_str(nvs_handle, NVS_DNS_SERVER_KEY, dns_server, &addr_len);
        if (ip_mode == IP_MODE_STATIC) {
            ESP_LOGI(TAG, "Static IP configured: %s/%s gw %s dns %s", static_ip, static_netmask,
                     static_gateway, dns_server[0] ? dns_server : "(auto)");
        } else if (dns_server[0]) {
            ESP_LOGI(TAG, "DNS override configured: %s", dns_server);
        }

        uint8_t stored_orientation = DISPLAY_ORIENTATION_LANDSCAPE;
        if (nvs_get_u8(nvs_handle, NVS_DISPLAY_ORIENTATION_KEY, &stored_orientation) == ESP_OK) {
            display_orientation = (display_orientation_t) stored_orientation;
            ESP_LOGI(
                TAG, "Loaded display orientation from NVS: %s",
                display_orientation == DISPLAY_ORIENTATION_LANDSCAPE ? "landscape" : "portrait");
        }

        int32_t stored_display_rotation_deg = 0;
        if (nvs_get_i32(nvs_handle, NVS_DISPLAY_ROTATION_DEG_KEY, &stored_display_rotation_deg) ==
            ESP_OK) {
            // Only 0/180 are supported; a legacy 90/270 value would swap the
            // paint geometry away from the native layout the streaming
            // pipeline assumes (see apply_config_from_json). Keep the board
            // default instead.
            if (stored_display_rotation_deg == 0 || stored_display_rotation_deg == 180) {
                display_rotation_deg = stored_display_rotation_deg;
                ESP_LOGI(TAG, "Loaded display rotation from NVS: %d degrees", display_rotation_deg);
            } else {
                ESP_LOGW(TAG, "Ignoring unsupported stored display rotation %ld degrees",
                         (long) stored_display_rotation_deg);
            }
        }

        size_t wifi_ssid_len = WIFI_SSID_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_WIFI_SSID_KEY, wifi_ssid, &wifi_ssid_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded WiFi SSID from NVS: %s", wifi_ssid);
        } else {
            strncpy(wifi_ssid, DEFAULT_WIFI_SSID, WIFI_SSID_MAX_LEN - 1);
            wifi_ssid[WIFI_SSID_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No WiFi SSID in NVS, using default: %s", wifi_ssid);
        }

        size_t wifi_pass_len = WIFI_PASS_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_WIFI_PASS_KEY, wifi_password, &wifi_pass_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded WiFi password from NVS (length: %zu)", wifi_pass_len);
        } else {
            strncpy(wifi_password, DEFAULT_WIFI_PASSWORD, WIFI_PASS_MAX_LEN - 1);
            wifi_password[WIFI_PASS_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No WiFi password in NVS, using default");
        }

        // Auto Rotate
        uint8_t stored_enabled = 0;
        if (nvs_get_u8(nvs_handle, NVS_AUTO_ROTATE_KEY, &stored_enabled) == ESP_OK) {
            auto_rotate_enabled = (stored_enabled != 0);
            ESP_LOGI(TAG, "Loaded auto-rotate enabled from NVS: %s",
                     auto_rotate_enabled ? "yes" : "no");
        }

        // Rotation schedule (cron). If absent, fall back to migrating a legacy
        // interval, else seed the default — both handled after this handle closes.
        size_t cron_len = sizeof(cron_buf);
        if (nvs_get_str(nvs_handle, NVS_ROTATE_CRON_KEY, cron_buf, &cron_len) == ESP_OK) {
            cron_load_from_joined(cron_buf);
            seed_default_cron = false;
            ESP_LOGI(TAG, "Loaded %d cron rule(s) from NVS", cron_rule_count);
        } else if (nvs_get_i32(nvs_handle, NVS_ROTATE_INTERVAL_KEY, &legacy_interval) == ESP_OK) {
            migrate_legacy_interval = true;
            seed_default_cron = false;
        }

        uint8_t stored_mode = ROTATION_MODE_URL;  // Default fallback
        if (nvs_get_u8(nvs_handle, NVS_ROTATION_MODE_KEY, &stored_mode) == ESP_OK) {
            rotation_mode = (rotation_mode_t) stored_mode;
            ESP_LOGI(TAG, "Loaded rotation mode from NVS: %s",
                     config_manager_rotation_mode_str(rotation_mode));
        } else if (storage_has_persistent_storage()) {
            rotation_mode = ROTATION_MODE_STORAGE;
            ESP_LOGI(TAG, "No rotation mode in NVS, using default for persistent storage: storage");
        } else {
            ESP_LOGI(TAG, "No rotation mode in NVS, using default for no-storage: url");
        }

        // Auto Rotate - SDCARD
        uint8_t stored_sd_mode = SD_ROTATION_RANDOM;
        if (nvs_get_u8(nvs_handle, NVS_SD_ROTATION_MODE_KEY, &stored_sd_mode) == ESP_OK) {
            sd_rotation_mode = (sd_rotation_mode_t) stored_sd_mode;
            ESP_LOGI(TAG, "Loaded SD rotation mode from NVS: %s",
                     sd_rotation_mode == SD_ROTATION_SEQUENTIAL ? "sequential" : "random");
        }

        int32_t stored_last_index = -1;
        if (nvs_get_i32(nvs_handle, NVS_LAST_INDEX_KEY, &stored_last_index) == ESP_OK) {
            last_index = stored_last_index;
            ESP_LOGI(TAG, "Loaded last index from NVS: %ld", (long) last_index);
        }

        // Auto Rotate - URL
        size_t url_len = IMAGE_URL_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_IMAGE_URL_KEY, image_url, &url_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded image URL from NVS: %s", image_url);
        } else {
            strncpy(image_url, DEFAULT_IMAGE_URL, IMAGE_URL_MAX_LEN - 1);
            image_url[IMAGE_URL_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No image URL in NVS, using default: %s", image_url);
        }

        // CA Certificate DER blob (heap-allocated)
        size_t blob_len = 0;
        if (nvs_get_blob(nvs_handle, NVS_CA_CERT_KEY, NULL, &blob_len) == ESP_OK && blob_len > 0) {
            ca_cert_der = malloc(blob_len);
            if (ca_cert_der &&
                nvs_get_blob(nvs_handle, NVS_CA_CERT_KEY, ca_cert_der, &blob_len) == ESP_OK) {
                ca_cert_der_len = blob_len;
                ESP_LOGI(TAG, "Loaded CA certificate from NVS (%zu bytes)", ca_cert_der_len);
            } else {
                free(ca_cert_der);
                ca_cert_der = NULL;
                ca_cert_der_len = 0;
            }
        }

        size_t access_token_len = ACCESS_TOKEN_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_ACCESS_TOKEN_KEY, access_token, &access_token_len) ==
            ESP_OK) {
            ESP_LOGI(TAG, "Loaded access token from NVS (length: %zu)", access_token_len);
        }

        size_t http_header_key_len = HTTP_HEADER_KEY_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_HTTP_HEADER_KEY_KEY, http_header_key,
                        &http_header_key_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded HTTP header key from NVS: %s", http_header_key);
        }

        size_t http_header_value_len = HTTP_HEADER_VALUE_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_HTTP_HEADER_VALUE_KEY, http_header_value,
                        &http_header_value_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded HTTP header value from NVS (length: %zu)", http_header_value_len);
        }

        uint8_t stored_save_dl = 0;
        if (nvs_get_u8(nvs_handle, NVS_SAVE_DOWNLOADED_KEY, &stored_save_dl) == ESP_OK) {
            save_downloaded_images = (stored_save_dl != 0);
            ESP_LOGI(TAG, "Loaded save_downloaded_images from NVS: %s",
                     save_downloaded_images ? "yes" : "no");
        }

        size_t etag_len = HTTP_ETAG_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_IMAGE_ETAG_KEY, image_etag, &etag_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded image ETag from NVS (length: %zu)", etag_len);
        }

        // Home Assistant
        size_t ha_url_len = HA_URL_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_HA_URL_KEY, ha_url, &ha_url_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded HA URL from NVS: %s", ha_url);
        } else {
            strncpy(ha_url, DEFAULT_HA_URL, HA_URL_MAX_LEN - 1);
            ha_url[HA_URL_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No HA URL in NVS, using default (empty)");
        }

        // The frame's own key. When unset, adopt the legacy openai_key so an
        // already-provisioned frame keeps generating after this rename, then
        // persist it so the migration happens once.
        size_t ai_key_len = AI_API_KEY_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_AI_API_KEY_KEY, ai_api_key, &ai_key_len) != ESP_OK ||
            ai_api_key[0] == '\0') {
            char legacy[AI_API_KEY_MAX_LEN] = {0};
            size_t legacy_len = sizeof(legacy);
            if (nvs_get_str(nvs_handle, "openai_key", legacy, &legacy_len) == ESP_OK &&
                legacy[0] != '\0') {
                snprintf(ai_api_key, sizeof(ai_api_key), "%s", legacy);
                nvs_handle_t w;
                if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &w) == ESP_OK) {
                    nvs_set_str(w, NVS_AI_API_KEY_KEY, ai_api_key);
                    nvs_commit(w);
                    nvs_close(w);
                }
                ESP_LOGI(TAG, "Migrated openai_key to the frame's AI key");
            }
        }

        size_t wurl_len = AI_WRITER_URL_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_AI_WRITER_URL_KEY, ai_writer_url, &wurl_len) != ESP_OK ||
            ai_writer_url[0] == '\0') {
            snprintf(ai_writer_url, sizeof(ai_writer_url), "%s", DEFAULT_AI_WRITER_URL);
        }
        size_t wmodel_len = AI_WRITER_MODEL_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_AI_WRITER_MODEL_KEY, ai_writer_model, &wmodel_len) != ESP_OK ||
            ai_writer_model[0] == '\0') {
            snprintf(ai_writer_model, sizeof(ai_writer_model), "%s", DEFAULT_AI_WRITER_MODEL);
        }
        size_t place_len = AI_PLACE_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_AI_PLACE_KEY, ai_place, &place_len) != ESP_OK) {
            ai_place[0] = '\0';
        }

        size_t order_len = sizeof(ai_model_order);
        if (nvs_get_blob(nvs_handle, NVS_AI_MODEL_ORDER_KEY, ai_model_order, &order_len) != ESP_OK ||
            ai_model_order[0] == '\0') {
            snprintf(ai_model_order, sizeof(ai_model_order), "%s", DEFAULT_AI_MODEL_ORDER);
        }

        int32_t streak = 0;
        if (nvs_get_i32(nvs_handle, NVS_AI_FAIL_STREAK_KEY, &streak) == ESP_OK) {
            ai_fail_streak = streak < 0 ? 0 : (int)streak;
        }

        size_t tmpl_len = sizeof(ai_template);
        if (nvs_get_blob(nvs_handle, NVS_AI_TEMPLATE_KEY, ai_template, &tmpl_len) == ESP_OK &&
            tmpl_len > 1) {
            ai_template_len = tmpl_len;
            ESP_LOGI(TAG, "Loaded prompt template from NVS (%u bytes)", (unsigned)tmpl_len);
        } else {
            ai_template_len = 0;
        }

        // Weather location. Out-of-range or absent coordinates fall back to the
        // compiled default rather than being clamped: a half-correct position
        // would produce a forecast for somewhere the frame is not, which is worse
        // than an honest default.
        double wx_lat_val = DEFAULT_WEATHER_LATITUDE;
        if (nvs_get_double(nvs_handle, NVS_WEATHER_LATITUDE_KEY, &wx_lat_val) == ESP_OK &&
            wx_lat_val >= WEATHER_LATITUDE_MIN && wx_lat_val <= WEATHER_LATITUDE_MAX) {
            weather_latitude = wx_lat_val;
        }
        double wx_lon_val = DEFAULT_WEATHER_LONGITUDE;
        if (nvs_get_double(nvs_handle, NVS_WEATHER_LONGITUDE_KEY, &wx_lon_val) == ESP_OK &&
            wx_lon_val >= WEATHER_LONGITUDE_MIN && wx_lon_val <= WEATHER_LONGITUDE_MAX) {
            weather_longitude = wx_lon_val;
        }

        // Generation endpoint and model. An unset or blank value means "use the
        // compiled default", so a fresh device works without any configuration.
        // A stored non-https endpoint is discarded rather than honoured: the API
        // key travels to whatever this says, so the scheme is not a preference.
        size_t ai_url_len = AI_URL_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_AI_URL_KEY, ai_url, &ai_url_len) != ESP_OK ||
            ai_url[0] == '\0' || strncmp(ai_url, "https://", 8) != 0) {
            snprintf(ai_url, sizeof(ai_url), "%s", DEFAULT_AI_URL);
        }
        size_t ai_model_len = AI_MODEL_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_AI_IMAGE_MODEL_KEY, ai_image_model, &ai_model_len) != ESP_OK ||
            ai_image_model[0] == '\0') {
            snprintf(ai_image_model, sizeof(ai_image_model), "%s", DEFAULT_AI_IMAGE_MODEL);
        }

        int32_t stored_ai_fails = 0;
        if (nvs_get_i32(nvs_handle, NVS_AI_MODEL_FAILS_KEY, &stored_ai_fails) == ESP_OK) {
            ai_model_fails = stored_ai_fails < 0 ? 0 : (int)stored_ai_fails;
        }

        size_t ai_dir_len = AI_DIRECTION_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_AI_DIRECTION_KEY, ai_direction, &ai_dir_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded AI direction (%u chars)", (unsigned)strlen(ai_direction));
        }
        ESP_LOGI(TAG, "Weather location: %.4f, %.4f", weather_latitude, weather_longitude);
        ESP_LOGI(TAG, "Generation endpoint: %s (%s)", ai_url, ai_image_model);

        // Power
        uint8_t deep_sleep_val = 1;  // Default to enabled
        if (nvs_get_u8(nvs_handle, NVS_DEEP_SLEEP_KEY, &deep_sleep_val) == ESP_OK) {
            deep_sleep_enabled = (deep_sleep_val != 0);
            ESP_LOGI(TAG, "Loaded deep sleep setting from NVS: %s",
                     deep_sleep_enabled ? "enabled" : "disabled");
        }

        // Low battery badge
        uint8_t low_batt_ind_val = 1;
        if (nvs_get_u8(nvs_handle, NVS_LOW_BATT_IND_KEY, &low_batt_ind_val) == ESP_OK) {
            low_battery_indicator = (low_batt_ind_val != 0);
        }
        uint8_t low_batt_thr_val = LOW_BATTERY_THRESHOLD_DEFAULT;
        if (nvs_get_u8(nvs_handle, NVS_LOW_BATT_THR_KEY, &low_batt_thr_val) == ESP_OK &&
            low_batt_thr_val >= LOW_BATTERY_THRESHOLD_MIN &&
            low_batt_thr_val <= LOW_BATTERY_THRESHOLD_MAX) {
            low_battery_threshold = low_batt_thr_val;
        }
        ESP_LOGI(TAG, "Low battery badge: %s, threshold %d%%", low_battery_indicator ? "on" : "off",
                 low_battery_threshold);

        // Debugging
        uint8_t debug_log_val = 0;
        if (nvs_get_u8(nvs_handle, NVS_DEBUG_LOG_KEY, &debug_log_val) == ESP_OK) {
            debug_log_enabled = (debug_log_val != 0);
            ESP_LOGI(TAG, "Loaded debug log setting from NVS: %s",
                     debug_log_enabled ? "enabled" : "disabled");
        }

        // Config sync timestamp
        if (nvs_get_i64(nvs_handle, "cfg_updated", &config_last_updated) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded config_last_updated: %lld", (long long) config_last_updated);
        }

        nvs_close(nvs_handle);
    }

    // Resolve the rotation schedule now that the read-only handle is closed.
    if (migrate_legacy_interval) {
        char rule[CRON_RULE_MAX_LEN];
        cron_from_legacy_interval((int) legacy_interval, rule, sizeof(rule));
        const char *one[1] = {rule};
        config_manager_set_cron_rules(one, 1);  // persists to NVS
        ESP_LOGI(TAG, "Migrated legacy interval %d s -> cron \"%s\"", (int) legacy_interval, rule);
    } else if (seed_default_cron) {
        // Fresh device: seed default in memory; persists on the first user save.
        cron_load_from_joined(DEFAULT_ROTATE_CRON);
        ESP_LOGI(TAG, "No rotation schedule in NVS, using default: %s", DEFAULT_ROTATE_CRON);
    }

    // Erase the legacy quiet-hours (sleep schedule) keys. That feature was
    // replaced by cron rules that carry their own active-hours window; the
    // firmware no longer reads these keys, so drop them from NVS.
    {
        nvs_handle_t erase_handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &erase_handle) == ESP_OK) {
            bool erased = false;
            const char *legacy_keys[] = {
                NVS_SLEEP_SCHEDULE_ENABLED_KEY,
                NVS_SLEEP_SCHEDULE_START_KEY,
                NVS_SLEEP_SCHEDULE_END_KEY,
            };
            for (size_t i = 0; i < sizeof(legacy_keys) / sizeof(legacy_keys[0]); i++) {
                if (nvs_erase_key(erase_handle, legacy_keys[i]) == ESP_OK) {
                    erased = true;
                }
            }
            if (erased) {
                nvs_commit(erase_handle);
                ESP_LOGI(TAG, "Erased legacy sleep-schedule keys from NVS");
            }
            nvs_close(erase_handle);
        }
    }

    // Apply timezone setting
    setenv("TZ", tz_string, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to: %s", tz_string);

    // Log current system time in local timezone
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);

    // Calculate UTC offset for display
    struct tm utc_timeinfo;
    gmtime_r(&now, &utc_timeinfo);
    int offset_hours = timeinfo.tm_hour - utc_timeinfo.tm_hour;

    // Handle day boundary crossing
    if (offset_hours > 12)
        offset_hours -= 24;
    if (offset_hours < -12)
        offset_hours += 24;

    ESP_LOGI(TAG, "Config manager initialized");
    return ESP_OK;
}
// ============================================================================
// General
// ============================================================================

void config_manager_set_device_name(const char *name)
{
    if (name == NULL) {
        return;
    }

    strncpy(device_name, name, DEVICE_NAME_MAX_LEN - 1);
    device_name[DEVICE_NAME_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_DEVICE_NAME_KEY, device_name);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Device name set to: %s", device_name);
}
const char *config_manager_get_device_name(void)
{
    return device_name;
}

esp_err_t config_manager_set_timezone(const char *tz)
{
    if (tz == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    // Refuse an over-long rule rather than store a truncated prefix: a POSIX
    // rule keeps its DST transitions at the end, so the prefix is a
    // different zone that nothing would flag.
    if (strlen(tz) >= TIMEZONE_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    strcpy(tz_string, tz);  // length checked above

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_TIMEZONE_KEY, tz_string);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Timezone set to: %s", tz_string);
    return ESP_OK;
}
const char *config_manager_get_timezone(void)
{
    if (tz_string[0] == '\0') {
        return "UTC0";
    }
    return tz_string;
}

void config_manager_set_ntp_server(const char *server)
{
    if (server == NULL) {
        return;
    }

    strncpy(ntp_server, server, NTP_SERVER_MAX_LEN - 1);
    ntp_server[NTP_SERVER_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_NTP_SERVER_KEY, ntp_server);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "NTP server set to: %s", ntp_server);
}

const char *config_manager_get_ntp_server(void)
{
    if (ntp_server[0] == '\0') {
        return DEFAULT_NTP_SERVER;
    }
    return ntp_server;
}

// ----------------------------------------------------------------------------
// Advanced network settings
// ----------------------------------------------------------------------------

// Persist one dotted-IPv4 string setting (helper for the network settings).
static void set_ip_str(const char *nvs_key, char *cache, const char *value)
{
    if (value == NULL) {
        value = "";
    }
    strncpy(cache, value, IP_ADDR_STR_MAX_LEN - 1);
    cache[IP_ADDR_STR_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, nvs_key, cache);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

void config_manager_set_ip_mode(ip_mode_t mode)
{
    ip_mode = mode;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_IP_MODE_KEY, (uint8_t) mode);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    ESP_LOGI(TAG, "IP mode set to: %s", mode == IP_MODE_STATIC ? "static" : "dhcp");
}

ip_mode_t config_manager_get_ip_mode(void)
{
    return ip_mode;
}

void config_manager_set_static_ip(const char *ip)
{
    set_ip_str(NVS_STATIC_IP_KEY, static_ip, ip);
}

const char *config_manager_get_static_ip(void)
{
    return static_ip;
}

void config_manager_set_static_netmask(const char *mask)
{
    set_ip_str(NVS_STATIC_NETMASK_KEY, static_netmask, mask);
}

const char *config_manager_get_static_netmask(void)
{
    return static_netmask;
}

void config_manager_set_static_gateway(const char *gw)
{
    set_ip_str(NVS_STATIC_GATEWAY_KEY, static_gateway, gw);
}

const char *config_manager_get_static_gateway(void)
{
    return static_gateway;
}

void config_manager_set_dns_server(const char *dns)
{
    set_ip_str(NVS_DNS_SERVER_KEY, dns_server, dns);
}

const char *config_manager_get_dns_server(void)
{
    return dns_server;
}

void config_manager_set_display_orientation(display_orientation_t orientation)
{
    display_orientation = orientation;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_DISPLAY_ORIENTATION_KEY, (uint8_t) orientation);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Display orientation set to: %s",
             orientation == DISPLAY_ORIENTATION_LANDSCAPE ? "landscape" : "portrait");
}

display_orientation_t config_manager_get_display_orientation(void)
{
    return display_orientation;
}

void config_manager_set_display_rotation_deg(int rotation_deg)
{
    display_rotation_deg = rotation_deg;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_DISPLAY_ROTATION_DEG_KEY, rotation_deg);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Display rotation set to %d degrees", rotation_deg);
}

int config_manager_get_display_rotation_deg(void)
{
    return display_rotation_deg;
}

void config_manager_set_wifi_ssid(const char *ssid)
{
    if (ssid == NULL) {
        return;
    }

    strncpy(wifi_ssid, ssid, WIFI_SSID_MAX_LEN - 1);
    wifi_ssid[WIFI_SSID_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_WIFI_SSID_KEY, wifi_ssid);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "WiFi SSID set to: %s", wifi_ssid);
}

const char *config_manager_get_wifi_ssid(void)
{
    return wifi_ssid;
}

void config_manager_set_wifi_password(const char *password)
{
    if (password == NULL) {
        return;
    }

    strncpy(wifi_password, password, WIFI_PASS_MAX_LEN - 1);
    wifi_password[WIFI_PASS_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_WIFI_PASS_KEY, wifi_password);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "WiFi password set (length: %zu)", strlen(wifi_password));
}

const char *config_manager_get_wifi_password(void)
{
    return wifi_password;
}
// ============================================================================
// Auto Rotate
// ============================================================================

void config_manager_set_auto_rotate(bool enabled)
{
    auto_rotate_enabled = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_AUTO_ROTATE_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Auto-rotate %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_auto_rotate(void)
{
    return auto_rotate_enabled;
}

int config_manager_get_cron_rule_count(void)
{
    return cron_rule_count;
}

const char *config_manager_get_cron_rule(int index)
{
    if (index < 0 || index >= cron_rule_count) {
        return NULL;
    }
    return cron_rules_store[index];
}

void config_manager_set_cron_rules(const char *const *rules, int count)
{
    if (count < 0) {
        count = 0;
    }
    cron_rule_count = 0;
    for (int i = 0; i < count && cron_rule_count < MAX_CRON_RULES; i++) {
        if (!rules[i] || rules[i][0] == '\0' || strlen(rules[i]) >= CRON_RULE_MAX_LEN) {
            continue;
        }
        strncpy(cron_rules_store[cron_rule_count], rules[i], CRON_RULE_MAX_LEN - 1);
        cron_rules_store[cron_rule_count][CRON_RULE_MAX_LEN - 1] = '\0';
        cron_rule_count++;
    }

    cron_persist();
    ESP_LOGI(TAG, "Rotation schedule set to %d cron rule(s)", cron_rule_count);
}

void config_manager_set_cron_rules_from_interval(int seconds)
{
    char rule[CRON_RULE_MAX_LEN];
    cron_from_legacy_interval(seconds, rule, sizeof(rule));
    const char *one[1] = {rule};
    config_manager_set_cron_rules(one, 1);
}

int config_manager_get_compiled_cron_rules(cron_rule_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < cron_rule_count && n < max; i++) {
        if (cron_parse(cron_rules_store[i], &out[n])) {
            n++;
        }
    }
    return n;
}

// Canonical name for a rotation mode. Public so the API serialisation and the
// logs cannot drift apart: a mode missing from this table shows up as "unknown"
// rather than being silently reported as some other mode.
const char *config_manager_rotation_mode_str(rotation_mode_t mode)
{
    switch (mode) {
    case ROTATION_MODE_STORAGE:
        return "storage";
    case ROTATION_MODE_URL:
        return "url";
    case ROTATION_MODE_AI:
        return "ai";
    }
    return "unknown";
}

void config_manager_set_rotation_mode(rotation_mode_t mode)
{
    if (!storage_has_persistent_storage() && mode == ROTATION_MODE_STORAGE) {
        ESP_LOGE(TAG, "Cannot set rotation mode to STORAGE: Local storage not supported");
        return;
    }

    rotation_mode = mode;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_ROTATION_MODE_KEY, (uint8_t) mode);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Rotation mode set to: %s", config_manager_rotation_mode_str(mode));
}

rotation_mode_t config_manager_get_rotation_mode(void)
{
    return rotation_mode;
}
// ============================================================================
// Auto Rotate - SDCARD
// ============================================================================

void config_manager_set_sd_rotation_mode(sd_rotation_mode_t mode)
{
    sd_rotation_mode = mode;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_SD_ROTATION_MODE_KEY, (uint8_t) mode);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "SD rotation mode set to: %s",
             mode == SD_ROTATION_SEQUENTIAL ? "sequential" : "random");
}

sd_rotation_mode_t config_manager_get_sd_rotation_mode(void)
{
    return sd_rotation_mode;
}

void config_manager_set_last_index(int32_t index)
{
    last_index = index;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_LAST_INDEX_KEY, index);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

int32_t config_manager_get_last_index(void)
{
    return last_index;
}

// ============================================================================
// Auto Rotate - URL
// ============================================================================

void config_manager_set_image_url(const char *url)
{
    const char *new_url = url ? url : "";
    bool url_changed = strcmp(image_url, new_url) != 0;

    strncpy(image_url, new_url, IMAGE_URL_MAX_LEN - 1);
    image_url[IMAGE_URL_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        if (image_url[0] != '\0') {
            nvs_set_str(nvs_handle, NVS_IMAGE_URL_KEY, image_url);
        } else {
            nvs_erase_key(nvs_handle, NVS_IMAGE_URL_KEY);
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    if (url_changed) {
        config_manager_set_image_etag("");
    }

    ESP_LOGI(TAG, "Image URL set to: %s", image_url[0] ? image_url : "(empty)");
}
const char *config_manager_get_image_url(void)
{
    return image_url;
}

static void set_ca_cert_locked(const uint8_t *der, size_t len)
{
    ca_cert_generation++;
    free(ca_cert_der);
    ca_cert_der = NULL;
    ca_cert_der_len = 0;

    if (der && len > 0) {
        ca_cert_der = malloc(len);
        if (ca_cert_der) {
            memcpy(ca_cert_der, der, len);
            ca_cert_der_len = len;
        }
    }

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        if (ca_cert_der) {
            nvs_set_blob(nvs_handle, NVS_CA_CERT_KEY, ca_cert_der, ca_cert_der_len);
        } else {
            nvs_erase_key(nvs_handle, NVS_CA_CERT_KEY);
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "CA certificate %s (%zu bytes)", ca_cert_der ? "set" : "cleared",
             ca_cert_der_len);
}

void config_manager_set_ca_cert_der(const uint8_t *der, size_t len)
{
    xSemaphoreTake(ca_cert_lock, portMAX_DELAY);
    set_ca_cert_locked(der, len);
    xSemaphoreGive(ca_cert_lock);
}

bool config_manager_has_ca_cert(void)
{
    xSemaphoreTake(ca_cert_lock, portMAX_DELAY);
    bool set = ca_cert_der_len > 0;
    xSemaphoreGive(ca_cert_lock);
    return set;
}

uint8_t *config_manager_copy_ca_cert_der(size_t *out_len, uint32_t *generation)
{
    xSemaphoreTake(ca_cert_lock, portMAX_DELAY);
    *out_len = ca_cert_der_len;
    *generation = ca_cert_generation;
    uint8_t *copy = NULL;
    if (ca_cert_der_len > 0) {
        copy = malloc(ca_cert_der_len);
        if (copy) {
            memcpy(copy, ca_cert_der, ca_cert_der_len);
        }
    }
    xSemaphoreGive(ca_cert_lock);
    return copy;
}

bool config_manager_clear_ca_cert_der_if_unchanged(uint32_t generation)
{
    xSemaphoreTake(ca_cert_lock, portMAX_DELAY);
    bool unchanged = generation == ca_cert_generation;
    if (unchanged) {
        set_ca_cert_locked(NULL, 0);
    }
    xSemaphoreGive(ca_cert_lock);
    return unchanged;
}

void config_manager_set_access_token(const char *token)
{
    if (token == NULL) {
        return;
    }

    strncpy(access_token, token, ACCESS_TOKEN_MAX_LEN - 1);
    access_token[ACCESS_TOKEN_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_ACCESS_TOKEN_KEY, access_token);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Access token set (length: %zu)", strlen(access_token));
}
const char *config_manager_get_access_token(void)
{
    return access_token;
}

esp_err_t config_manager_set_http_password(const char *password)
{
    if (password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(password) >= HTTP_PASSWORD_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Persist first and only then switch the live value, so a failed write
    // can't leave a password that guards this boot but vanishes on the next.
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_HTTP_PASSWORD_KEY, password);
        if (err == ESP_OK) {
            err = nvs_commit(nvs_handle);
        }
        nvs_close(nvs_handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store HTTP API password: %s", esp_err_to_name(err));
        return err;
    }

    strcpy(http_password, password);  // length checked above

    // Never log the value itself.
    ESP_LOGI(TAG, "HTTP API password %s", http_password[0] ? "set" : "cleared");
    return ESP_OK;
}

const char *config_manager_get_http_password(void)
{
    return http_password;
}

void config_manager_set_http_header_key(const char *key)
{
    if (key == NULL) {
        return;
    }

    strncpy(http_header_key, key, HTTP_HEADER_KEY_MAX_LEN - 1);
    http_header_key[HTTP_HEADER_KEY_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_HTTP_HEADER_KEY_KEY, http_header_key);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "HTTP header key set to: %s", http_header_key);
}
const char *config_manager_get_http_header_key(void)
{
    return http_header_key;
}

void config_manager_set_http_header_value(const char *value)
{
    if (value == NULL) {
        return;
    }

    strncpy(http_header_value, value, HTTP_HEADER_VALUE_MAX_LEN - 1);
    http_header_value[HTTP_HEADER_VALUE_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_HTTP_HEADER_VALUE_KEY, http_header_value);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "HTTP header value set (length: %zu)", strlen(http_header_value));
}
const char *config_manager_get_http_header_value(void)
{
    return http_header_value;
}

void config_manager_set_save_downloaded_images(bool enabled)
{
    save_downloaded_images = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_SAVE_DOWNLOADED_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Save downloaded images %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_save_downloaded_images(void)
{
    return save_downloaded_images;
}

void config_manager_set_image_etag(const char *etag)
{
    const char *new_etag = etag ? etag : "";

    // No-op if value unchanged — avoids NVS wear when the server does not send
    // an ETag (empty stays empty across every 200) or repeats the same ETag.
    if (strncmp(image_etag, new_etag, HTTP_ETAG_MAX_LEN) == 0) {
        return;
    }

    strncpy(image_etag, new_etag, HTTP_ETAG_MAX_LEN - 1);
    image_etag[HTTP_ETAG_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        if (image_etag[0] != '\0') {
            nvs_set_str(nvs_handle, NVS_IMAGE_ETAG_KEY, image_etag);
        } else {
            nvs_erase_key(nvs_handle, NVS_IMAGE_ETAG_KEY);
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

const char *config_manager_get_image_etag(void)
{
    return image_etag;
}
// ============================================================================
// Home Assistant
// ============================================================================

void config_manager_set_ha_url(const char *url)
{
    if (url) {
        strncpy(ha_url, url, HA_URL_MAX_LEN - 1);
        ha_url[HA_URL_MAX_LEN - 1] = '\0';

        // Strip trailing slashes so callers can safely append "/api/...".
        // A doubled slash ("host//api/...") is a different path to Home
        // Assistant's router and returns 404.
        size_t len = strlen(ha_url);
        while (len > 0 && ha_url[len - 1] == '/') {
            ha_url[--len] = '\0';
        }

        nvs_handle_t nvs_handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
            nvs_set_str(nvs_handle, NVS_HA_URL_KEY, ha_url);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
        }

        ESP_LOGI(TAG, "HA URL set to: %s", ha_url);
    }
}
const char *config_manager_get_ha_url(void)
{
    return ha_url;
}

// ============================================================================
// AI Generation
// ============================================================================

const char *config_manager_get_ai_api_key(void)
{
    return ai_api_key;
}

void config_manager_set_ai_api_key(const char *key)
{
    if (key == NULL) {
        return;
    }
    strncpy(ai_api_key, key, AI_API_KEY_MAX_LEN - 1);
    ai_api_key[AI_API_KEY_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_AI_API_KEY_KEY, ai_api_key);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    // Never log the value.
    ESP_LOGI(TAG, "Frame AI key %s", ai_api_key[0] ? "set" : "cleared");
}

const char *config_manager_get_ai_writer_url(void)
{
    return ai_writer_url[0] ? ai_writer_url : DEFAULT_AI_WRITER_URL;
}

void config_manager_set_ai_writer_url(const char *url)
{
    if (url == NULL) {
        return;
    }
    strncpy(ai_writer_url, url, sizeof(ai_writer_url) - 1);
    ai_writer_url[sizeof(ai_writer_url) - 1] = '\0';
    if (ai_writer_url[0] == '\0') {
        snprintf(ai_writer_url, sizeof(ai_writer_url), "%s", DEFAULT_AI_WRITER_URL);
    }
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_AI_WRITER_URL_KEY, ai_writer_url);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    ESP_LOGI(TAG, "Prompt writer endpoint set to %s", ai_writer_url);
}

const char *config_manager_get_ai_writer_model(void)
{
    return ai_writer_model[0] ? ai_writer_model : DEFAULT_AI_WRITER_MODEL;
}

void config_manager_set_ai_writer_model(const char *model)
{
    if (model == NULL) {
        return;
    }
    strncpy(ai_writer_model, model, sizeof(ai_writer_model) - 1);
    ai_writer_model[sizeof(ai_writer_model) - 1] = '\0';
    if (ai_writer_model[0] == '\0') {
        snprintf(ai_writer_model, sizeof(ai_writer_model), "%s", DEFAULT_AI_WRITER_MODEL);
    }
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_AI_WRITER_MODEL_KEY, ai_writer_model);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    ESP_LOGI(TAG, "Prompt writer model set to %s", ai_writer_model);
}

const char *config_manager_get_ai_place(void)
{
    // The compiled template uses {{place}}, so an unset value would make every
    // prompt fail to compose. Fall back rather than refuse.
    return ai_place[0] ? ai_place : DEFAULT_AI_PLACE;
}

const char *config_manager_get_ai_model_order(void)
{
    return ai_model_order[0] ? ai_model_order : DEFAULT_AI_MODEL_ORDER;
}

esp_err_t config_manager_set_ai_model_order(const char *csv)
{
    if (!csv || csv[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(csv) >= sizeof(ai_model_order)) {
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(ai_model_order, sizeof(ai_model_order), "%s", csv);

    // The head of the order is the model in use; keeping ai_image_model in step
    // means every existing reader of it (status, logs) still sees the truth.
    char head[AI_MODEL_MAX_LEN];
    size_t i = 0;
    while (csv[i] && csv[i] != ',' && i < sizeof(head) - 1) {
        head[i] = csv[i];
        i++;
    }
    head[i] = '\0';
    if (head[0]) {
        config_manager_set_ai_image_model(head);
    }

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t err = nvs_set_blob(nvs_handle, NVS_AI_MODEL_ORDER_KEY, ai_model_order,
                                 strlen(ai_model_order) + 1);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Model order set to %s", ai_model_order);
    }
    return err;
}

int config_manager_get_ai_fail_streak(void)
{
    return ai_fail_streak;
}

void config_manager_set_ai_fail_streak(int days)
{
    ai_fail_streak = days < 0 ? 0 : days;
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_AI_FAIL_STREAK_KEY, ai_fail_streak);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

const char *config_manager_get_ai_template(void)
{
    return ai_template_len > 1 ? ai_template : NULL;
}

esp_err_t config_manager_set_ai_template(const char *tmpl)
{
    if (!tmpl || tmpl[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(tmpl) + 1;  // include the terminator
    if (len > sizeof(ai_template)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(ai_template, tmpl, len);
    ai_template_len = len;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t err = nvs_set_blob(nvs_handle, NVS_AI_TEMPLATE_KEY, ai_template, len);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Prompt template saved (%u bytes)", (unsigned)len);
    }
    return err;
}

void config_manager_clear_ai_template(void)
{
    ai_template[0] = '\0';
    ai_template_len = 0;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_erase_key(nvs_handle, NVS_AI_TEMPLATE_KEY);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    ESP_LOGI(TAG, "Prompt template reset to the built-in default");
}

void config_manager_set_ai_place(const char *place)
{
    if (place == NULL) {
        return;
    }
    strncpy(ai_place, place, sizeof(ai_place) - 1);
    ai_place[sizeof(ai_place) - 1] = '\0';
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_AI_PLACE_KEY, ai_place);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    ESP_LOGI(TAG, "Place set to \"%s\"", ai_place);
}

void config_manager_set_weather_latitude(double value)
{
    weather_latitude = value;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_double(nvs_handle, NVS_WEATHER_LATITUDE_KEY, value);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Weather latitude set to %.4f", weather_latitude);
}

double config_manager_get_weather_latitude(void)
{
    return weather_latitude;
}

void config_manager_set_weather_longitude(double value)
{
    weather_longitude = value;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_double(nvs_handle, NVS_WEATHER_LONGITUDE_KEY, value);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Weather longitude set to %.4f", weather_longitude);
}

double config_manager_get_weather_longitude(void)
{
    return weather_longitude;
}

void config_manager_set_ai_url(const char *url)
{
    if (url == NULL) {
        return;
    }

    // The https requirement is enforced here as well as at the JSON boundary:
    // this is the chokepoint every write goes through, and an http endpoint would
    // ship the key as a cleartext bearer token on every generation. An
    // empty value is not a scheme violation but a request for the default.
    if (url[0] != '\0' && strncmp(url, "https://", 8) != 0) {
        ESP_LOGW(TAG, "Refusing non-https generation endpoint: %s", url);
        return;
    }

    strncpy(ai_url, url, AI_URL_MAX_LEN - 1);
    ai_url[AI_URL_MAX_LEN - 1] = '\0';
    if (ai_url[0] == '\0') {
        snprintf(ai_url, sizeof(ai_url), "%s", DEFAULT_AI_URL);
    }

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_AI_URL_KEY, ai_url);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Generation endpoint set to %s", ai_url);
}

const char *config_manager_get_ai_url(void)
{
    if (ai_url[0] == '\0') {
        return DEFAULT_AI_URL;
    }
    return ai_url;
}

void config_manager_set_ai_image_model(const char *model)
{
    if (model == NULL) {
        return;
    }

    strncpy(ai_image_model, model, AI_MODEL_MAX_LEN - 1);
    ai_image_model[AI_MODEL_MAX_LEN - 1] = '\0';
    if (ai_image_model[0] == '\0') {
        snprintf(ai_image_model, sizeof(ai_image_model), "%s", DEFAULT_AI_IMAGE_MODEL);
    }

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_AI_IMAGE_MODEL_KEY, ai_image_model);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Image model set to %s", ai_image_model);
}

const char *config_manager_get_ai_image_model(void)
{
    if (ai_image_model[0] == '\0') {
        return DEFAULT_AI_IMAGE_MODEL;
    }
    return ai_image_model;
}

int config_manager_get_ai_model_fails(void)
{
    return ai_model_fails;
}

void config_manager_set_ai_model_fails(int fails)
{
    if (fails < 0) {
        fails = 0;
    }
    ai_model_fails = fails;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_AI_MODEL_FAILS_KEY, (int32_t)ai_model_fails);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

void config_manager_set_ai_direction(const char *direction)
{
    if (direction == NULL) {
        return;
    }

    strncpy(ai_direction, direction, AI_DIRECTION_MAX_LEN - 1);
    ai_direction[AI_DIRECTION_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_AI_DIRECTION_KEY, ai_direction);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "AI direction set (%u chars)", (unsigned)strlen(ai_direction));
}

const char *config_manager_get_ai_direction(void)
{
    return ai_direction;
}

void config_manager_set_deep_sleep_enabled(bool enabled)
{
    deep_sleep_enabled = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_DEEP_SLEEP_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Deep sleep %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_deep_sleep_enabled(void)
{
    return deep_sleep_enabled;
}

void config_manager_set_low_battery_indicator(bool enabled)
{
    low_battery_indicator = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_LOW_BATT_IND_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

bool config_manager_get_low_battery_indicator(void)
{
    return low_battery_indicator;
}

void config_manager_set_low_battery_threshold(int percent)
{
    if (percent < LOW_BATTERY_THRESHOLD_MIN || percent > LOW_BATTERY_THRESHOLD_MAX) {
        return;
    }
    low_battery_threshold = percent;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_LOW_BATT_THR_KEY, (uint8_t) percent);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

int config_manager_get_low_battery_threshold(void)
{
    return low_battery_threshold;
}

void config_manager_set_debug_log_enabled(bool enabled)
{
    debug_log_enabled = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_DEBUG_LOG_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Debug log %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_debug_log_enabled(void)
{
    return debug_log_enabled;
}

void config_manager_set_config_last_updated(int64_t timestamp)
{
    config_last_updated = timestamp;
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i64(nvs_handle, "cfg_updated", config_last_updated);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

int64_t config_manager_get_config_last_updated(void)
{
    return config_last_updated;
}

void config_manager_touch_config(void)
{
    time_t now;
    time(&now);
    config_manager_set_config_last_updated((int64_t) now);
}
