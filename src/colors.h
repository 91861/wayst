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
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Colors */
typedef struct
{
    uint8_t r, g, b;
} ColorRGB;

typedef struct
{
    uint8_t r, g, b, a;
} ColorRGBA;

#define COLOR_RGB_FMT   "rgb(%d, %d, %d)"
#define COLOR_RGB_AP(c) (c.r), (c.g), (c.b)

#define COLOR_RGBA_FMT   "rgb(%d, %d, %d, %f)"
#define COLOR_RGBA_AP(c) (c.r), (c.g), (c.b), (ColorRGBA_get_float(c, 3))

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
static ColorRGB ColorRGB_from_xorg_rgb_specification(const char* string, bool* failed)
{
    uint16_t    r = 0.0, g = 0.0, b = 0.0;
    const char* s      = NULL;
    char*       ss     = NULL;
    int         digits = 0;
    if (!string) {
        goto fail;
    } else if (strstr(string, "rgb:")) {
        s = string + 4;
    } else if (!isxdigit(*string) || strlen(string) < 5) {
        goto fail;
    } else {
        s = string;
    }

    r      = strtol(s, &ss, 16);
    digits = ss - s;
    s      = ++ss;
    if (digits < 1 || digits > 4) {
        goto fail;
    }
    r = scale_to_8bit_color_value(r, digits);

    g      = strtol(s, &ss, 16);
    digits = ss - s;
    s      = ++ss;
    if (digits < 1 || digits > 4) {
        goto fail;
    }
    g = scale_to_8bit_color_value(g, digits);

    b      = strtol(s, &ss, 16);
    digits = ss - s;
    if (digits < 1 || digits > 4) {
        goto fail;
    }
    b = scale_to_8bit_color_value(b, digits);

    return (ColorRGB){ .r = r, .g = g, .b = b };

fail:
    if (failed) {
        *failed = true;
    }
    return (ColorRGB){ 0 };
}

static ColorRGB ColorRGB_from_xorg_old_rgb_specification(const char* string, bool* failed)
{
    const char* s = NULL;
    uint16_t    r = 0.0, g = 0.0, b = 0.0;
    char        tmp[5] = { 0 };

    if (!string) {
        goto fail;
    } else if (*string == '#') {
        s = string + 1;
    } else if (!isxdigit(*string)) {
        goto fail;
    } else {
        s = string;
    }
    uint32_t digits = strlen(s);

    if (digits % 3 || digits < 3 || digits > 12) {
        goto fail;
    }

    digits /= 3;

    memcpy(tmp, s, digits);
    r = scale_to_8bit_color_value(strtol(tmp, NULL, 16), digits);
    s += digits;
    memcpy(tmp, s, digits);
    g = scale_to_8bit_color_value(strtol(tmp, NULL, 16), digits);
    s += digits;
    memcpy(tmp, s, digits);
    b = scale_to_8bit_color_value(strtol(tmp, NULL, 16), digits);

    return (ColorRGB){ .r = r, .g = g, .b = b };

fail:
    if (failed) {
        *failed = true;
    }
    return (ColorRGB){ 0 };
}

static ColorRGB ColorRGB_from_xorg_rgb_intensity_specification(const char* string, bool* failed)
{
    float r = 0.0f, g = 0.0f, b = 0.0f;
    int   res = sscanf(string + (strstr(string, "rgbi:") ? 5 : 0), "%g/%g/%g", &r, &g, &b);
    if (failed && res != 3) {
        *failed = true;
    }
    return (ColorRGB){ .r = r * 255, .g = g * 255, .b = b * 255 };
}

/**
 * parse hex string.
 * doesn't need to start with '#' or be NULL-terminated
 * @param[out] failed - is set ot 1 if input is invalid
 */
static ColorRGB ColorRGB_from_hex(const char* str, bool* failed)
{
    if (!str) {
        goto fail;
    }
    if (*str == '#') {
        ++str;
    }
    if (strnlen(str, 7) < 6) {
        goto fail;
    }
    bool     f   = false;
    ColorRGB ret = {
        .r = (uint8_t)((hex_char(str[0], &f) << 4) + hex_char(str[1], &f)),
        .g = (uint8_t)((hex_char(str[2], &f) << 4) + hex_char(str[3], &f)),
        .b = (uint8_t)((hex_char(str[4], &f) << 4) + hex_char(str[5], &f)),
    };
    if (f) {
        goto fail;
    } else {
        return ret;
    }

fail:
    if (failed) {
        *failed = true;
    }
    return (ColorRGB){ 0, 0, 0 };
}

/**
 *parse hex string.
 * doesn't need to start with '#' or be NULL-terminated
 * @param[out] failed - is set ot 1 if input is invalid
 */
static ColorRGBA ColorRGBA_from_hex(const char* str, bool* failed)
{
    bool f = false;
    if (!str) {
        goto fail;
    }
    if (*str == '#') {
        ++str;
    }
    if (strnlen(str, 8) < 8) {
        goto check_rgb;
    }
    ColorRGBA ret = {
        .r = (uint8_t)((hex_char(str[0], &f) << 4) + hex_char(str[1], &f)),
        .g = (uint8_t)((hex_char(str[2], &f) << 4) + hex_char(str[3], &f)),
        .b = (uint8_t)((hex_char(str[4], &f) << 4) + hex_char(str[5], &f)),
        .a = (uint8_t)((hex_char(str[6], &f) << 4) + hex_char(str[7], &f)),
    };
    if (f) {
        goto fail;
    } else {
        return ret;
    }

check_rgb:;
    ColorRGB rgb = ColorRGB_from_hex(str, &f);
    if (!f) {
        return (ColorRGBA){ .r = rgb.r, .g = rgb.g, .b = rgb.b, .a = 255 };
    }

fail:
    if (failed) {
        *failed = true;
    }
    return (ColorRGBA){ 0, 0, 0, 0 };
}

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

static inline float ColorRGB_get_hue(const ColorRGB c)
{
    uint8_t max = MAX(c.r, MAX(c.g, c.b)), min = MIN(c.r, MIN(c.g, c.b));

    if (min == max) {
        return 0.0f;
    }

    float hue;
    if (c.r == max) {
        hue = (float)(c.g - c.b);
    } else if (c.g == max) {
        hue = 2.0f + (float)(c.b - c.r) / (max - min);
    } else {
        hue = 4.0f + (float)(c.r - c.g) / (max - min);
    }
    hue *= 60.0f;
    return hue > 0.0f ? hue : hue + 360.0f;
}

static inline float ColorRGB_get_lightness(const ColorRGB c)
{
    uint8_t max = MAX(c.r, MAX(c.g, c.b)), min = MIN(c.r, MIN(c.g, c.b));
    return (float)(max + min) / 255.0f / 2.0f;
}

static inline float ColorRGB_get_saturation(const ColorRGB c)
{
    uint8_t max = MAX(c.r, MAX(c.g, c.b)), min = MIN(c.r, MIN(c.g, c.b));
    uint8_t delta = max - min;

    if (!delta)
        return 0.0f;

    float lightness = (float)(max + min) / 2.0f;
    return ((float)delta / 255.0f) / (1 - ABS(2 * lightness - 1.0));
}
