#include "colors.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


ColorRGB ColorRGB_from_any(const char* string, bool* failed)
{
    bool      f = false;
    ColorRGB c = { 0 };

    if (*string == '#') {
        c = ColorRGB_from_hex(string, &f);
        if (f) {
            f = false;
            c = ColorRGB_from_xorg_old_rgb_specification(string, &f);
        }
    } else if (strstr(string, "rgbi:")) {
        c = ColorRGB_from_xorg_rgb_intensity_specification(string, &f);
    } else if (strstr(string, "rgb:")) {
        c = ColorRGB_from_xorg_rgb_specification(string, &f);
    } else if (strstr(string, "rgb(")) {
        c = ColorRGB_from_rgb_paren(string, &f);
    } else if (strstr(string, "hsl(")) {
        c = ColorRGB_from_hsl_paren(string, &f);
    } else {
        c = ColorRGB_from_hex(string, &f);

        if (f) {
            f = false;
            c = ColorRGB_from_xorg_rgb_specification(string, &f);
        }

        if (f) {
            f = false;
            c = ColorRGB_from_xorg_rgb_intensity_specification(string, &f);
        }
    }

    if (f && failed) {
        *failed = true;
    }

    return c;
}

ColorRGBA ColorRGBA_from_any(const char* string, bool* failed)
{
    bool      f = false;
    ColorRGBA c = { 0 };

    if (*string == '#') {
        c = ColorRGBA_from_hex(string, &f);
        if (f) {
            f = false;
            c = ColorRGBA_from_RGB(ColorRGB_from_xorg_old_rgb_specification(string, &f));
        }
    } else if (strstr(string, "rgbi:")) {
        c = ColorRGBA_from_RGB(ColorRGB_from_xorg_rgb_intensity_specification(string, &f));
    } else if (strstr(string, "rgb:")) {
        c = ColorRGBA_from_RGB(ColorRGB_from_xorg_rgb_specification(string, &f));
    } else if (strstr(string, "rgb(")) {
        c = ColorRGBA_from_RGB(ColorRGB_from_rgb_paren(string, &f));
    } else if (strstr(string, "rgba(")) {
        c = ColorRGBA_from_rgba_paren(string, &f);
    } else if (strstr(string, "hsl(")) {
        c = ColorRGBA_from_RGB(ColorRGB_from_hsl_paren(string, &f));
    } else if (strstr(string, "hsla(")) {
        c = ColorRGBA_from_hsla_paren(string, &f);
        // TODO: } else if (strstr(string, "hwb(")) {
        //    c = ColorRGBA_from_RGB(ColorRGB_from_hwb_paren(string, &f));
    } else {
        c = ColorRGBA_from_hex(string, &f);

        if (f) {
            f = false;
            c = ColorRGBA_from_RGB(ColorRGB_from_xorg_rgb_specification(string, &f));
        }

        if (f) {
            f = false;
            c = ColorRGBA_from_RGB(ColorRGB_from_xorg_rgb_intensity_specification(string, &f));
        }
    }

    if (f && failed) {
        *failed = true;
    }

    return c;
}

ColorRGB ColorRGB_from_xorg_rgb_specification(const char* string, bool* failed)
{
    uint16_t    r = 0.0, g = 0.0, b = 0.0;
    const char* s      = NULL;
    char*       ss     = NULL;
    int         digits = 0;

    if (!string) {
        goto fail;
    } else if (strstr(string, "rgb:") == string) {
        s = string + 4;
    } else if (!isxdigit(*string) || strlen(string) < 5) {
        goto fail;
    } else {
        s = string;
    }

    r      = strtol(s, &ss, 16);
    digits = ss - s;
    if (*ss != '/') {
        goto fail;
    }
    s = ++ss;
    if (digits < 1 || digits > 4) {
        goto fail;
    }
    r = scale_to_8bit_color_value(r, digits);

    g      = strtol(s, &ss, 16);
    digits = ss - s;
    if (*ss != '/') {
        goto fail;
    }
    s = ++ss;
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

ColorRGB ColorRGB_from_xorg_old_rgb_specification(const char* string, bool* failed)
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

ColorRGB ColorRGB_from_xorg_rgb_intensity_specification(const char* string, bool* failed)
{
    float vals[3] = { 0.0f, 0.0f, 0.0f };

    if (strstr(string, "rgbi:") == string)
        string += 5;
    while (*string == ' ')
        ++string;
    if (!isdigit(*string))
        goto fail;

    char* s = (char*)string;
    for (int i = 0; i < 3; ++i) {
        int   valsize = strcspn(s, "/");
        char* endptr  = NULL;
        if (!*s)
            goto fail;

        vals[i] = strtof(s, &endptr);

        if (!endptr || (endptr == s))
            goto fail;
        s += valsize;
        while (*s == ',' || *s == ' ' || *s == '/')
            s++;
    }

    return (ColorRGB){ .r = vals[0] * 255, .g = vals[1] * 255, .b = vals[2] * 255 };

fail:
    *failed = true;
    return (ColorRGB){ .r = 0, .g = 0, .b = 0 };
}

ColorRGB ColorRGB_from_hex(const char* str, bool* failed)
{
    if (!str) {
        goto fail;
    }

    if (*str == '#' || *str == ' ') {
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

ColorRGBA ColorRGBA_from_hex(const char* str, bool* failed)
{
    bool f = false;

    if (!str) {
        goto fail;
    }

    if (*str == '#' || *str == ' ') {
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

static uint8_t get_next_paren_value(const char* string, int valsize, bool* failed)
{
    const char* sto_ps  = strstr(string, "%");
    const char* sto_dot = strstr(string, ".");
    int         to_ps   = sto_ps ? sto_ps - string : 0;
    int         to_dot  = sto_dot ? sto_dot - string : 0;

    if (to_ps <= valsize && to_ps) {
        char*  ep  = NULL;
        double val = strtod(string, &ep);
        if (!ep) {
            *failed = true;
            return 0;
        }
        return CLAMP(val / 100.0, 0.0, 1.0) * UINT8_MAX;
    } else if (to_dot <= valsize && to_dot) {
        char*  ep  = NULL;
        double val = strtod(string, &ep);
        if (!ep) {
            *failed = true;
            return 0;
        }
        return CLAMP(val, 0.0, 1.0) * UINT8_MAX;
    } else {
        char*         ep  = NULL;
        unsigned long val = strtoul(string, &ep, 10);
        if (!ep) {
            *failed = true;
            return 0;
        }
        return MIN(val, UINT8_MAX);
    }
}

static float get_next_paren_value_float(const char* string, int valsize, bool* failed)
{
    const char* sto_ps   = strstr(string, "%");
    const char* sto_deg  = strstr(string, "deg");
    const char* sto_rad  = strstr(string, "rad");
    const char* sto_grad = strstr(string, "grad");
    int         to_ps    = sto_ps ? sto_ps - string : 0;
    int         to_deg   = sto_deg ? sto_deg - string : 0;
    int         to_rad   = sto_rad ? sto_rad - string : 0;
    int         to_grad  = sto_grad ? sto_grad - string : 0;

    if (to_ps <= valsize && to_ps) {
        char*  ep  = NULL;
        double val = strtod(string, &ep);
        if (!ep) {
            *failed = true;
            return 0;
        }
        return CLAMP(val / 100.0, 0.0, 1.0);
    } else if (to_deg <= valsize) {
        char*  ep  = NULL;
        double val = strtod(string, &ep);
        if (!ep) {
            *failed = true;
            return 0;
        }
        while (val < 0.0)
            val += 360;
        while (val > 360)
            val -= 360;

        return CLAMP(val / 360.0, 0.0, 1.0);
    } else if (to_rad <= valsize) {
        char*  ep  = NULL;
        double val = strtod(string, &ep);
        if (!ep) {
            *failed = true;
            return 0;
        }
        while (val < 0.0)
            val += (2.0 * M_PI);
        while (val > (2.0 * M_PI))
            val -= (2.0 * M_PI);
        return CLAMP(val / (2.0 * M_PI), 0.0, 1.0);
    } else if (to_grad <= valsize) {
        char*  ep  = NULL;
        double val = strtod(string, &ep);
        if (!ep) {
            *failed = true;
            return 0;
        }
        while (val < 0.0)
            val += 400.0;
        while (val > 400.0)
            val -= 400.0;
        return CLAMP(val / 400.0, 0.0, 1.0);
    } else {
        char*  ep  = NULL;
        double val = strtod(string, &ep);
        if (!ep) {
            *failed = true;
            return 0;
        }
        return CLAMP(val, 0.0, 1.0);
    }
}

ColorRGB ColorRGB_from_rgb_paren(const char* str, bool* failed)
{
    if (!str) {
        goto fail;
    }

    while (*str == ' ')
        ++str;

    if (strstr(str, "rgb(")) {
        str += 4;
        if (!strstr(str, ")")) {
            goto fail;
        }
    } else if (!isdigit(*str)) {
        goto fail;
    }

    ColorRGB clr;
    uint8_t* vals = (uint8_t*)&clr;

    const char* s = str;
    for (int i = 0; i < 3; ++i) {
        bool f       = false;
        int  valsize = strcspn(s, ", /");
        vals[i]      = get_next_paren_value(s, valsize, &f);
        if (f || !valsize)
            goto fail;
        s += valsize;
        while (*s == ',' || *s == ' ' || *s == '/')
            s++;
    }
    return clr;

fail:
    if (failed) {
        *failed = true;
    }
    return (ColorRGB){ 0, 0, 0 };
}

ColorRGBA ColorRGBA_from_rgba_paren(const char* str, bool* failed)
{
    if (!str) {
        goto fail;
    }

    while (*str == ' ')
        ++str;

    if (strstr(str, "rgba(")) {
        str += 5;
        if (!strstr(str, ")")) {
            goto fail;
        }
    } else if (!isdigit(*str)) {
        goto fail;
    }

    ColorRGBA clr;
    uint8_t*  vals = (uint8_t*)&clr;

    const char* s = str;
    for (int i = 0; i < 4; ++i) {
        bool f       = false;
        int  valsize = strcspn(s, ", /");
        vals[i]      = get_next_paren_value(s, valsize, &f);
        if (f || !valsize)
            goto fail;
        s += valsize;
        while (*s == ',' || *s == ' ' || *s == '/')
            s++;
    }
    return clr;

fail:
    if (failed) {
        *failed = true;
    }
    return (ColorRGBA){ 0, 0, 0, 0 };
}

ColorRGB ColorRGB_from_hsl_paren(const char* str, bool* failed)
{
    if (!str) {
        goto fail;
    }

    while (*str == ' ')
        ++str;

    if (strstr(str, "hsl(")) {
        str += 4;
        if (!strstr(str, ")")) {
            goto fail;
        }
    } else if (!isdigit(*str)) {
        goto fail;
    }

    float vals[3];

    const char* s = str;
    for (int i = 0; i < 3; ++i) {
        bool f       = false;
        int  valsize = strcspn(s, ", /");
        vals[i]      = get_next_paren_value_float(s, valsize, &f);
        if (f || !valsize)
            goto fail;
        s += valsize;
        while (*s == ',' || *s == ' ' || *s == '/')
            s++;
    }
    return ColorRGB_new_from_hsl(vals[0], vals[1], vals[2]);

fail:
    if (failed) {
        *failed = true;
    }
    return (ColorRGB){ 0, 0, 0 };
}

ColorRGBA ColorRGBA_from_hsla_paren(const char* str, bool* failed)
{
    if (!str) {
        goto fail;
    }

    while (*str == ' ')
        ++str;

    if (strstr(str, "hsla(")) {
        str += 5;
        if (!strstr(str, ")")) {
            goto fail;
        }
    } else if (!isdigit(*str)) {
        goto fail;
    }

    float vals[4];

    const char* s = str;
    for (int i = 0; i < 4; ++i) {
        bool f       = false;
        int  valsize = strcspn(s, ", /");
        vals[i]      = get_next_paren_value_float(s, valsize, &f);
        if (f || !valsize)
            goto fail;
        s += valsize;
        while (*s == ',' || *s == ' ' || *s == '/')
            s++;
    }
    return ColorRGBA_new_from_hsla(vals[0], vals[1], vals[2], vals[3]);

fail:
    if (failed) {
        *failed = true;
    }
    return (ColorRGBA){ 0, 0, 0, 0 };
}

float ColorRGB_get_hue(const ColorRGB c)
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

float color_component_gamma_correct(float val)
{
    if (val < 0.03938f) {
        val /= 12.92;
    } else {
        val += 0.055f;
        val /= 1.055f;
        val = powf(val, 2.4);
    }
    return val;
}

/* https://www.w3.org/TR/WCAG20/#relativeluminancedef */
float ColorRGB_get_readability_WCAG(const ColorRGB* color1, const ColorRGB* color2)
{
    float l1 = ColorRGB_get_relative_luminance(color1),
          l2 = ColorRGB_get_relative_luminance(color2);
    float lo = MIN(l1, l2), hi = MAX(l1, l2);
    return (lo + 0.05) / (hi + 0.05);
}

bool ColorRGB_is_readable_WCAG(const ColorRGB* color1, const ColorRGB* color2)
{
    return ColorRGB_get_readability_WCAG(color1, color2) > 3.0;
}
