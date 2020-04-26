/* See LICENSE for license information. */

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

__attribute__((always_inline)) static inline bool ColorRGBA_eq(ColorRGBA a,
                                                               ColorRGBA b)
{
    return !memcmp(&a, &b, sizeof(ColorRGBA));
}

__attribute__((always_inline)) static inline bool ColorRGB_eq(ColorRGB a,
                                                              ColorRGB b)
{
    return !memcmp(&a, &b, sizeof(ColorRGB));
}

__attribute__((always_inline)) static inline uint8_t hex_char(char  c,
                                                              bool* failed)
{
    if (!((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
          (c >= '0' && c <= '9'))) {
        LOG("\'%c\' not a hex code\n", c);
        *failed = true;
        return 0;
    }

    return c >= 'A' ? (tolower(c) - 'a' + 10) : c - '0';
}

/* Generic utility functions */

__attribute__((always_inline)) static inline const char*
strstrn(const char* h, const char* n, const size_t h_len)
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

/**
 * get length of string up to max value */
__attribute__((always_inline)) static inline size_t strlen_max(const char* s,
                                                               size_t      max)
{
    size_t i;
    for (i = 0; *(s++) && i++ < max;)
        ;
    return i;
}

/**
 * parse hex string.
 * doesn't need to start with '#' or be NULL-terminated
 * @param[out] failed - is set ot 1 if input is invalid
 */
static ColorRGB ColorRGB_from_hex(const char* str, bool* failed)
{
    if (!str)
        goto fail;

    if (*str == '#')
        ++str;

    if (strlen_max(str, 7) < 6)
        goto fail;

    bool     f   = false;
    ColorRGB ret = {
        .r = (uint8_t)((hex_char(str[0], &f) << 4) + hex_char(str[1], &f)),
        .g = (uint8_t)((hex_char(str[2], &f) << 4) + hex_char(str[3], &f)),
        .b = (uint8_t)((hex_char(str[4], &f) << 4) + hex_char(str[5], &f)),
    };

    if (f)
        goto fail;
    else
        return ret;

fail:
    if (failed)
        *failed = true;
    return (ColorRGB){ 0, 0, 0 };
}

/**
 *parse hex string.
 * doesn't need to start with '#' or be NULL-terminated
 * @param[out] failed - is set ot 1 if input is invalid
 */
static ColorRGBA ColorGRBA_from_hex(const char* str, bool* failed)
{
    size_t len;
    bool   f = false;

    if (!str)
        goto fail;

    if (*str == '#')
        ++str;

    len = strlen_max(str, 10);

    if (len < 8)
        goto check_rgb;

    ColorRGBA ret = {
        .r = (uint8_t)((hex_char(str[0], &f) << 4) + hex_char(str[1], &f)),
        .g = (uint8_t)((hex_char(str[2], &f) << 4) + hex_char(str[3], &f)),
        .b = (uint8_t)((hex_char(str[4], &f) << 4) + hex_char(str[5], &f)),
        .a = (uint8_t)((hex_char(str[6], &f) << 4) + hex_char(str[7], &f)),
    };

    if (f)
        goto fail;
    else
        return ret;

check_rgb:;
    ColorRGB rgb = ColorRGB_from_hex(str, &f);
    if (!f)
        return (ColorRGBA){ .r = rgb.r, .g = rgb.g, .b = rgb.b, .a = 255 };

fail:
    if (failed)
        *failed = true;

    return (ColorRGBA){ 0, 0, 0, 0 };
}

__attribute__((always_inline)) static inline ColorRGB ColorRGB_from_RGBA(
  const ColorRGBA c)
{
    return (ColorRGB){
        .r = c.r,
        .g = c.g,
        .b = c.b,
    };
}

__attribute__((always_inline)) static inline ColorRGBA ColorRGBA_from_RGB(
  const ColorRGB c)
{
    return (ColorRGBA){
        .r = c.r,
        .g = c.g,
        .b = c.b,
        .a = 255,
    };
}

__attribute__((always_inline)) static inline float ColorRGB_get_float(
  const ColorRGB c,
  const size_t   idx)
{
    ASSERT(idx <= 2, "bad index");

    return (float)(&c.r)[idx] / 255;
}

__attribute__((always_inline)) static inline float ColorRGBA_get_float(
  ColorRGBA c,
  size_t    idx)
{
    ASSERT(idx <= 3, "bad index");

    return (float)(&c.r)[idx] / 255;
}

__attribute__((always_inline)) static inline float ColorRGB_get_float_blend(
  const ColorRGB c1,
  const ColorRGB c2,
  const double   factor,
  const size_t   idx)
{
    ASSERT(idx <= 2, "bad index");

    return ((float)(&c1.r)[idx] * (1.0 - factor) +
            (float)(&c2.r)[idx] * factor) /
           255;
}

__attribute__((always_inline)) static inline float ColorRGB_get_float_add(
  const ColorRGB c1,
  const ColorRGB c2,
  const double   factor,
  const size_t   idx)
{
    ASSERT(idx <= 2, "bad index");

    return MIN(1.0, (float)(&c1.r)[idx] * (1.0 - factor) +
                      (float)(&c2.r)[idx] * factor) /
           255;
}

__attribute__((always_inline)) static inline float ColorRGBA_get_float_blend(
  const ColorRGBA c1,
  const ColorRGBA c2,
  const double    factor,
  const size_t    idx)
{
    ASSERT(idx <= 3, "bad index");

    return ((float)(&c1.r)[idx] * (1.0 - factor) +
            (float)(&c2.r)[idx] * factor) /
           255;
}

__attribute__((always_inline)) static inline float ColorRGBA_get_float_add(
  const ColorRGBA c1,
  const ColorRGBA c2,
  const double    factor,
  const size_t    idx)
{
    ASSERT(idx <= 3, "bad index");

    return MIN(1.0, (float)(&c1.r)[idx] * (1.0 - factor) +
                      (float)(&c2.r)[idx] * factor) /
           255;
}
