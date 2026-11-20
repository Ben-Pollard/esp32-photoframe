#ifndef IMAGE_MODEL_POLICY_H
#define IMAGE_MODEL_POLICY_H

#include <stdbool.h>

// Choosing which image model to use when the provider landscape shifts under us.
//
// The API does not expose a model's output encoding or dimensions, so capability
// can only be discovered by trying. The stored model is therefore the head of
// the preference list: it is tried first every day, and the list is only walked
// when it actually fails. A model that works is never probed past, so a working
// setup costs exactly one request per day no matter how far down the list it
// started.
typedef enum {
    // The model cannot serve this device: response too large to decode,
    // unsupported parameters, undecodable payload. Retrying it is pointless, so
    // demote immediately.
    IMAGE_FAIL_CAPABILITY,
    // The model was not at fault: connect/TLS failure, timeout, 429, 5xx.
    // Counted, and demoted only after IMAGE_MODEL_DEMOTE_AFTER_FAILS in a row.
    IMAGE_FAIL_TRANSIENT,
    // Neither: bad credentials, exhausted credit, device-side problem. Leave the
    // choice alone -- swapping models will not fix any of these.
    IMAGE_FAIL_NEUTRAL,
} image_model_failure_t;

// The ordered candidates. Exposed so a caller can report or verify the list;
// the owner sets the order in image_model_policy.c.
const char *const *image_model_candidates(int *count);

// The next candidate after `current`, or `current` itself if it is last or not
// present. Never wraps: walking back to the top would re-probe what already
// failed.
const char *image_model_next(const char *current);

// Whether a transient failure count warrants demoting the model.
bool image_model_should_demote(int consecutive_transient);

// Record an outcome for the model currently configured. Demotion persists the
// successor, so tomorrow starts on the model that worked.
void image_model_on_failure(image_model_failure_t kind);
void image_model_on_success(void);

// Move a model to the head of the order -- i.e. "use this one from now on".
void image_model_choose(const char *model);

#endif
