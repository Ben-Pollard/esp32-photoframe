#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "weather.h"

// Pure formatting for the weather data block. Kept apart from weather.c so it
// can be compiled and tested without cJSON, HTTP or NVS: this is the text the
// prompt writer receives, and it is the one part of the weather path where a
// silent change alters what the model sees.

// Appends printf-formatted text at out[n] and returns the new length.
//
// The clamp matters: snprintf returns the length it *would* have written, so
// accumulating those returns lets n run past cap on a truncated write, and the
// following `cap - n` then underflows into an enormous size argument. Keeping n
// at most cap-1 means there is always room for the terminator.
static size_t append_at(char *out, size_t cap, size_t n, const char *fmt, ...)
{
    if (n + 1 >= cap) {
        return n;
    }

    va_list ap;
    va_start(ap, fmt);
    int wrote = vsnprintf(out + n, cap - n, fmt, ap);
    va_end(ap);

    if (wrote < 0) {
        return n;
    }
    n += (size_t)wrote;
    return (n > cap - 1) ? cap - 1 : n;
}

// English ordinal suffix. The 11th/12th/13th exception is the whole trap: a
// naive suffix rule yields "11st", and this string is on a prompt every day.
static const char *ordinal_suffix(int n)
{
    if (n % 100 >= 11 && n % 100 <= 13) {
        return "th";
    }
    switch (n % 10) {
    case 1:  return "st";
    case 2:  return "nd";
    case 3:  return "rd";
    default: return "th";
    }
}

static const char *MONTHS[] = {"January", "February", "March",     "April",   "May",      "June",
                               "July",    "August",   "September", "October", "November", "December"};

// One fixed-width label plus value line, matching the lab's column layout. Width
// 13 is the longest label ("precipitation"), and the lab pads to the longest, so
// these must stay in step or the device and the lab drift apart.
static size_t block_line(char *out, size_t cap, size_t n, const char *label, const char *fmt, ...)
{
    n = append_at(out, cap, n, "%-13s  ", label);
    if (n + 1 >= cap) {
        return n;
    }
    va_list ap;
    va_start(ap, fmt);
    int wrote = vsnprintf(out + n, cap - n, fmt, ap);
    va_end(ap);
    if (wrote < 0) {
        return n;
    }
    n += (size_t)wrote;
    if (n > cap - 1) {
        n = cap - 1;
    }
    return append_at(out, cap, n, "\n");
}

void weather_day_summary(const weather_day_t *d, char *out, size_t cap)
{
    static const char *DAYS[] = {"Sunday", "Monday", "Tuesday",     "Wednesday",
                                "Thursday", "Friday", "Saturday"};
    if (!d || cap == 0) {
        return;
    }
    out[0] = '\0';
    size_t n = 0;

    // The block has a FIXED SHAPE: the same eight lines, in the same order,
    // every day. Absence is rendered as the words "not reported" inside its own
    // line rather than by dropping the line.
    //
    // This is deliberate and it is the opposite of what an earlier version of
    // this function did. Deciding whether to print "wind" based on whether wind
    // was non-zero means the firmware is interpreting the numbers and choosing
    // what the model gets to see -- and inference about what the weather *means*
    // is the prompt writer's job, not this function's. A stable schema is also
    // the only way the device's block can be compared line for line against the
    // lab's, which is how the two are kept in step.

    // date
    if (d->day[0] != '\0') {
        int y = 0, m = 0, dom = 0;
        if (sscanf(d->day, "%4d-%2d-%2d", &y, &m, &dom) == 3 && m >= 1 && m <= 12 &&
            d->weekday >= 0 && d->weekday < 7) {
            n = block_line(out, cap, n, "date", "%s %d%s %s %d", DAYS[d->weekday], dom,
                           ordinal_suffix(dom), MONTHS[m - 1], y);
        } else {
            n = block_line(out, cap, n, "date", "%s", d->day);
        }
    } else {
        n = block_line(out, cap, n, "date", "not reported");
    }

    // condition -- the verbatim Open-Meteo text, with the raw code kept beside it.
    if (d->has_condition) {
        n = block_line(out, cap, n, "condition", "%s (wmo code %d)", d->condition, d->weather_code);
    } else if (d->has_code) {
        n = block_line(out, cap, n, "condition",
                       "not reported -- wmo code %d is undefined", d->weather_code);
    } else {
        n = block_line(out, cap, n, "condition", "not reported");
    }

    // temperature
    {
        char hi[24], lo[24];
        if (d->has_temp_max) {
            snprintf(hi, sizeof(hi), "%.1f \xc2\xb0""C", d->temp_max_c);
        } else {
            snprintf(hi, sizeof(hi), "not reported");
        }
        if (d->has_temp_min) {
            snprintf(lo, sizeof(lo), "%.1f \xc2\xb0""C", d->temp_min_c);
        } else {
            snprintf(lo, sizeof(lo), "not reported");
        }
        n = block_line(out, cap, n, "temperature", "high %s, low %s", hi, lo);
    }

    // cloud cover
    if (d->has_cloud) {
        n = block_line(out, cap, n, "cloud cover", "%d %% mean", d->cloud_cover_pct);
    } else {
        n = block_line(out, cap, n, "cloud cover", "not reported");
    }

    // sunshine against the daylight available. Hours rather than seconds so no
    // arithmetic is needed, and the share spelled out because dividing two
    // numbers is not what the writer is for.
    if (d->has_sunshine && d->has_daylight && d->daylight_s > 0.0) {
        int share = (int)((d->sunshine_s / d->daylight_s) * 100.0 + 0.5);
        n = block_line(out, cap, n, "sunshine", "%.1f h of %.1f h daylight (%d%%)",
                       d->sunshine_s / 3600.0, d->daylight_s / 3600.0, share);
    } else {
        n = block_line(out, cap, n, "sunshine", "not reported");
    }

    // precipitation -- zero is printed as a real zero. "0.0 mm over 0 h" says
    // something; a missing line says nothing, and the two are different days.
    if (d->has_precip_hours || d->precip_mm > 0.0) {
        n = block_line(out, cap, n, "precipitation", "%.1f mm over %d h", d->precip_mm,
                       d->precip_hours);
    } else {
        n = block_line(out, cap, n, "precipitation", "not reported");
    }

    // snowfall
    if (d->has_snow) {
        if (d->snow_cm == (double)(int)d->snow_cm) {
            n = block_line(out, cap, n, "snowfall", "%d cm", (int)d->snow_cm);
        } else {
            n = block_line(out, cap, n, "snowfall", "%.1f cm", d->snow_cm);
        }
    } else {
        n = block_line(out, cap, n, "snowfall", "not reported");
    }

    // wind
    if (d->wind_max_kmh > 0.0 || d->has_gust) {
        char gust[24];
        if (d->has_gust) {
            snprintf(gust, sizeof(gust), "%.1f km/h", d->gust_max_kmh);
        } else {
            snprintf(gust, sizeof(gust), "not reported");
        }
        n = block_line(out, cap, n, "wind", "max %.1f km/h, gusts to %s", d->wind_max_kmh, gust);
    } else {
        n = block_line(out, cap, n, "wind", "not reported");
    }

    // The lab's block ends without a trailing newline; trim it so a prompt
    // assembled from this does not gain a gap the lab would not have.
    if (n > 0 && out[n - 1] == '\n') {
        out[n - 1] = '\0';
    }
}

// "20th Nov | Clear sky". The month is abbreviated to three letters because the
// strip has to stay short on a 800px-wide panel, and the separator is a pipe
// rather than a dash so the date and the condition read as two fields.
//
// An unavailable condition is OMITTED, not named. "20th Nov | not reported" would
// put a data-quality message on the wall; the date alone is still a true caption,
// and the absence of a condition is the honest signal.
void weather_label_text(const weather_day_t *d, char *out, size_t cap)
{
    if (!d || cap == 0) {
        return;
    }
    out[0] = '\0';
    const char *what = d->has_condition ? d->condition : NULL;
    int y = 0, m = 0, dom = 0;
    bool dated = sscanf(d->day, "%4d-%2d-%2d", &y, &m, &dom) == 3 && m >= 1 && m <= 12;
    if (dated && what) {
        snprintf(out, cap, "%d%s %.3s | %s", dom, ordinal_suffix(dom), MONTHS[m - 1], what);
    } else if (dated) {
        snprintf(out, cap, "%d%s %.3s", dom, ordinal_suffix(dom), MONTHS[m - 1]);
    } else if (what) {
        snprintf(out, cap, "%s", what);
    }
}
