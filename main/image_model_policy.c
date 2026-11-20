#include "image_model_policy.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "config_manager.h"
#include "esp_log.h"

static const char *TAG = "image_model";

// The preference order lives in NVS, not here, because it is the thing the user
// can reorder and the thing a failure rewrites. This module only parses it and
// applies the two operations that change it.
#define MAX_ENTRIES AI_MODEL_ORDER_MAX_ENTRIES

static char order_buf[AI_MODEL_ORDER_MAX_LEN];
static char *order[MAX_ENTRIES];
static int order_n;

static void load_order(void)
{
    snprintf(order_buf, sizeof(order_buf), "%s", config_manager_get_ai_model_order());
    order_n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(order_buf, ",", &save); tok && order_n < MAX_ENTRIES;
         tok = strtok_r(NULL, ",", &save)) {
        // trim surrounding spaces
        while (*tok == ' ') {
            tok++;
        }
        size_t len = strlen(tok);
        while (len > 0 && tok[len - 1] == ' ') {
            tok[--len] = '\0';
        }
        if (len > 0) {
            order[order_n++] = tok;
        }
    }
}

const char *const *image_model_candidates(int *count)
{
    load_order();
    if (count) {
        *count = order_n;
    }
    return (const char *const *) order;
}

const char *image_model_next(const char *current)
{
    load_order();
    if (order_n == 0) {
        return current ? current : "";
    }
    if (!current) {
        return order[0];
    }
    for (int i = 0; i < order_n; i++) {
        if (strcmp(order[i], current) == 0) {
            return (i + 1 < order_n) ? order[i + 1] : order[i];
        }
    }
    return order[0];
}

bool image_model_should_demote(int consecutive_transient)
{
    return consecutive_transient >= IMAGE_MODEL_DEMOTE_AFTER_FAILS;
}

// Writes the current order back, optionally moving `model` to the head or the
// tail. One code path for both operations, because "choose" and "demote" are the
// same edit with a different destination.
static void rewrite_order(const char *model, bool to_head, const char *why)
{
    load_order();
    if (order_n == 0) {
        return;
    }

    char csv[AI_MODEL_ORDER_MAX_LEN];
    size_t n = 0;
    csv[0] = '\0';

    if (!model) {
        // No-op rewrite.
        snprintf(csv, sizeof(csv), "%s", config_manager_get_ai_model_order());
    } else {
        if (to_head) {
            n += (size_t) snprintf(csv + n, sizeof(csv) - n, "%s", model);
        }
        for (int i = 0; i < order_n; i++) {
            if (strcmp(order[i], model) == 0) {
                continue;
            }
            n += (size_t) snprintf(csv + n, sizeof(csv) - n, "%s%s", n ? "," : "", order[i]);
            if (n >= sizeof(csv) - 1) {
                break;
            }
        }
        if (!to_head) {
            n += (size_t) snprintf(csv + n, sizeof(csv) - n, "%s%s", n ? "," : "", model);
        }
    }

    ESP_LOGW(TAG, "%s: %s", why, csv);
    esp_err_t err = config_manager_set_ai_model_order(csv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not persist the model order (%s)", esp_err_to_name(err));
    }
}

void image_model_on_failure(image_model_failure_t kind)
{
    const char *current = config_manager_get_ai_image_model();

    switch (kind) {
    case IMAGE_FAIL_CAPABILITY:
        // This model cannot serve the device. Move it to the tail so the next
        // one becomes the head, and the user's remaining order is preserved.
        rewrite_order(current, false, "model output unusable -- moving to the end");
        config_manager_set_ai_model_fails(0);
        break;
    case IMAGE_FAIL_TRANSIENT: {
        int fails = config_manager_get_ai_model_fails() + 1;
        if (image_model_should_demote(fails)) {
            rewrite_order(current, false, "repeated connection failures -- moving to the end");
            config_manager_set_ai_model_fails(0);
        } else {
            config_manager_set_ai_model_fails(fails);
            ESP_LOGW(TAG, "transient failure %d of %d before moving the model down",
                     fails, IMAGE_MODEL_DEMOTE_AFTER_FAILS);
        }
        break;
    }
    case IMAGE_FAIL_NEUTRAL:
        break;
    }
}

void image_model_on_success(void)
{
    if (config_manager_get_ai_model_fails() != 0) {
        config_manager_set_ai_model_fails(0);
    }
}

void image_model_choose(const char *model)
{
    if (!model || !model[0]) {
        return;
    }
    rewrite_order(model, true, "chosen by the owner -- moving to the head");
}
