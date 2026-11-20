#ifndef CONFIG_H
#define CONFIG_H

#include <driver/gpio.h>

// Uncomment to debug deep sleep wake
// #define DEBUG_DEEP_SLEEP_WAKE

// What a rotation tick should put on the panel. The cron schedule decides *when*;
// this decides *what*. Adding a value here means touching every if/ternary that
// enumerates the mode -- there is no switch anywhere for the compiler to catch it.
typedef enum {
    ROTATION_MODE_STORAGE = 0,  // rotate through enabled albums on local storage
    ROTATION_MODE_URL = 1,      // fetch the configured image URL
    ROTATION_MODE_AI = 2        // generate today's picture (weather -> prompt -> image)
} rotation_mode_t;

typedef enum { SD_ROTATION_RANDOM = 0, SD_ROTATION_SEQUENTIAL = 1 } sd_rotation_mode_t;

typedef enum {
    DISPLAY_ORIENTATION_LANDSCAPE = 0,
    DISPLAY_ORIENTATION_PORTRAIT = 1
} display_orientation_t;

// IP configuration mode (#43): DHCP (default) or a static address. The DNS
// override is independent — it applies in both modes (empty = automatic).
typedef enum { IP_MODE_DHCP = 0, IP_MODE_STATIC = 1 } ip_mode_t;

#define IP_ADDR_STR_MAX_LEN 16  // dotted IPv4 + NUL

#define DEVICE_NAME_MAX_LEN 64
#define WIFI_SSID_MAX_LEN 33  // 32-byte SSID + NUL
#define WIFI_PASS_MAX_LEN 64
#define IMAGE_URL_MAX_LEN 256
#define HA_URL_MAX_LEN 256
#define ROTATION_MODE_MAX_LEN 16
#define TIMEZONE_MAX_LEN 64
#define NTP_SERVER_MAX_LEN 128
#define ACCESS_TOKEN_MAX_LEN 512

// Low battery badge: drawn on the picture at or below this charge (percent)
#define LOW_BATTERY_THRESHOLD_DEFAULT 10
#define LOW_BATTERY_THRESHOLD_MIN 5
#define LOW_BATTERY_THRESHOLD_MAX 50
// Optional password guarding the device's own HTTP API (#130). Empty disables
// it, which is the default -- most frames live on a trusted home network.
#define HTTP_PASSWORD_MAX_LEN 64  // buffer size: at most 63 bytes of password
#define HTTP_HEADER_KEY_MAX_LEN 64
#define HTTP_HEADER_VALUE_MAX_LEN 512
#define CA_CERT_MAX_LEN 4096
#define HTTP_ETAG_MAX_LEN 128

#define DEFAULT_DEVICE_NAME "PhotoFrame"
#define DEFAULT_WIFI_SSID "PhotoFrame"
#define DEFAULT_WIFI_PASSWORD "photoframe123"
#define DEFAULT_IMAGE_URL "https://loremflickr.com/800/480"
#define DEFAULT_HA_URL ""
#define DEFAULT_TIMEZONE "UTC0"
#define DEFAULT_NTP_SERVER "pool.ntp.org"

#define DEFAULT_ALBUM_NAME "Default"

#include "board_hal.h"

// Overridable so host tests can point the .current.* scheme at a local dir
#ifndef FS_MOUNT_POINT
#define FS_MOUNT_POINT "/storage"
#endif

#define IMAGE_DIRECTORY FS_MOUNT_POINT "/images"
#define DOWNLOAD_DIRECTORY IMAGE_DIRECTORY "/Downloads"

#define CURRENT_UPLOAD_PATH FS_MOUNT_POINT "/.current.tmp"
// A downloaded thumbnail waits here until its image is on the panel; the
// direct-display upload stages its thumbnail under the same name.
#define CURRENT_THUMB_UPLOAD_PATH FS_MOUNT_POINT "/.current_thumb.tmp"
#define CURRENT_JPG_PATH FS_MOUNT_POINT "/.current.jpg"
#define CURRENT_BMP_PATH FS_MOUNT_POINT "/.current.bmp"
#define CURRENT_PNG_PATH FS_MOUNT_POINT "/.current.png"
#define CURRENT_EPD_PATH FS_MOUNT_POINT "/.current.epdgz"
#define CURRENT_IMAGE_LINK FS_MOUNT_POINT "/.current.lnk"
#define CURRENT_CALIBRATION_PATH FS_MOUNT_POINT "/.calibration.png"

#ifdef DEBUG_DEEP_SLEEP_WAKE
#define AUTO_SLEEP_TIMEOUT_SEC 60
#else
#define AUTO_SLEEP_TIMEOUT_SEC 120
#endif

// Longer auto-sleep window during out-of-box setup (captive-portal
// provisioning), so the user has time to scan the QR code and configure WiFi
// via the app before the device sleeps.
#define OOBE_AUTO_SLEEP_TIMEOUT_SEC 600

// If a timer wake turns out to be early (RTC drift revealed by external RTC
// restore or NTP sync), rotate anyway when the scheduled time is at most this
// close; otherwise go back to sleep until the scheduled time. Must stay
// shorter than the time a rotation takes end-to-end: a rotation that starts
// within tolerance then finishes past its scheduled minute, so the next
// wake-up computation lands on the following scheduled time instead of
// re-firing the one that was just serviced.
#define EARLY_WAKE_TOLERANCE_SEC 5

// Upper bound on how long a server may ask us to stay awake after rotating (via
// the X-Post-Rotate-Wait-Sec image-response header) so it can pull our config.
// Caps a misbehaving/hostile server from keeping the frame awake and draining
// the battery.
#define POST_ROTATE_WAIT_MAX_SEC 30

// How long HA-configured frames keep the HTTP server up after rotating so a
// late config push can land. A server-requested post-rotate wait can extend it.
#define HA_CONFIG_WINDOW_SEC 10

// Image fetch timing (#121). FETCH_IO_TIMEOUT_MS bounds each socket operation
// (connect, time to first byte, and every wait for more data), so a dead link
// fails fast while a slow one still finishes. Retries only start while the
// fetch has used less than FETCH_RETRY_BUDGET_MS in total: quick failures
// (refused, DNS, HTTP error) get retried, but an attempt that already ran into
// the I/O timeout is not repeated. A frame on weak WiFi can legitimately need
// 30-80 s for one image, so this is not a cap on a single transfer; the
// auto-sleep timer remains the backstop for that.
#define FETCH_IO_TIMEOUT_MS 30000
#define FETCH_RETRY_BUDGET_MS 20000
// Pause between fetch attempts; counted against FETCH_RETRY_BUDGET_MS.
#define FETCH_RETRY_DELAY_MS 3000

// Default rotation schedule for fresh / factory-reset devices: every 12 hours.
// Simplified 3-field cron: "minute hour day-of-week".
#define DEFAULT_ROTATE_CRON "0 */12 *"
#define MAX_CRON_RULES 7
#define CRON_RULE_MAX_LEN 64

// WiFi
#define NVS_WIFI_SSID_KEY "wifi_ssid"
#define NVS_WIFI_PASS_KEY "wifi_pass"

// General
#define NVS_NAMESPACE "photoframe"
#define NVS_SETUP_COMPLETE_KEY "setup_complete"
#define NVS_DEVICE_NAME_KEY "device_name"
#define NVS_TIMEZONE_KEY "timezone"
#define NVS_DISPLAY_ORIENTATION_KEY "disp_orient"
#define NVS_DISPLAY_ROTATION_DEG_KEY "disp_rot_deg"

// Advanced network settings (collapsed section in the UI): custom NTP server,
// static IP instead of DHCP, and DNS override (#43)
#define NVS_NTP_SERVER_KEY "ntp_server"
#define NVS_IP_MODE_KEY "ip_mode"
#define NVS_STATIC_IP_KEY "static_ip"
#define NVS_STATIC_NETMASK_KEY "static_mask"
#define NVS_STATIC_GATEWAY_KEY "static_gw"
#define NVS_DNS_SERVER_KEY "dns_server"

// Auto Rotate
#define NVS_AUTO_ROTATE_KEY "auto_rotate"
#define NVS_ROTATE_CRON_KEY "rotate_cron"
#define NVS_ROTATE_INTERVAL_KEY "rotate_int"  // legacy: read once to migrate to cron
#define NVS_ROTATION_MODE_KEY "rotation_mode"
#define NVS_SLEEP_SCHEDULE_ENABLED_KEY "sleep_sched_en"
#define NVS_SLEEP_SCHEDULE_START_KEY "sleep_start"
#define NVS_SLEEP_SCHEDULE_END_KEY "sleep_end"

// Auto Rotate - SDCard
#define NVS_SD_ROTATION_MODE_KEY "sd_rot_mode"
#define NVS_LAST_INDEX_KEY "last_idx"
#define NVS_ENABLED_ALBUMS_KEY "enabled_albums"

// Auto Rotate - URL
#define NVS_IMAGE_URL_KEY "image_url"
#define NVS_CA_CERT_KEY "ca_cert"
#define NVS_ACCESS_TOKEN_KEY "access_token"
#define NVS_HTTP_PASSWORD_KEY "http_password"
#define NVS_HTTP_HEADER_KEY_KEY "http_hdr_key"
#define NVS_HTTP_HEADER_VALUE_KEY "http_hdr_val"
#define NVS_SAVE_DOWNLOADED_KEY "save_dl"
#define NVS_IMAGE_ETAG_KEY "image_etag"
#define NVS_LAST_FETCH_ERROR_KEY "last_fetch_err"

// Power
#define NVS_DEEP_SLEEP_KEY "deep_sleep"

// Debugging
#define NVS_DEBUG_LOG_KEY "debug_log"
#define NVS_LOW_BATT_IND_KEY "low_batt_ind"
#define NVS_LOW_BATT_THR_KEY "low_batt_thr"
#define NVS_LAST_CRASH_KEY "last_crash"

// Home Assistant
#define NVS_HA_URL_KEY "ha_url"

// AI API Keys (webapp client use; the openai key slot is also the bearer token
// for on-device generation -- see ai_gen.c)
#define AI_API_KEY_MAX_LEN 256

// On-device image generation (ai_gen.c).
//
// The request shape is OpenRouter's /api/v1/images, whose response is always
// base64 (there is no URL form), so the device decodes it itself. The endpoint
// and the image model are configuration rather than constants because they are
// only meaningful as a pair: pointing at a different endpoint while the model
// stays pinned is a knob that cannot turn.
//
// The 512 tier is chosen deliberately for this hardware: it returns 688x384,
// which sits under 2x the 800x480 panel and so clears image_processor's
// forced-1/2 decode gate -- the image is decoded once at full size (0.79 MB
// RGB888) and upscaled 1.25x, with no down-then-up resample chain.
#define AI_URL_MAX_LEN 256
#define AI_MODEL_MAX_LEN 128
#define DEFAULT_AI_URL "https://openrouter.ai/api/v1/images"
#define DEFAULT_AI_IMAGE_MODEL "bytedance-seed/seedream-5-0-flash"
#define AI_GEN_ASPECT_RATIO "16:9"
#define AI_GEN_RESOLUTION "512"
// Generation is the slow half of the job -- 16 s measured on the flash tier, 77 s
// on seedream-5-0-pro, which is why this is not 60 s. It is a per-socket
// operation timeout, so a slow-but-progressing response still completes.
#define AI_GEN_TIMEOUT_MS 120000
// Ceiling on the JSON body, a guard against a runaway read rather than a memory
// saving: the configured tier answers ~840 KB. Headroom here decides the largest
// image tier the device can accept.
#define AI_GEN_RESPONSE_MAX_LEN (3 * 1024 * 1024)
#define AI_GEN_TASK_STACK 16384

// Upper bound a rotation tick will wait on the AI pipeline. The deep-sleep path
// sleeps as soon as trigger_image_rotation() returns, so the AI branch blocks on
// its worker and needs a ceiling: the cycle measures ~54 s, which leaves room for
// a slower model without letting a wedged socket hold the device awake forever.
#define AI_ROTATION_TIMEOUT_MS 180000

// Creative direction: free text folded into the prompt writer's system prompt as
// the owner's standing instructions -- the place to put the fixed parts (where
// the frame is, what style to lean to, what to avoid) so they can be edited by
// hand while the daily cycle stays autonomous.
#define AI_DIRECTION_MAX_LEN 512
#define NVS_AI_DIRECTION_KEY "ai_dir"
#define NVS_AI_URL_KEY "ai_url"
#define NVS_AI_IMAGE_MODEL_KEY "ai_model"
// The key the frame uses. Separate from the legacy openai_key slot, which the
// browser's upload dialog uses for client-side generation -- different consumer,
// different key. Migrated from openai_key at boot when this one is unset, so an
// already-provisioned frame keeps working.
#define AI_API_KEY_MAX_LEN 256
#define NVS_AI_API_KEY_KEY "ai_apikey"
#define NVS_LEGACY_OPENAI_KEY "openai_key"

// The prompt writer's endpoint and model are config for the same reason the
// image pair is: they are only meaningful together. Defaults preserve the
// previous compile-time behaviour.
#define AI_WRITER_URL_MAX_LEN 256
#define AI_WRITER_MODEL_MAX_LEN 128
#define DEFAULT_AI_WRITER_URL "https://openrouter.ai/api/v1/chat/completions"
#define DEFAULT_AI_WRITER_MODEL "google/gemini-3.1-flash-lite"
#define NVS_AI_WRITER_URL_KEY "ai_wurl"
#define NVS_AI_WRITER_MODEL_KEY "ai_wmodel"

// The coarse area the prompt is written for ("the North Downs, Surrey"), as
// distinct from the precise coordinates the weather API needs. They are kept
// together in the UI because they must agree, but they are different things.
#define AI_PLACE_MAX_LEN 128
// Fallback for a factory-fresh device: the compiled template contains {{place}},
// and compose_prompt refuses to send an unresolved placeholder, so there has to
// be a value even before anyone configures one. Matches the lab's default.
#define DEFAULT_AI_PLACE "Surrey, England"
#define NVS_AI_PLACE_KEY "ai_place"

// The editable prompt template. Stored as an NVS blob rather than a string:
// strings cap at 4000 bytes, and the guard on a composed prompt is 8192. Empty
// means "use the compiled default", so a frame reset or a bad save always has
// something sane to fall back to.
#define AI_TEMPLATE_MAX_LEN 8192
#define NVS_AI_TEMPLATE_KEY "ai_tmpl"

// The model preference order, comma-separated ids. The HEAD is the model in use;
// choosing a model moves it to the head, and a failure moves it to the tail. One
// ordered list, so there is no separate "active" field to drift out of step.
#define AI_MODEL_ORDER_MAX_LEN 1024
#define AI_MODEL_ORDER_MAX_ENTRIES 8
#define NVS_AI_MODEL_ORDER_KEY "ai_order"
#define DEFAULT_AI_MODEL_ORDER                                                                     \
    "bytedance-seed/seedream-5-0-pro,bytedance-seed/seedream-5-0-flash,"                            \
    "google/gemini-3.1-flash-image,google/gemini-3.1-flash-lite-image,"                             \
    "microsoft/mai-image-2.6-flash,krea/krea-2-medium-turbo"

// Consecutive days with no successful autonomous generation before the panel is
// replaced by a warning. Seven is long enough to ride out an outage and short
// enough that the frame cannot sit stale for a fortnight without telling anyone.
#define AI_FAIL_STREAK_WARN_DAYS 7
#define NVS_AI_FAIL_STREAK_KEY "ai_fdays"
// Consecutive transient failures (connect/TLS/429/5xx) before the current model
// is demoted. A capability failure -- output too large to decode, parameters
// rejected -- demotes immediately, because retrying that model is pointless.
// The distinction matters: demoting on a single TLS blip would discard a model
// that works, which is exactly what happened once already.
#define NVS_AI_MODEL_FAILS_KEY "ai_fails"
#define IMAGE_MODEL_DEMOTE_AFTER_FAILS 3

// Weather for the daily prompt (weather.c). This is the weather service's
// location, not a parameter of the generation itself.
//
// timezone=auto makes Open-Meteo resolve the zone from the coordinates, so the
// day it labels as index 0 is correct even if our own clock has not synced --
// only the *server's* calendar decides which day we get.
#define OPEN_METEO_FORECAST_URL "https://api.open-meteo.com/v1/forecast"
// Their one condition of use: identify the client. A generic or shared user agent
// is how a whole netblock ends up throttled, so this is not optional courtesy.
#define WEATHER_USER_AGENT "esp32-photoframe/1.0 (+https://github.com/aitjcize/esp32-photoframe)"
#define WEATHER_TIMEOUT_MS 15000
#define DEFAULT_WEATHER_LATITUDE 51.2361    // Guildford, Surrey
#define DEFAULT_WEATHER_LONGITUDE -0.5762
#define WEATHER_LATITUDE_MIN -90.0
#define WEATHER_LATITUDE_MAX 90.0
#define WEATHER_LONGITUDE_MIN -180.0
#define WEATHER_LONGITUDE_MAX 180.0
#define NVS_WEATHER_LATITUDE_KEY "wx_lat"
#define NVS_WEATHER_LONGITUDE_KEY "wx_lon"

// The prompt writer (ai_prompt.c) -- deliberately a different, far cheaper model
// than the image model. Measured: ~$0.0002 per call against ~$0.045 for the image
// it goes on to produce, returning in about 2 s.
#define AI_PROMPT_URL "https://openrouter.ai/api/v1/chat/completions"
#define AI_PROMPT_MODEL "google/gemini-3.1-flash-lite"
#define AI_PROMPT_MAX_TOKENS 220
#define AI_PROMPT_TEMPERATURE 1.0
#define AI_PROMPT_TIMEOUT_MS 30000
// Ceiling for the composed system prompt (guidance + owner direction). The prompt
// is heap-allocated at its exact size, so this is not a truncation limit -- it is
// a guard that refuses to send something absurd rather than discovering it later.
#define AI_PROMPT_SYSTEM_MAX 8192

// OTA Configuration
// The release channel this fork's OTA client reads. Pointed at our fork: the
// upstream one would deliver firmware without any of this work.
#define GITHUB_API_URL "https://api.github.com/repos/Ben-Pollard/esp32-photoframe/releases/latest"
#define OTA_CHECK_INTERVAL_MS (24 * 60 * 60 * 1000)  // 24 hours

#endif
