#ifndef AI_PROMPT_H
#define AI_PROMPT_H

#include "esp_err.h"

// Turns "what today is like" into an image prompt using a cheap text model.
//
// This is the cheap half of the pipeline by a wide margin: a 250-in/90-out call
// is a few ten-thousandths of a dollar, roughly 1/200th of the image it goes on
// to generate, and it completes in about two seconds. It is not worth
// optimising.
//
// The system prompt stays deliberately thin -- telling the model the target is a
// colour e-ink display is enough to steer it, and anything more constraining
// starts flattening the creativity that makes the daily picture worth looking at.
// The owner's standing instructions (config `ai_direction`, edited from the UI)
// carry the fixed parts: place, style, things to prefer or avoid.

/**
 * @brief Ask the text model for one image prompt.
 *
 * @param day_context  Plain-language description of the day, normally from
 *                   weather_day_summary(). May be NULL or empty, in which case
 *                   the model writes from the standing instructions alone.
 * @param direction  Owner's standing instructions from config. May be NULL or
 *                   empty.
 * @param out_prompt Set to a malloc'd prompt on ESP_OK; caller frees.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM, or ESP_FAIL with the
 *         reason left in ai_prompt_last_error().
 */
esp_err_t ai_prompt_write(const char *day_context, const char *direction, char **out_prompt);

/**
 * @brief Reason from the most recent ai_prompt_write() failure ("" if none).
 */
const char *ai_prompt_last_error(void);

/**
 * @brief The built-in prompt template, for reset and for showing a diff in the UI.
 *
 * Not the template in use -- that may be a saved one; see
 * config_manager_get_ai_template().
 */
const char *ai_prompt_default_template(void);

#endif
