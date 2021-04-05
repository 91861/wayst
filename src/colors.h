/* See LICENSE for license information. */

/**
 * Utility functions for dealing with colors
 *
 * TODO:
 * - Some way to test if given bg-fg combo is readable and a way to make it so
 */

#pragma once

#include "util.h"
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <threads.h>

/* Colors */
typedef struct
{
    uint8_t r, g, b;
} ColorRGB;

typedef struct
{
    uint8_t r, g, b, a;
} ColorRGBA;

#define RELATIVE_LUMINANCE_BRIGHT_COLOR_TRESHOLD 0.04

#define COLOR_RGB_FMT   "rgb(%d, %d, %d)"
#define COLOR_RGB_AP(c) (c.r), (c.g), (c.b)

#define COLOR_RGBA_FMT   "rgb(%d, %d, %d, %f)"
#define COLOR_RGBA_AP(c) (c.r), (c.g), (c.b), (ColorRGBA_get_float(c, 3))

static char* termcolor_fg_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    _Thread_local static char _termcolorbuf[64];
    sprintf(_termcolorbuf, "\e[38;2;%u;%u;%um", r, g, b);
    return _termcolorbuf;
}

static char* termcolor_bg_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    _Thread_local static char _termcolorbuf[64];
    sprintf(_termcolorbuf, "\e[48;2;%u;%u;%um", r, g, b);
    return _termcolorbuf;
}

static inline bool ColorRGBA_eq(ColorRGBA a, ColorRGBA b)
{
    return !memcmp(&a, &b, sizeof(ColorRGBA));
}

static inline bool ColorRGB_eq(ColorRGB a, ColorRGB b)
{
    return !memcmp(&a, &b, sizeof(ColorRGB));
}

static inline uint8_t hex_char(char c, bool* failed)
{
    if (!isxdigit(c)) {
        LOG("\'%c\' not a hex digit\n", c);
        *failed = true;
        return 0;
    }

    return c >= 'A' ? (tolower(c) - 'a' + 10) : c - '0';
}

/* General utility functions */

static inline const char* strstrn(const char* h, const char* n, const size_t h_len)
{
    for (size_t i = 0; i < h_len; ++i) {

        if (h[i] == *n) {

            for (size_t sub_i = 0; n[sub_i]; ++sub_i) {
                if (i + sub_i >= h_len)
                    return NULL;

                if (h[i + sub_i] != n[sub_i])
                    goto continue_outer;
            }
            return &h[i];
        }
    continue_outer:;
    }

    return NULL;
}

static inline int max_value_from_number_of_hex_digits(int digits)
{
    ASSERT(digits > 0, "is positive");

    static const int values[] = {
        0xf, 0xff, 0xfff, 0xffff, 0xfffff, 0xffffff, 0xfffffff,
    };
    return values[digits - 1];
}

static inline int scale_to_8bit_color_value(int value, uint8_t digits)
{
    if (digits == 2) {
        return value;
    } else {
        int64_t tmp = value;
        return tmp / max_value_from_number_of_hex_digits(digits) * 0xff;
    }
}

/** Parse a color from an RGB Device specification string as XParseColor()
 * The string should start with 'rgb:' or with a digit
 *
 * An RGB Device specification is identified by the prefix ''rgb:'' and conforms to the following
 * syntax: rgb:<red>/<green>/<blue>
 *    <red>, <green>, <blue> := h | hh | hhh | hhhh
 *    h := single hexadecimal digits (case insignificant)
 *
 * Note that h indicates the value scaled in 4 bits, hh the value scaled in 8 bits, hhh the value
 * scaled in 12 bits, and hhhh the value scaled in 16 bits, respectively. */
ColorRGB ColorRGB_from_xorg_rgb_specification(const char* string, bool* failed);

ColorRGB ColorRGB_from_xorg_old_rgb_specification(const char* string, bool* failed);

ColorRGB ColorRGB_from_xorg_rgb_intensity_specification(const char* string, bool* failed);

/**
 * parse hex string.
 * doesn't need to start with '#' or be NULL-terminated
 * @param[out] failed - is set ot 1 if input is invalid
 */
ColorRGB ColorRGB_from_hex(const char* str, bool* failed);

/**
 *parse hex string.
 * doesn't need to start with '#' or be NULL-terminated
 * @param[out] failed - is set ot 1 if input is invalid
 */
ColorRGBA ColorRGBA_from_hex(const char* str, bool* failed);

static inline ColorRGB ColorRGB_from_RGBA(const ColorRGBA c)
{
    return (ColorRGB){
        .r = c.r,
        .g = c.g,
        .b = c.b,
    };
}

static inline ColorRGBA ColorRGBA_from_RGB(const ColorRGB c)
{
    return (ColorRGBA){
        .r = c.r,
        .g = c.g,
        .b = c.b,
        .a = 255,
    };
}

static ColorRGB ColorRGB_new_from_blend(ColorRGB base, ColorRGB blend, float factor)
{
    return (ColorRGB){
        .r = CLAMP(base.r * (1.0f - factor) + factor * blend.r, 0, 255),
        .g = CLAMP(base.g * (1.0f - factor) + factor * blend.g, 0, 255),
        .b = CLAMP(base.b * (1.0f - factor) + factor * blend.b, 0, 255),
    };
}

static double _hue_to_color_component(double p, double q, double t)
{
    if (t < 0) {
        t += 1;
    } else if (t > 1) {
        t -= 1;
    }

    if (t < 1 / 6.0) {
        return p + (q - p) * 6 * t;
    } else if (t < 1 / 2.0) {
        return q;
    } else if (t < 2 / 3.0) {
        return p + (q - p) * (2 / 3.0 - t) * 6;
    } else {
        return p;
    }
}

static ColorRGB ColorRGB_new_from_hsl(float h, float s, float l)
{
    if (s == 0.0) {
        return (ColorRGB){ .r = l, .g = l, .b = l };
    } else {
        double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
        double p = 2 * l - q;
        return (ColorRGB){
            .r = _hue_to_color_component(p, q, h + 1.0 / 3.0) * 255,
            .g = _hue_to_color_component(p, q, h) * 255,
            .b = _hue_to_color_component(p, q, h - 1.0 / 3.0) * 255,
        };
    }
}

static inline float ColorRGB_get_float(const ColorRGB c, const size_t idx)
{
    ASSERT(idx <= 2, "bad index");
    return (float)(&c.r)[idx] / 255;
}

static inline float ColorRGBA_get_float(ColorRGBA c, size_t idx)
{
    ASSERT(idx <= 3, "bad index");
    return (float)(&c.r)[idx] / 255;
}

static inline float ColorRGB_get_float_blend(const ColorRGB c1,
                                             const ColorRGB c2,
                                             const double   factor,
                                             const size_t   idx)
{
    ASSERT(idx <= 2, "bad index");
    return ((float)(&c1.r)[idx] * (1.0 - factor) + (float)(&c2.r)[idx] * factor) / 255;
}

static inline float ColorRGB_get_float_add(const ColorRGB c1,
                                           const ColorRGB c2,
                                           const double   factor,
                                           const size_t   idx)
{
    ASSERT(idx <= 2, "bad index");
    return MIN(1.0, (float)(&c1.r)[idx] * (1.0 - factor) + (float)(&c2.r)[idx] * factor) / 255;
}

static inline float ColorRGBA_get_float_blend(const ColorRGBA c1,
                                              const ColorRGBA c2,
                                              const double    factor,
                                              const size_t    idx)
{
    ASSERT(idx <= 3, "bad index");
    return ((float)(&c1.r)[idx] * (1.0 - factor) + (float)(&c2.r)[idx] * factor) / 255;
}

static inline float ColorRGBA_get_float_add(const ColorRGBA c1,
                                            const ColorRGBA c2,
                                            const double    factor,
                                            const size_t    idx)
{
    ASSERT(idx <= 3, "bad index");
    return MIN(1.0, (float)(&c1.r)[idx] * (1.0 - factor) + (float)(&c2.r)[idx] * factor) / 255;
}

static inline float ColorRGB_get_luma(const ColorRGB c)
{
    return ColorRGB_get_float(c, 0) * 0.2126 + ColorRGB_get_float(c, 1) * 0.7152 +
           ColorRGB_get_float(c, 2) * 0.0722;
}

float ColorRGB_get_hue(const ColorRGB c);

static inline float ColorRGB_get_lightness(const ColorRGB c)
{
    uint8_t max = MAX(c.r, MAX(c.g, c.b)), min = MIN(c.r, MIN(c.g, c.b));
    return (float)(max + min) / 255.0f / 2.0f;
}

static inline float ColorRGB_get_saturation(const ColorRGB c)
{
    uint8_t max = MAX(c.r, MAX(c.g, c.b)), min = MIN(c.r, MIN(c.g, c.b));
    uint8_t delta = max - min;

    if (!delta) {
        return 0.0f;
    }

    float lightness = (float)(max + min) / 2.0f;
    return ((float)delta / 255.0f) / (1 - ABS(2 * lightness - 1.0));
}

float color_component_gamma_correct(float val);

static inline float ColorRGB_get_relative_luminance(const ColorRGB* c)
{
    float r = color_component_gamma_correct(ColorRGB_get_float(*c, 0)),
          g = color_component_gamma_correct(ColorRGB_get_float(*c, 1)),
          b = color_component_gamma_correct(ColorRGB_get_float(*c, 2));
    return r * 0.2126 + g * 0.7152 + b * 0.0722;
}

float ColorRGB_get_readability_WCAG(const ColorRGB* color1, const ColorRGB* color2);

bool ColorRGB_is_readable_WCAG(const ColorRGB* color1, const ColorRGB* color2);

_Thread_local static char _ColorRGB_term_string[128];
static char*              ColorRGB_to_term_string(ColorRGB color)
{
    uint8_t fg_cmp = ColorRGB_get_luma(color) < 0.5 ? 255 : 0;
    char*   t_bg   = termcolor_bg_rgb(color.r, color.g, color.b);
    size_t  off    = strlen(t_bg);
    memccpy(_ColorRGB_term_string, t_bg, 1, off);
    off += snprintf(_ColorRGB_term_string + off,
                    ARRAY_SIZE(_ColorRGB_term_string) - off,
                    "%s",
                    termcolor_fg_rgb(fg_cmp, fg_cmp, fg_cmp));
    snprintf(_ColorRGB_term_string + off,
             ARRAY_SIZE(_ColorRGB_term_string) - off,
             COLOR_RGB_FMT,
             COLOR_RGB_AP(color));
    return _ColorRGB_term_string;
}
