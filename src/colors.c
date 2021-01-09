#include "colors.h"

ColorRGB ColorRGB_from_xorg_rgb_specification(const char* string, bool* failed)
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
    float r = 0.0f, g = 0.0f, b = 0.0f;
    int   res = sscanf(string + (strstr(string, "rgbi:") ? 5 : 0), "%g/%g/%g", &r, &g, &b);
    if (failed && res != 3) {
        *failed = true;
    }
    return (ColorRGB){ .r = r * 255, .g = g * 255, .b = b * 255 };
}

ColorRGB ColorRGB_from_hex(const char* str, bool* failed)
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

ColorRGBA ColorRGBA_from_hex(const char* str, bool* failed)
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

static inline float color_component_gamma_correct(float val)
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

static inline float ColorRGB_get_relative_luminance(const ColorRGB* c)
{
    float r = color_component_gamma_correct(ColorRGB_get_float(*c, 0)),
          g = color_component_gamma_correct(ColorRGB_get_float(*c, 1)),
          b = color_component_gamma_correct(ColorRGB_get_float(*c, 2));
    return r * 0.2126 + g * 0.7152 + b * 0.0722;
}

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
