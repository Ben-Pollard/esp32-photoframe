#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t display_manager_init(void);
esp_err_t display_manager_show_image(const char *filename);

esp_err_t display_manager_show_calibration(void);
esp_err_t display_manager_clear(void);
bool display_manager_is_busy(void);
void display_manager_rotate_from_storage(void);
const char *display_manager_get_current_image(void);
void display_manager_initialize_paint(void);

/**
 * @brief Display an RGB buffer directly on the e-paper display
 *
 * This function takes an already-processed RGB buffer (with colors matching
 * the 6-color palette) and displays it directly. This is more efficient for
 * SD-card-less systems where no file I/O is needed.
 *
 * @param rgb_buffer RGB888 buffer (3 bytes per pixel, already dithered to palette)
 * @param width Image width
 * @param height Image height
 * @return esp_err_t ESP_OK on success
 */
esp_err_t display_manager_show_rgb_buffer(const uint8_t *rgb_buffer, int width, int height);

/**
 * @brief Row-streaming variant of display_manager_show_rgb_buffer
 *
 * begin acquires the display and clears the buffer; push paints one RGB row
 * (rows must arrive top to bottom); end refreshes the panel (when show is
 * true) and releases the display. After a successful begin, end MUST be
 * called on every path. Used by the image processor to display panel-size
 * images without materializing a full RGB frame.
 *
 * end publishes what the display shows (see display_publish_t) and may
 * snapshot the finished frame; pass NULL for an anonymous buffer display.
 */
typedef struct {
    // Logical name recorded as the current image so /api/current_image can
    // resolve it; the named file need not exist yet. NULL clears the record.
    const char *display_name;
    // When set, gzip the finished 4bpp frame to this path as .epdgz before
    // releasing the display (album snapshot)
    const char *save_path;
    // Recorded instead of display_name -- atomically, under the display
    // mutex -- when the snapshot fails, so the link never points at a
    // missing album entry
    const char *fallback_name;
    // Optional caption burnt into the frame after dithering, e.g.
    // "20th Nov | Clear sky". Drawn on a light plate in flat ink so it stays
    // crisp: anything anti-aliased or dithered would blur on this panel.
    const char *overlay_label;
} display_publish_t;

esp_err_t display_manager_begin_rgb_stream(void);
esp_err_t display_manager_push_rgb_row(int y, const uint8_t *rgb_row, int width);
// Column variant for rotated streaming: paints pixels (x, 0..height-1). Used
// when rows are produced in processing-space order on a rotated orientation.
esp_err_t display_manager_push_rgb_column(int x, const uint8_t *rgb_col, int height);
// Returns ESP_ERR_NOT_FINISHED when the display succeeded but the snapshot
// failed (the fallback_name, when given, has been published in its place).
esp_err_t display_manager_end_rgb_stream(bool show, const display_publish_t *pub);

/**
 * @brief Replace the panel with a plain message (headline + two detail lines).
 *
 * Used when the frame has failed to produce a picture for long enough that
 * silently showing a stale one would mislead. Rendered in the same flat bitmap
 * font as the date plate, so the text stays hard-edged.
 */
esp_err_t display_manager_show_warning(const char *headline, const char *line1, const char *line2);

#endif
