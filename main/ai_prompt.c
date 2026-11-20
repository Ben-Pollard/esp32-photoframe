#include "ai_prompt.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "config_manager.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "prompt_template_default.h"

static const char *TAG = "ai_prompt";

#define PROMPT_RESPONSE_MAX 8192

static char prompt_last_error[192] = {0};

const char *ai_prompt_last_error(void)
{
    return prompt_last_error;
}

const char *ai_prompt_default_template(void)
{
    return PROMPT_TEMPLATE_DEFAULT;
}

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(prompt_last_error, sizeof(prompt_last_error), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "%s", prompt_last_error);
}

// Duplicates a formatted string at exactly the size it needs. The composed
// prompt used to sit in a fixed stack buffer, where snprintf clipped it silently
// and dropped the tail of the instructions with no error at all. Sizing it first
// removes that whole class of failure, and keeps ~3 KB off the ai_gen task stack.
static char *heap_fmt(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        return NULL;
    }
    char *buf = malloc((size_t)need + 1);
    if (!buf) {
        return NULL;
    }
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)need + 1, fmt, ap);
    va_end(ap);
    return buf;
}

// The prompt text is NOT compiled in by hand. It is generated from
// docs/prompt_template.md by scripts/gen_prompt_template.py, so the device and
// the lab send the same words and cannot quietly diverge. See
// prompt_template_default.h.
static const char WEATHER_PLACEHOLDER[] = "{{weather}}";

// Fills the day's data block into the template. The template owns all wording;
// this only substitutes, exactly as the lab's fill() does.
static char *compose_prompt(const char *day_context, const char *direction)
{
    // A saved template wins; the compiled default is the fallback, so a bad or
    // cleared template can never leave the frame with nothing to send.
    const char *tpl = config_manager_get_ai_template();
    if (!tpl) {
        tpl = PROMPT_TEMPLATE_DEFAULT;
    }
    const char *at = strstr(tpl, WEATHER_PLACEHOLDER);
    if (!at) {
        // The generator would have failed loudly; refuse rather than send a
        // template with no weather slot to fill.
        fail("prompt template has no %s placeholder", WEATHER_PLACEHOLDER);
        return NULL;
    }
    const char *ctx = (day_context && day_context[0] != '\0')
                          ? day_context
                          : "No weather information is available for today.";
    size_t head = (size_t)(at - tpl);
    const char *tail = at + strlen(WEATHER_PLACEHOLDER);
    bool has_direction = direction && direction[0] != '\0';
    char *composed = heap_fmt("%.*s%s%s%s%s", (int)head, tpl, ctx, tail,
                              has_direction ? "\n\nStanding instructions from the owner of the "
                                              "frame, which take precedence:\n"
                                            : "",
                              has_direction ? direction : "");
    if (!composed) {
        return NULL;
    }

    // {{place}} is optional: a template may legitimately hard-code where the
    // frame hangs. If it is used, the place must be configured -- an unresolved
    // placeholder sent as prose is a silent failure.
    for (int guard = 0; guard < 8; guard++) {
        const char *ph = strstr(composed, "{{place}}");
        if (!ph) {
            break;
        }
        const char *place = config_manager_get_ai_place();
        if (!place || place[0] == '\0') {
            free(composed);
            fail("prompt template uses {{place}} but no place is configured");
            return NULL;
        }
        size_t before = (size_t)(ph - composed);
        char *next = heap_fmt("%.*s%s%s", (int)before, composed, place,
                              ph + strlen("{{place}}"));
        free(composed);
        if (!next) {
            return NULL;
        }
        composed = next;
    }

    // Anything else left in braces is a typo, and would otherwise reach the
    // model as literal text.
    if (strstr(composed, "{{") != NULL) {
        free(composed);
        fail("prompt template still contains an unresolved placeholder");
        return NULL;
    }
    return composed;
}

// Compacts a model's answer in place: drops leading/trailing whitespace and one
// layer of wrapping quotes, since models habitually quote the thing they were
// asked to produce. Operates on the buffer it is given so the caller keeps one
// allocation.
static void clean_in_place(char *s)
{
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') {
        start++;
    }

    size_t len = strlen(start);
    while (len > 0 &&
           (start[len - 1] == ' ' || start[len - 1] == '\n' || start[len - 1] == '\r' ||
            start[len - 1] == '\t')) {
        start[--len] = '\0';
    }

    if (len >= 2 && ((start[0] == '"' && start[len - 1] == '"') ||
                    (start[0] == '\'' && start[len - 1] == '\''))) {
        start[len - 1] = '\0';
        start++;
    }

    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
}

esp_err_t ai_prompt_write(const char *day_context, const char *direction, char **out_prompt)
{
    if (!out_prompt) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_prompt = NULL;
    prompt_last_error[0] = '\0';

    const char *key = config_manager_get_ai_api_key();
    if (!key || key[0] == '\0') {
        fail("No AI key configured (Settings > AI Generation)");
        return ESP_ERR_INVALID_STATE;
    }

    // cJSON builds the body so the owner's direction text and the forecast are
    // escaped correctly however they are punctuated.
    cJSON *req = cJSON_CreateObject();
    if (!req) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(req, "model", config_manager_get_ai_writer_model());
    cJSON_AddNumberToObject(req, "max_tokens", AI_PROMPT_MAX_TOKENS);
    cJSON_AddNumberToObject(req, "temperature", AI_PROMPT_TEMPERATURE);

    cJSON *messages = cJSON_AddArrayToObject(req, "messages");
    if (!messages) {
        cJSON_Delete(req);
        return ESP_ERR_NO_MEM;
    }

    // ONE message. This is not a chat: the writer is a single turn, and some
    // OpenAI-compatible endpoints ignore a system role entirely, so relying on
    // one would make the prompt non-portable. The lab sends the identical shape.
    char *composed = compose_prompt(day_context, direction);
    if (!composed) {
        cJSON_Delete(req);
        if (prompt_last_error[0] == '\0') {
            fail("Out of memory composing the prompt");
        }
        return ESP_ERR_NO_MEM;
    }
    size_t composed_len = strlen(composed);
    if (composed_len > AI_PROMPT_SYSTEM_MAX) {
        free(composed);
        cJSON_Delete(req);
        fail("Prompt is %u bytes, over the %d byte limit", (unsigned)composed_len,
             AI_PROMPT_SYSTEM_MAX);
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON *usr_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(usr_msg, "role", "user");
    cJSON_AddStringToObject(usr_msg, "content", composed);
    cJSON_AddItemToArray(messages, usr_msg);
    ESP_LOGI(TAG, "Prompt %u bytes (one message)", (unsigned)composed_len);
    free(composed);

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t cfg = {
        .url = config_manager_get_ai_writer_url(),
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = AI_PROMPT_TIMEOUT_MS,
        .buffer_size = 2048,
        .user_agent = "ESP32-PhotoFrame",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        fail("Failed to initialise HTTP client");
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    char auth[AI_API_KEY_MAX_LEN + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", key);
    esp_http_client_set_header(client, "Authorization", auth);

    esp_err_t err = ESP_FAIL;
    char *resp = malloc(PROMPT_RESPONSE_MAX);
    bool opened = false;
    if (!resp) {
        esp_http_client_cleanup(client);
        free(body);
        return ESP_ERR_NO_MEM;
    }

    int body_len = (int)strlen(body);
    if (esp_http_client_open(client, body_len) != ESP_OK) {
        fail("Failed to open %s", config_manager_get_ai_writer_url());
        goto cleanup;
    }
    opened = true;
    if (esp_http_client_write(client, body, body_len) < body_len) {
        fail("Failed to send prompt request");
        goto cleanup;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    size_t total = 0;
    while (total + 1 < PROMPT_RESPONSE_MAX) {
        int n = esp_http_client_read(client, resp + total, (int)(PROMPT_RESPONSE_MAX - total - 1));
        if (n < 0) {
            fail("Failed to read prompt response");
            goto cleanup;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
    }
    resp[total] = '\0';

    if (status != 200) {
        char detail[160] = "";
        cJSON *err_root = (total > 0 && total < 4096) ? cJSON_ParseWithLength(resp, total) : NULL;
        if (err_root) {
            cJSON *e = cJSON_GetObjectItemCaseSensitive(err_root, "error");
            cJSON *m = e ? cJSON_GetObjectItemCaseSensitive(e, "message") : NULL;
            if (cJSON_IsString(m)) {
                snprintf(detail, sizeof(detail), ": %s", cJSON_GetStringValue(m));
            }
            cJSON_Delete(err_root);
        }
        fail("Prompt model returned HTTP %d%s", status, detail);
        goto cleanup;
    }

    cJSON *root = cJSON_ParseWithLength(resp, total);
    if (!root) {
        fail("Prompt response was not valid JSON");
        goto cleanup;
    }
    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    const cJSON *first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    const cJSON *msg = first ? cJSON_GetObjectItemCaseSensitive(first, "message") : NULL;
    const cJSON *content = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
    if (!cJSON_IsString(content) || cJSON_GetStringValue(content)[0] == '\0') {
        fail("Prompt response carried no message content");
        cJSON_Delete(root);
        goto cleanup;
    }

    // Copy out before the DOM is freed; the content string does not outlive it.
    char *prompt = strdup(cJSON_GetStringValue(content));
    cJSON_Delete(root);
    if (!prompt) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    clean_in_place(prompt);
    if (prompt[0] == '\0') {
        free(prompt);
        fail("Prompt was empty after cleanup");
        goto cleanup;
    }

    *out_prompt = prompt;
    err = ESP_OK;
    ESP_LOGI(TAG, "Wrote prompt of %u chars", (unsigned)strlen(prompt));

cleanup:
    if (opened) {
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    free(resp);
    free(body);
    return err;
}
