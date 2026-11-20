#ifndef AI_GEN_H
#define AI_GEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// On-device image generation: POST a prompt to the generation endpoint, decode the
// base64 PNG it returns, and stream it straight to the panel. Nothing is stored;
// the display is the only sink (display_publish_t is NULL -- anonymous).
//
// Generation is slow (6-12 s measured for the configured model, plus a ~15 s
// panel refresh), so it never runs on the httpd task: ai_gen_start() spawns a
// worker and returns immediately, and callers poll ai_gen_get_status(), exactly
// the shape ota_manager.c uses.
//
// State machine (SM-R1 -- no implicit phases). Transitions, all driven by the
// worker task; IDLE/terminal states are entered from the start guard:
//
//   IDLE -------- start() ----------> REQUESTING        (prompt supplied)
//   IDLE -------- start(NULL) ------> WEATHER           (autonomous)
//   WEATHER ------ forecast ok ----> PROMPTING
//   WEATHER ------ anything else --> ERROR
//   PROMPTING ----- prompt ok -----> REQUESTING
//   PROMPTING ----- anything else -> ERROR
//   REQUESTING -- headers 200 -----> DECODING
//   REQUESTING -- anything else ---> ERROR
//   DECODING ----- b64+format ok --> DISPLAYING
//   DECODING ----- anything else --> ERROR
//   DISPLAYING --- panel accepts ---> SUCCESS
//   DISPLAYING --- panel rejects --> ERROR
//   SUCCESS/ERROR -- start() -----> REQUESTING   (terminal states re-arm)
//
// A start() while WEATHER/PROMPTING/REQUESTING/DECODING/DISPLAYING is refused
// with ESP_ERR_INVALID_STATE; no queueing, matching the busy-refusal style used
// for displays elsewhere.
typedef enum {
    AI_STATE_IDLE,
    AI_STATE_WEATHER,     // fetching today's forecast (autonomous runs only)
    AI_STATE_PROMPTING,   // asking the text model for the image prompt
    AI_STATE_REQUESTING,  // asking the image model
    AI_STATE_DECODING,
    AI_STATE_DISPLAYING,
    AI_STATE_SUCCESS,
    AI_STATE_ERROR
} ai_state_t;

typedef struct {
    ai_state_t state;
    char error[192];
    int http_status;         // last response status code, 0 if none received
    size_t response_bytes;   // JSON body length
    size_t image_bytes;      // decoded image length
    int64_t elapsed_ms;      // monotonic, set when the worker finishes
} ai_gen_status_t;

/**
 * @brief Create the status mutex. Safe to call once from app_main.
 */
esp_err_t ai_gen_init(void);

/**
 * @brief Kick off a generation + display on a worker task.
 *
 * @param prompt Text prompt, UTF-8. Pass NULL for an autonomous run: the worker
 *               fetches today's forecast, has the text model write the prompt
 *               from it plus the configured direction, and generates from that.
 *               An empty string is rejected rather than treated as autonomous, so
 *               a caller bug cannot silently change what gets generated.
 * @return ESP_OK started; ESP_ERR_INVALID_STATE if one is already running or the
 *         panel is busy; ESP_ERR_NO_MEM if the worker could not be created.
 */
esp_err_t ai_gen_start(const char *prompt);

/**
 * @brief Copy the current status (thread-safe).
 */
void ai_gen_get_status(ai_gen_status_t *status);

/**
 * @brief True while a run is in flight (any non-idle, non-terminal state).
 *
 * Prefer this over comparing states at the call site: the set of active states
 * changes as the pipeline grows, and duplicating that list is how a caller ends up
 * calling the device free while it is mid-weather-fetch.
 */
bool ai_gen_is_busy(void);

/**
 * @brief Human-readable name for an ai_state_t ("idle", "requesting", ...).
 */
const char *ai_gen_state_name(ai_state_t state);

#endif
