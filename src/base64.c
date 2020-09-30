#include "base64.h"
#include "util.h"
#include <ctype.h>
#include <stdint.h>

static inline bool isbase64(char c)
{
    return isalnum(c) || c == '+' || c == '/' || c == '=';
}

ptrdiff_t base64_decode(const char* input, char* output) 
{
    static const char decode_table[] = { 62, 0,  0,  0,  63, 52, 53, 54, 55, 56, 57, 58, 59, 60,
                                         61, 0,  0,  0,  -1, 0,  0,  0,  0,  1,  2,  3,  4,  5,
                                         6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
                                         20, 21, 22, 23, 24, 25, 0,  0,  0,  0,  0,  0,  26, 27,
                                         28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
                                         42, 43, 44, 45, 46, 47, 48, 49, 50, 51 };
    char* dst = output;
    while (isbase64(*input)) {
        int b[4];
        for (uint_fast8_t i = 0; i < ARRAY_SIZE(b); ++i)
            b[i] = likely(*input) ? decode_table[*(input++) - 43] : -1;

        if (b[0] == -1 || b[1] == -1)
            break;
        *dst++ = (b[0] << 2) | ((b[1] & 0x30) >> 4);
        if (b[2] == -1)
            break;
        *dst++ = ((b[1] & 0x0f) << 4) | ((b[2] & 0x3c) >> 2);
        if (b[3] == -1)
            break;
        *dst++ = ((b[2] & 0x03) << 6) | b[3];
    }

    return dst - output;
}
