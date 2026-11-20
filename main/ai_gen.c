#include "ai_gen.h"
#include "image_model_policy.h"

#include <stdlib.h>
#include <string.h>

#include "ai_prompt.h"
#include "cJSON.h"
#include "config.h"
#include "config_manager.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "image_processor.h"
#include "power_manager.h"
#include "processing_settings.h"
#include "weather.h"

static const char *TAG = "ai_gen";

static SemaphoreHandle_t ai_status_mutex = NULL;
static ai_gen_status_t ai_status = {0};

static void ai_lock(void)
{
    if (ai_status_mutex) {
        xSemaphoreTake(ai_status_mutex, portMAX_DELAY);
    }
}

static void ai_unlock(void)
{
    if (ai_status_mutex) {
        xSemaphoreGive(ai_status_mutex);
    }
}

static void set_state(ai_state_t state, const char *error_msg)
{
    ai_lock();
    ai_status.state = state;
    if (error_msg) {
        snprintf(ai_status.error, sizeof(ai_status.error), "%s", error_msg);
    } else {
        ai_status.error[0] = '\0';
    }
    ai_unlock();
}

const char *ai_gen_state_name(ai_state_t state)
{
    switch (state) {
    case AI_STATE_IDLE:
        return "idle";
    case AI_STATE_WEATHER:
        return "weather";
    case AI_STATE_PROMPTING:
        return "prompting";
    case AI_STATE_REQUESTING:
        return "requesting";
    case AI_STATE_DECODING:
        return "decoding";
    case AI_STATE_DISPLAYING:
        return "displaying";
    case AI_STATE_SUCCESS:
        return "success";
    case AI_STATE_ERROR:
        return "error";
    }
    return "unknown";
}

esp_err_t ai_gen_init(void)
{
    if (!ai_status_mutex) {
        ai_status_mutex = xSemaphoreCreateMutex();
        if (!ai_status_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    ai_lock();
    memset(&ai_status, 0, sizeof(ai_status));
    ai_status.state = AI_STATE_IDLE;
    ai_unlock();
    return ESP_OK;
}

void ai_gen_get_status(ai_gen_status_t *status)
{
    if (status && ai_status_mutex) {
        if (xSemaphoreTake(ai_status_mutex, portMAX_DELAY) == pdTRUE) {
            memcpy(status, &ai_status, sizeof(ai_gen_status_t));
            xSemaphoreGive(ai_status_mutex);
        }
    }
}

// base64 ---------------------------------------------------------------------

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Decodes a base64 span, skipping anything outside the alphabet (chunk boundaries
// and any line wrapping land here) and stopping at '='. Returns bytes written.
//
// Called with dst at the head of the response buffer and src partway into the same
// buffer: every 4 chars consumed emit 3 bytes, so the write cursor is strictly
// behind the read cursor throughout and the decode is safe in place. That avoids a
// second ~630 KB PSRAM allocation for the image.
static size_t b64_decode_span(const char *src, size_t src_len, uint8_t *dst, size_t dst_cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t out = 0;

    for (size_t i = 0; i < src_len; i++) {
        if (src[i] == '=') {
            break;
        }
        int v = b64_value(src[i]);
        if (v < 0) {
            continue;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out >= dst_cap) {
                return out;
            }
            dst[out++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return out;
}

// Locates the "b64_json" string value in the response body without parsing it --
// the payload is hundreds of KB and cJSON would build a DOM around it needlessly.
// Returns offsets into `json`, not new allocations.
static bool find_b64_json_field(const char *json, size_t len, size_t *out_off, size_t *out_len)
{
    static const char key[] = "\"b64_json\"";
    const char *end = json + len;

    const char *p = memmem(json, len, key, sizeof(key) - 1);
    if (!p) {
        return false;
    }
    p += sizeof(key) - 1;

    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++;
    }
    if (p >= end || *p != ':') {
        return false;
    }
    p++;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++;
    }
    if (p >= end || *p != '"') {
        return false;
    }
    p++;

    const char *start = p;
    while (p < end && *p != '"') {
        p++;
    }
    if (p >= end) {
        return false;  // unterminated string
    }

    *out_off = (size_t)(start - json);
    *out_len = (size_t)(p - start);
    return *out_len > 0;
}

// Pulls error.message out of a small error body, falling back to a bare status.
static void describe_error_body(const char *body, size_t len, int status, char *out, size_t out_cap)
{
    cJSON *root = (len > 0 && len < 4096) ? cJSON_ParseWithLength(body, len) : NULL;
    const char *msg = NULL;
    if (root) {
        cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (err) {
            cJSON *m = cJSON_GetObjectItemCaseSensitive(err, "message");
            if (cJSON_IsString(m)) {
                msg = cJSON_GetStringValue(m);
            }
        }
    }
    if (msg && msg[0]) {
        snprintf(out, out_cap, "HTTP %d: %s", status, msg);
    } else {
        snprintf(out, out_cap, "HTTP %d", status);
    }
    if (root) {
        cJSON_Delete(root);
    }
}

// worker ---------------------------------------------------------------------

static void ai_gen_task(void *arg)
{
    char *prompt = (char *)arg;  // NULL => autonomous: derive it from the weather
    const bool autonomous = (prompt == NULL);
    int64_t started_ms = esp_timer_get_time() / 1000;  // monotonic, not wall clock
    char *buf = NULL;
    esp_http_client_handle_t client = NULL;
    char *body = NULL;
    // The forecast day, kept for the caption: the label dates the picture it is
    // drawn on, not "today", so a stale image cannot be mistaken for a fresh one.
    weather_day_t day = {0};
    bool have_day = false;

    const char *key = config_manager_get_ai_api_key();
    if (!key || key[0] == '\0') {
        set_state(AI_STATE_ERROR, "No AI key configured (Settings > AI Generation)");
        goto done;
    }

    if (!prompt) {
        // Autonomous run: the forecast becomes a sentence, the sentence becomes a
        // prompt, the prompt becomes a picture. Both extra steps are seconds next
        // to the generation and refresh that follow, so folding them in here costs
        // nothing and keeps the whole daily cycle on the device.
        set_state(AI_STATE_WEATHER, NULL);
        // Open-Meteo is free and generous, and this is one request for the whole
        // day's data -- never a probe per field. Because the cycle runs in the
        // early morning, a generous backoff costs nothing anyone notices: a
        // minute, then ten. Any failure is retried except a misconfiguration
        // (no usable coordinates), which waiting cannot fix.
        static const int weather_backoff_s[] = {60, 600};
        esp_err_t werr;
        int tries = 0;
        for (;;) {
            werr = weather_get_today(&day);
            if (werr == ESP_OK || werr == ESP_ERR_INVALID_STATE ||
                tries >= (int)(sizeof(weather_backoff_s) / sizeof(weather_backoff_s[0]))) {
                break;
            }
            int wait_s = weather_backoff_s[tries++];
            ESP_LOGW(TAG, "Weather fetch failed (%s); retrying in %d s",
                     esp_err_to_name(werr), wait_s);
            for (int s = 0; s < wait_s; s++) {
                // Once a second, so the auto-sleep timer cannot put the frame
                // back down in the middle of its own retry window.
                power_manager_reset_sleep_timer();
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        if (werr != ESP_OK) {
            char detail[128];
            snprintf(detail, sizeof(detail), "Weather fetch failed: %s", esp_err_to_name(werr));
            set_state(AI_STATE_ERROR, detail);
            goto done;
        }

        have_day = true;
        char summary[640];
        weather_day_summary(&day, summary, sizeof(summary));
        ESP_LOGI(TAG, "Today: %s", summary);

        set_state(AI_STATE_PROMPTING, NULL);
        if (ai_prompt_write(summary, config_manager_get_ai_direction(), &prompt) != ESP_OK) {
            char detail[224];
            snprintf(detail, sizeof(detail), "Prompt writer failed: %s", ai_prompt_last_error());
            set_state(AI_STATE_ERROR, detail);
            goto done;
        }
        ESP_LOGI(TAG, "Prompt written: %s", prompt);
        power_manager_reset_sleep_timer();
    }

    set_state(AI_STATE_REQUESTING, NULL);

    // Image providers do not agree on parameters. Gemini's image models take
    // aspect_ratio and resolution; Seedream, Krea and others reject both with a
    // bare 400 rather than ignoring them -- which is exactly what stopped this
    // firmware generating with the model the lab settled on. Rather than keep a
    // list of which model supports what, send them and, if the provider refuses,
    // send again without. A 400 never returns an image and is not billed, so the
    // retry costs one request's latency and no money.
    esp_err_t err = ESP_OK;
    int64_t content_length = 0;
    int status = 0;
    bool with_options = true;

    for (int attempt = 0; attempt < 2; attempt++) {
        cJSON *req = cJSON_CreateObject();
        if (!req) {
            set_state(AI_STATE_ERROR, "Out of memory building request");
            goto done;
        }
        cJSON_AddStringToObject(req, "model", config_manager_get_ai_image_model());
        cJSON_AddStringToObject(req, "prompt", prompt);
        if (with_options) {
            cJSON_AddStringToObject(req, "aspect_ratio", AI_GEN_ASPECT_RATIO);
            cJSON_AddStringToObject(req, "resolution", AI_GEN_RESOLUTION);
        }
        body = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        if (!body) {
            set_state(AI_STATE_ERROR, "Out of memory serialising request");
            goto done;
        }

        esp_http_client_config_t cfg = {
            .url = config_manager_get_ai_url(),
            .method = HTTP_METHOD_POST,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = AI_GEN_TIMEOUT_MS,
            .buffer_size = 4096,
            .user_agent = "ESP32-PhotoFrame",
        };
        client = esp_http_client_init(&cfg);
        if (!client) {
            set_state(AI_STATE_ERROR, "Failed to initialise HTTP client");
            goto done;
        }
        esp_http_client_set_header(client, "Content-Type", "application/json");
        char auth_header[AI_API_KEY_MAX_LEN + 16];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", key);
        esp_http_client_set_header(client, "Authorization", auth_header);
        // Never log the body or the auth header: they carry the key and the prompt.

        int body_len = (int)strlen(body);
        err = esp_http_client_open(client, body_len);
        if (err != ESP_OK) {
            // Log the host explicitly: a TLS failure here reads identically whether
            // it came from this connection or another task's, and that ambiguity
            // cost a debugging cycle.
            ESP_LOGE(TAG, "Failed to open %s: %s", config_manager_get_ai_url(),
                     esp_err_to_name(err));
            char detail[128];
            snprintf(detail, sizeof(detail), "Failed to open connection: %s",
                     esp_err_to_name(err));
            set_state(AI_STATE_ERROR, detail);
            // Connect/TLS failure: the model is not at fault. Counted, and only
            // demoted if it keeps happening.
            image_model_on_failure(IMAGE_FAIL_TRANSIENT);
            goto done;
        }
        int written = esp_http_client_write(client, body, body_len);
        if (written < body_len) {
            set_state(AI_STATE_ERROR, "Failed to send request body");
            image_model_on_failure(IMAGE_FAIL_TRANSIENT);
            goto done;
        }

        content_length = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);

        if (status == 400 && with_options) {
            ESP_LOGW(TAG, "Provider refused aspect_ratio/resolution; retrying without them");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = NULL;
            free(body);
            body = NULL;
            with_options = false;
            continue;
        }
        break;
    }

    ai_lock();
    ai_status.http_status = status;
    ai_unlock();

    // One growing PSRAM buffer holds the whole JSON body; the decoded image is
    // then written over its head in place (b64_decode_span), so there is no
    // second large allocation and no copy.
    size_t capacity = 8192;
    size_t total = 0;
    buf = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM);
    if (!buf) {
        set_state(AI_STATE_ERROR, "Out of memory for response");
        goto done;
    }

    bool truncated = false;
    while (total + 1 < AI_GEN_RESPONSE_MAX_LEN) {
        if (total + 1 >= capacity) {
            size_t want = capacity * 2;
            if (want > AI_GEN_RESPONSE_MAX_LEN) {
                want = AI_GEN_RESPONSE_MAX_LEN;
            }
            char *grown = heap_caps_realloc(buf, want, MALLOC_CAP_SPIRAM);
            if (!grown) {
                set_state(AI_STATE_ERROR, "Failed to grow response buffer");
                goto done;
            }
            buf = grown;
            capacity = want;
        }
        int n = esp_http_client_read(client, buf + total, (int)(capacity - total - 1));
        if (n < 0) {
            set_state(AI_STATE_ERROR, "Failed to read response body");
            image_model_on_failure(IMAGE_FAIL_TRANSIENT);
            goto done;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
        // The read loop can run back-to-back for seconds on a fast link; yield on
        // each 32 KB boundary so the idle task gets a window.
        if ((total >> 15) != ((total - (size_t)n) >> 15)) {
            vTaskDelay(1);
        }
        // Keep the auto-sleep timer alive while we wait on a slow generation.
        power_manager_reset_sleep_timer();
    }
    if (total >= (size_t)AI_GEN_RESPONSE_MAX_LEN - 1) {
        truncated = true;
    }
    buf[total] = '\0';

    ai_lock();
    ai_status.response_bytes = total;
    ai_unlock();

    ESP_LOGI(TAG, "Generation response: HTTP %d, %u bytes%s", status, (unsigned)total,
             content_length == 0 ? " (chunked)" : "");

    if (status != 200) {
        char detail[224];
        describe_error_body(buf, total, status, detail, sizeof(detail));
        set_state(AI_STATE_ERROR, detail);
        // 400: the model cannot be driven with the parameters it was sent.
        // 404: the model no longer exists. Both argue for the next candidate.
        // 429 and 5xx are the provider's problem; 401/402/403 are ours. Neither
        // should cost the model its place.
        if (status == 400 || status == 404) {
            image_model_on_failure(IMAGE_FAIL_CAPABILITY);
        } else if (status < 0 || status == 429 || status >= 500) {
            // Negative status: the connection died before headers arrived. That
            // is the provider's problem, so it counts as transient rather than
            // costing the model its place.
            image_model_on_failure(IMAGE_FAIL_TRANSIENT);
        }
        goto done;
    }

    // Check truncation BEFORE looking for the image. A body cut off at the cap
    // has no complete b64_json field, so searching first reports "no image
    // payload" and hides the real cause -- which is exactly what happened on the
    // first Seedream run: 9.5 MiB of base64 against a 3 MiB cap read as a missing
    // field rather than an oversized response.
    if (truncated) {
        char detail[160];
        snprintf(detail, sizeof(detail),
                 "Response exceeded the %d MiB limit (truncated at %u bytes); "
                 "this model's image is too large for the device",
                 (int)(AI_GEN_RESPONSE_MAX_LEN / (1024 * 1024)), (unsigned)total);
        set_state(AI_STATE_ERROR, detail);
        image_model_on_failure(IMAGE_FAIL_CAPABILITY);
        goto done;
    }

    set_state(AI_STATE_DECODING, NULL);

    size_t b64_off = 0;
    size_t b64_len = 0;
    if (!find_b64_json_field(buf, total, &b64_off, &b64_len)) {
        set_state(AI_STATE_ERROR, "Response contained no b64_json image payload");
        image_model_on_failure(IMAGE_FAIL_CAPABILITY);
        goto done;
    }

    size_t img_len = b64_decode_span(buf + b64_off, b64_len, (uint8_t *)buf, total);
    if (img_len < 128) {
        set_state(AI_STATE_ERROR, "Decoded image payload too small");
        image_model_on_failure(IMAGE_FAIL_CAPABILITY);
        goto done;
    }
    ai_lock();
    ai_status.image_bytes = img_len;
    ai_unlock();
    ESP_LOGI(TAG, "Decoded image payload: %u bytes", (unsigned)img_len);

    image_format_t format = image_processor_detect_format_buffer((const uint8_t *)buf, img_len);
    if (format != IMAGE_FORMAT_PNG && format != IMAGE_FORMAT_JPG) {
        set_state(AI_STATE_ERROR, "Decoded payload is not a PNG or JPG image");
        image_model_on_failure(IMAGE_FAIL_CAPABILITY);
        goto done;
    }

    // The panel has been free when we started; a rotation or an upload may have
    // taken it while we waited on the API. Bail rather than block for the mutex.
    if (display_manager_is_busy()) {
        set_state(AI_STATE_ERROR, "Display became busy during generation");
        goto done;
    }

    set_state(AI_STATE_DISPLAYING, NULL);
    ESP_LOGI(TAG, "Streaming generated image to the display");

    // The caption is burnt in after dithering, in flat ink, on a light plate --
    // the only way text stays hard-edged on this panel. Only an autonomous run
    // has a forecast to caption, so a manual /api/ai/generate carries no label
    // rather than inventing a condition or guessing a date.
    char label[96] = "";
    if (have_day) {
        weather_label_text(&day, label, sizeof(label));
    }
    display_publish_t pub = {0};
    pub.overlay_label = label[0] ? label : NULL;
    if (label[0]) {
        ESP_LOGI(TAG, "Overlay label: %s", label);
    }

    // Anonymous display: nothing is filed into an album or the .current.* slot.
    err = image_processor_process_to_display((const uint8_t *)buf, img_len, format,
                                           processing_settings_get_dithering_algorithm(), &pub);
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
        char detail[224];
        const char *why = image_processor_get_last_error();
        snprintf(detail, sizeof(detail), "Display failed: %s",
                 why && why[0] ? why : esp_err_to_name(err));
        set_state(AI_STATE_ERROR, detail);
        goto done;
    }

    ai_lock();
    ai_status.elapsed_ms = (esp_timer_get_time() / 1000) - started_ms;
    ai_unlock();
    set_state(AI_STATE_SUCCESS, NULL);
    image_model_on_success();
    ESP_LOGI(TAG, "Generated image displayed in %lld ms", (long long)ai_status.elapsed_ms);

    power_manager_reset_sleep_timer();

done:
    // The streak counts scheduled cycles, not button presses: a user retrying
    // Generate now on a broken key must not fast-forward the warning.
    if (autonomous) {
        ai_lock();
        ai_state_t final_state = ai_status.state;
        ai_unlock();
        if (final_state == AI_STATE_SUCCESS) {
            if (config_manager_get_ai_fail_streak() != 0) {
                config_manager_set_ai_fail_streak(0);
            }
        } else if (final_state == AI_STATE_ERROR) {
            int days = config_manager_get_ai_fail_streak() + 1;
            config_manager_set_ai_fail_streak(days);
            if (days >= AI_FAIL_STREAK_WARN_DAYS) {
                // Leaving a stale picture on the wall implies it is today's.
                display_manager_show_warning("Generation failing",
                                             "Press BOOT, then open", "photoframe.local");
            }
        }
    }

    free(prompt);
    free(body);
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (buf) {
        heap_caps_free(buf);
    }
    vTaskDelete(NULL);
}

static bool ai_state_is_active(ai_state_t s)
{
    return s == AI_STATE_WEATHER || s == AI_STATE_PROMPTING || s == AI_STATE_REQUESTING ||
           s == AI_STATE_DECODING || s == AI_STATE_DISPLAYING;
}

bool ai_gen_is_busy(void)
{
    if (!ai_status_mutex) {
        return false;
    }
    ai_lock();
    bool busy = ai_state_is_active(ai_status.state);
    ai_unlock();
    return busy;
}

esp_err_t ai_gen_start(const char *prompt)
{
    // NULL means "work it out yourself". An empty string is a caller bug and is
    // rejected rather than quietly reinterpreted as an autonomous run.
    if (prompt && prompt[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ai_status_mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    if (display_manager_is_busy()) {
        ESP_LOGW(TAG, "Display busy, refusing generation");
        return ESP_ERR_INVALID_STATE;
    }

    char *copy = prompt ? strdup(prompt) : NULL;
    if (prompt && !copy) {
        return ESP_ERR_NO_MEM;
    }

    // Claim the run before the lock goes, rather than letting the worker claim it
    // when it first runs. Two reasons, both real:
    //
    //  - The worker is created at priority 5, the same priority as the rotation
    //    timer and button tasks, so it does not preempt them. A caller that polls
    //    ai_gen_is_busy() right after this returned would read whatever the
    //    *previous* run left behind -- a stale SUCCESS, which the deep-sleep
    //    path would record as a good wake and sleep on while this run had barely
    //    started.
    //  - Check-then-create is not atomic. Two callers (a cron tick and a POST)
    //    could both pass the guard and spawn two workers, each holding a ~840 KB
    //    buffer and both trying to drive one panel.
    //
    // Seeding the state here makes ai_gen_is_busy() true the instant this returns
    // ESP_OK. The worker sets the same state again, which is a no-op.
    ai_lock();
    if (ai_state_is_active(ai_status.state)) {
        ai_unlock();
        free(copy);
        ESP_LOGW(TAG, "Generation already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    ai_status.state = prompt ? AI_STATE_REQUESTING : AI_STATE_WEATHER;
    ai_status.error[0] = '\0';
    ai_status.http_status = 0;
    ai_status.response_bytes = 0;
    ai_status.image_bytes = 0;
    ai_status.elapsed_ms = 0;
    ai_unlock();

    BaseType_t ok = xTaskCreate(&ai_gen_task, "ai_gen", AI_GEN_TASK_STACK, copy, 5, NULL);
    if (ok != pdPASS) {
        free(copy);
        // Nothing else can have touched the state: the worker never existed.
        set_state(AI_STATE_IDLE, "Failed to create generation task");
        ESP_LOGE(TAG, "Failed to create generation task");
        return ESP_ERR_NO_MEM;
    }

    power_manager_reset_sleep_timer();
    return ESP_OK;
}
