#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "cron.h"
#include "esp_err.h"

esp_err_t config_manager_init(void);

// ============================================================================
// General
// ============================================================================

void config_manager_set_device_name(const char *name);
const char *config_manager_get_device_name(void);

esp_err_t config_manager_set_timezone(const char *tz);
const char *config_manager_get_timezone(void);

void config_manager_set_ntp_server(const char *server);
const char *config_manager_get_ntp_server(void);

// Advanced network settings (#43), same collapsed UI section as the NTP server
// above: static IP (instead of DHCP) and DNS override. The DNS override applies
// in both IP modes (empty string = automatic). Values are dotted IPv4 strings.
void config_manager_set_ip_mode(ip_mode_t mode);
ip_mode_t config_manager_get_ip_mode(void);
void config_manager_set_static_ip(const char *ip);
const char *config_manager_get_static_ip(void);
void config_manager_set_static_netmask(const char *mask);
const char *config_manager_get_static_netmask(void);
void config_manager_set_static_gateway(const char *gw);
const char *config_manager_get_static_gateway(void);
void config_manager_set_dns_server(const char *dns);
const char *config_manager_get_dns_server(void);

void config_manager_set_display_orientation(display_orientation_t orientation);
display_orientation_t config_manager_get_display_orientation(void);

void config_manager_set_display_rotation_deg(int rotation_deg);
int config_manager_get_display_rotation_deg(void);

void config_manager_set_wifi_ssid(const char *ssid);
const char *config_manager_get_wifi_ssid(void);

void config_manager_set_wifi_password(const char *password);
const char *config_manager_get_wifi_password(void);

// ============================================================================
// Auto Rotate
// ============================================================================

void config_manager_set_auto_rotate(bool enabled);
bool config_manager_get_auto_rotate(void);

int config_manager_get_cron_rule_count(void);

// Returns NULL if index is out of range.
const char *config_manager_get_cron_rule(int index);

// Replaces the rule set (caps at MAX_CRON_RULES, drops empty/over-long entries);
// caller should validate expressions with cron_parse() first.
void config_manager_set_cron_rules(const char *const *rules, int count);
void config_manager_set_cron_rules_from_interval(int seconds);

// Compiles the stored rules into `out`; returns the count written (<= max),
// skipping any that fail to parse.
int config_manager_get_compiled_cron_rules(cron_rule_t *out, int max);

void config_manager_set_rotation_mode(rotation_mode_t mode);
rotation_mode_t config_manager_get_rotation_mode(void);

// ============================================================================
// Auto Rotate - SDCARD
// ============================================================================

void config_manager_set_sd_rotation_mode(sd_rotation_mode_t mode);
sd_rotation_mode_t config_manager_get_sd_rotation_mode(void);

void config_manager_set_last_index(int32_t index);
int32_t config_manager_get_last_index(void);

// ============================================================================
// Auto Rotate - URL
// ============================================================================

void config_manager_set_image_url(const char *url);
const char *config_manager_get_image_url(void);

void config_manager_set_ca_cert_der(const uint8_t *der, size_t len);
bool config_manager_has_ca_cert(void);
// A private copy of the pin for a request, with the pin's generation at the
// time. *out_len is the pin's length whether or not the copy succeeded:
// NULL with 0 means nothing is pinned, NULL with a length means out of
// memory. Caller frees.
uint8_t *config_manager_copy_ca_cert_der(size_t *out_len, uint32_t *generation);
// Clears the pin only if it is still the one `generation` was taken from.
bool config_manager_clear_ca_cert_der_if_unchanged(uint32_t generation);

void config_manager_set_access_token(const char *token);
const char *config_manager_get_access_token(void);

/**
 * @brief Set the password guarding the device's own HTTP API. Empty disables
 *        authentication, which is the default.
 * @return ESP_OK once stored; ESP_ERR_INVALID_SIZE if longer than
 *         HTTP_PASSWORD_MAX_LEN - 1 bytes; an NVS error if it could not be
 *         persisted, in which case the previous password stays in effect.
 */
esp_err_t config_manager_set_http_password(const char *password);

/** @brief Current HTTP API password; empty string when authentication is off. */
const char *config_manager_get_http_password(void);

void config_manager_set_http_header_key(const char *key);
const char *config_manager_get_http_header_key(void);

void config_manager_set_http_header_value(const char *value);
const char *config_manager_get_http_header_value(void);

void config_manager_set_save_downloaded_images(bool enabled);
bool config_manager_get_save_downloaded_images(void);

// ETag captured from the last successful image fetch.
// Empty string means "not set" — skip the If-None-Match request header.
void config_manager_set_image_etag(const char *etag);
const char *config_manager_get_image_etag(void);

// ============================================================================
// Home Assistant
// ============================================================================

void config_manager_set_ha_url(const char *url);
const char *config_manager_get_ha_url(void);

// ============================================================================
// AI API Keys
// ============================================================================



// Weather location (drives weather.c) and the generation endpoint/model pair plus
// the owner's standing creative instructions.
void config_manager_set_weather_latitude(double value);
double config_manager_get_weather_latitude(void);
void config_manager_set_weather_longitude(double value);
double config_manager_get_weather_longitude(void);
void config_manager_set_ai_url(const char *url);
const char *config_manager_get_ai_url(void);
void config_manager_set_ai_image_model(const char *model);
const char *config_manager_get_ai_image_model(void);

// The key the frame uses. Migrated once from the retired openai_key slot at
// boot when unset, so devices provisioned before the rename keep working.
const char *config_manager_get_ai_api_key(void);
void config_manager_set_ai_api_key(const char *key);

// Prompt-writer endpoint and model. Config for the same reason the image pair
// is: they only mean anything together.
const char *config_manager_get_ai_writer_url(void);
void config_manager_set_ai_writer_url(const char *url);
const char *config_manager_get_ai_writer_model(void);
void config_manager_set_ai_writer_model(const char *model);

// Coarse area the prompt is written for, as opposed to the weather coordinates.
const char *config_manager_get_ai_place(void);
void config_manager_set_ai_place(const char *place);

// Ordered model preference list, comma-separated. Head = model in use.
const char *config_manager_get_ai_model_order(void);
esp_err_t config_manager_set_ai_model_order(const char *csv);

// Consecutive autonomous-generation failures, in days.
int config_manager_get_ai_fail_streak(void);
void config_manager_set_ai_fail_streak(int days);

// Editable prompt template. NULL means "use the compiled default".
const char *config_manager_get_ai_template(void);
esp_err_t config_manager_set_ai_template(const char *tmpl);
void config_manager_clear_ai_template(void);
/**
 * @brief Consecutive transient generation failures against the current model.
 *
 * Persisted because the device deep-sleeps: a counter in RAM would reset every
 * day and "eventually demote" would never arrive.
 */
int config_manager_get_ai_model_fails(void);
void config_manager_set_ai_model_fails(int fails);
void config_manager_set_ai_direction(const char *direction);
const char *config_manager_get_ai_direction(void);

// Canonical mode name shared by the logs and the API serialisation.
const char *config_manager_rotation_mode_str(rotation_mode_t mode);

// ============================================================================
// Power
// ============================================================================

void config_manager_set_deep_sleep_enabled(bool enabled);
bool config_manager_get_deep_sleep_enabled(void);

void config_manager_set_low_battery_indicator(bool enabled);
bool config_manager_get_low_battery_indicator(void);
void config_manager_set_low_battery_threshold(int percent);
int config_manager_get_low_battery_threshold(void);

// ============================================================================
// Debugging
// ============================================================================

void config_manager_set_debug_log_enabled(bool enabled);
bool config_manager_get_debug_log_enabled(void);

// ============================================================================
// Config Sync
// ============================================================================

void config_manager_set_config_last_updated(int64_t timestamp);
int64_t config_manager_get_config_last_updated(void);
void config_manager_touch_config(void);

#endif
